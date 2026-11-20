#[cfg(any(not(target_os = "macos"), test))]
use anyhow::Context;
#[cfg(any(target_os = "macos", test))]
use gpui_updater_core::DriverEvent;
use gpui_updater_core::Release;
#[cfg(not(target_os = "macos"))]
use gpui_updater_core::{
    Artifact, EngineConfig, OwnershipHints, StaticManifestSource, UpdateEngine, Verification,
};
#[cfg(target_os = "macos")]
use gpui_updater_core::{DriverEvents, UpdateDriver};
#[cfg(target_os = "macos")]
use gpui_updater_sparkle::{SparkleDriver, SparkleOptions, StagedUpdatePolicy};
use std::path::PathBuf;
#[cfg(target_os = "macos")]
use std::{cell::RefCell, rc::Rc};

#[cfg(not(target_os = "macos"))]
const FEED: &str =
    "https://github.com/lumirth/stickwalker/releases/download/desktop-feed/latest.json";
#[derive(Clone, Debug)]
pub enum AppUpdate {
    Unsupported,
    Idle,
    Checking {
        background: bool,
    },
    UpToDate,
    Available(Box<Release>),
    Downloading {
        percent: Option<u8>,
    },
    #[cfg(not(target_os = "macos"))]
    ReadyToInstall(Artifact),
    Installing,
    Restarting,
    ReadyToRestart {
        path: Option<PathBuf>,
    },
    Failed(String),
}

impl AppUpdate {
    #[cfg(not(target_os = "macos"))]
    fn setup_error(error: anyhow::Error) -> Self {
        eprintln!("Desktop update setup failed: {error:#}");
        Self::Failed(error.to_string())
    }

    fn failed(message: &str, error: impl std::fmt::Display) -> Self {
        eprintln!("{message} {error:#}");
        Self::Failed(message.into())
    }
}

pub enum UpdateEvent {
    #[cfg(any(target_os = "macos", test))]
    Driver(Box<DriverEvent>),
    #[cfg(any(not(target_os = "macos"), debug_assertions))]
    Native(AppUpdate),
}

enum UpdateBackend {
    #[cfg(target_os = "macos")]
    Sparkle(Rc<RefCell<SparkleDriver>>),
    #[cfg(not(target_os = "macos"))]
    Native(async_channel::Sender<UpdateEvent>),
    Unavailable,
}

pub struct Updates {
    backend: UpdateBackend,
    pub events: async_channel::Receiver<UpdateEvent>,
    pub state: AppUpdate,
}

#[cfg(not(target_os = "macos"))]
fn engine() -> anyhow::Result<UpdateEngine<StaticManifestSource>> {
    let key = option_env!("STICKWALKER_UPDATE_KEY")
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| anyhow::anyhow!("This development build has no signed app update feed."))?;
    let source = StaticManifestSource::new(option_env!("STICKWALKER_APP_FEED").unwrap_or(FEED))
        .os(std::env::consts::OS)
        .arch(std::env::consts::ARCH);
    let config = EngineConfig::new(env!("CARGO_PKG_VERSION").parse()?)
        .ed25519_public_key(key)
        .verification(Verification::Strict);
    let exe = gpui_updater_core::install::current_install_root()
        .context("Couldn’t locate Stickwalker Desktop.")?;
    let ownership = gpui_updater_core::install::ownership(&exe, &OwnershipHints::new());
    anyhow::ensure!(
        ownership.is_self_update(),
        "Update with your package manager. {}",
        ownership.update_command().unwrap_or_default()
    );
    #[cfg(target_os = "linux")]
    let config = config.install_root(linux_bundle_root(&exe)?);
    let engine = UpdateEngine::new(source, config);
    Ok(engine)
}
#[cfg(any(target_os = "linux", test))]
fn linux_bundle_root(exe: &std::path::Path) -> anyhow::Result<&std::path::Path> {
    let root = exe
        .parent()
        .ok_or_else(|| anyhow::anyhow!("Couldn’t locate Stickwalker Desktop."))?;
    let expected = [
        "stickwalker",
        "icon.png",
        "60-stickwalker.rules",
        "install-launcher.py",
        concat!(env!("STICKWALKER_APP_ID"), ".desktop"),
    ];
    // The Linux installer atomically replaces this entire directory.
    anyhow::ensure!(
        expected.iter().all(|name| root.join(name).is_file()),
        "Extract the full Stickwalker folder to update."
    );
    for entry in std::fs::read_dir(root).context("Couldn’t read the Stickwalker folder.")? {
        let entry = entry.context("Couldn’t read the Stickwalker folder.")?;
        let name = entry.file_name();
        if expected.iter().any(|file| name == *file)
            || (name == ".directory" && entry.file_type()?.is_file())
        {
            continue;
        }
        if name == "__pycache__" && entry.file_type()?.is_dir() {
            for child in std::fs::read_dir(entry.path())? {
                let child = child?;
                anyhow::ensure!(
                    child.file_type()?.is_file()
                        && child.path().extension().is_some_and(|ext| ext == "pyc"),
                    "Move “__pycache__/{}” out of the Stickwalker folder to update.",
                    child.file_name().to_string_lossy()
                );
            }
            continue;
        }
        anyhow::bail!(
            "Move “{}” out of the Stickwalker folder to update.",
            name.to_string_lossy()
        );
    }
    Ok(root)
}

impl Updates {
    pub fn new() -> Self {
        let (tx, events) = async_channel::unbounded();
        #[cfg(target_os = "macos")]
        let (backend, state) = match SparkleDriver::load(
            SparkleOptions::new().staged_updates(StagedUpdatePolicy::HostRelaunches),
        ) {
            Ok(Some(mut driver)) => {
                driver.start(DriverEvents::new(move |event| {
                    let _ = tx.try_send(UpdateEvent::Driver(Box::new(event)));
                }));
                driver.set_automatically_checks_for_updates(false);
                driver.set_automatically_downloads_updates(false);
                (
                    UpdateBackend::Sparkle(Rc::new(RefCell::new(driver))),
                    AppUpdate::Idle,
                )
            }
            Ok(None) => (UpdateBackend::Unavailable, AppUpdate::Unsupported),
            Err(error) => (
                UpdateBackend::Unavailable,
                AppUpdate::failed("Couldn’t start app updates.", error),
            ),
        };
        #[cfg(not(target_os = "macos"))]
        let (backend, state) =
            if option_env!("STICKWALKER_UPDATE_KEY").is_some_and(|k| !k.trim().is_empty()) {
                (UpdateBackend::Native(tx), AppUpdate::Idle)
            } else {
                (UpdateBackend::Unavailable, AppUpdate::Unsupported)
            };
        #[cfg(target_os = "windows")]
        std::thread::spawn(|| {
            if let Ok(root) = gpui_updater_core::install::current_install_root() {
                gpui_updater_core::install::remove_leftovers(&root);
            }
        });
        Self {
            backend,
            events,
            state,
        }
    }
    pub fn can_act(&self) -> bool {
        !matches!(self.backend, UpdateBackend::Unavailable)
            && matches!(
                self.state,
                AppUpdate::Idle
                    | AppUpdate::UpToDate
                    | AppUpdate::Available(_)
                    | AppUpdate::Failed(_)
                    | AppUpdate::ReadyToRestart { .. }
            )
    }
    pub fn blocks_hardware(&self) -> bool {
        #[cfg(target_os = "macos")]
        if matches!(self.backend, UpdateBackend::Sparkle(_))
            && matches!(self.state, AppUpdate::Downloading { .. })
        {
            return true;
        }
        matches!(self.state, AppUpdate::Installing | AppUpdate::Restarting)
    }
    pub fn action_label(&self) -> String {
        match &self.state {
            AppUpdate::Available(release) => {
                format!("Update Stickwalker Desktop to {}", release.version)
            }
            AppUpdate::ReadyToRestart { .. } => "Restart to Update".into(),
            _ => "Check for Updates…".into(),
        }
    }
    pub fn check(&mut self, background: bool) {
        if !matches!(
            self.state,
            AppUpdate::Idle | AppUpdate::UpToDate | AppUpdate::Failed(_)
        ) {
            return;
        }
        match &self.backend {
            UpdateBackend::Unavailable => return,
            #[cfg(target_os = "macos")]
            UpdateBackend::Sparkle(driver) => driver.borrow().check_for_update_information(),
            #[cfg(not(target_os = "macos"))]
            UpdateBackend::Native(tx) => {
                let tx = tx.clone();
                std::thread::spawn(move || {
                    let state = match engine() {
                        Err(error) => AppUpdate::setup_error(error),
                        Ok(engine) => match engine.check() {
                            Ok(Some(release)) => AppUpdate::Available(Box::new(release)),
                            Ok(None) => AppUpdate::Idle,
                            Err(error) => AppUpdate::failed("Couldn’t check for updates.", error),
                        },
                    };
                    let _ = tx.try_send(UpdateEvent::Native(state));
                });
            }
        }
        self.state = AppUpdate::Checking { background };
    }
    pub fn install(&mut self) {
        let AppUpdate::Available(_release) = &self.state else {
            return;
        };
        match &self.backend {
            UpdateBackend::Unavailable => return,
            #[cfg(target_os = "macos")]
            UpdateBackend::Sparkle(driver) => driver.borrow_mut().install(),
            #[cfg(not(target_os = "macos"))]
            UpdateBackend::Native(tx) => {
                let tx = tx.clone();
                let release = _release.clone();
                std::thread::spawn(move || {
                    let state = match engine() {
                        Err(error) => AppUpdate::setup_error(error),
                        Ok(engine) => match engine.download(&release, |n, total| {
                            let _ = tx.try_send(UpdateEvent::Native(AppUpdate::Downloading {
                                percent: total
                                    .filter(|n| *n > 0)
                                    .map(|total| (n * 100 / total).min(100) as u8),
                            }));
                        }) {
                            Ok(artifact) => AppUpdate::ReadyToInstall(artifact),
                            Err(error) => AppUpdate::failed("Couldn’t download the update.", error),
                        },
                    };
                    let _ = tx.try_send(UpdateEvent::Native(state));
                });
            }
        }
        self.state = AppUpdate::Downloading { percent: None };
    }
    // Called on the UI thread only after the active hardware job has finished.
    #[cfg(not(target_os = "macos"))]
    pub fn install_downloaded(&mut self) {
        let AppUpdate::ReadyToInstall(artifact) = &self.state else {
            return;
        };
        let UpdateBackend::Native(tx) = &self.backend else {
            return;
        };
        let artifact = artifact.clone();
        self.state = AppUpdate::Installing;
        let tx = tx.clone();
        std::thread::spawn(move || {
            let state = match engine() {
                Err(error) => AppUpdate::setup_error(error),
                Ok(engine) => match engine.install(&artifact) {
                    Ok(installed) => AppUpdate::ReadyToRestart {
                        path: installed.restart_path,
                    },
                    Err(error) => AppUpdate::failed("Couldn’t install the update.", error),
                },
            };
            let _ = tx.try_send(UpdateEvent::Native(state));
        });
    }
    pub fn accept(&mut self, event: UpdateEvent) {
        let checking = matches!(self.state, AppUpdate::Checking { .. });
        let background = matches!(self.state, AppUpdate::Checking { background: true });
        let current = || {
            if background {
                AppUpdate::Idle
            } else {
                AppUpdate::UpToDate
            }
        };
        match event {
            #[cfg(any(not(target_os = "macos"), debug_assertions))]
            UpdateEvent::Native(state) => {
                self.state = match state {
                    AppUpdate::Failed(_) if background => AppUpdate::Idle,
                    AppUpdate::Idle if checking => current(),
                    state => state,
                };
            }
            #[cfg(any(target_os = "macos", test))]
            UpdateEvent::Driver(event) => {
                let check_failed = || {
                    if background {
                        AppUpdate::Idle
                    } else {
                        AppUpdate::Failed("Couldn’t check for updates.".into())
                    }
                };
                self.state = match *event {
                    DriverEvent::Checking
                    | DriverEvent::Found(_)
                    | DriverEvent::Downloading { .. }
                    | DriverEvent::Installing
                        if matches!(self.state, AppUpdate::Restarting) =>
                    {
                        return;
                    }
                    DriverEvent::Failed(error) if matches!(self.state, AppUpdate::Restarting) => {
                        AppUpdate::failed("Couldn’t restart to update.", error)
                    }
                    DriverEvent::Settled if matches!(self.state, AppUpdate::Restarting) => {
                        AppUpdate::Idle
                    }
                    // Sparkle repeats discovery when the user accepts an offered update.
                    DriverEvent::Checking | DriverEvent::Found(_)
                        if matches!(
                            self.state,
                            AppUpdate::Downloading { .. } | AppUpdate::Installing
                        ) =>
                    {
                        return;
                    }
                    DriverEvent::Checking => AppUpdate::Checking {
                        background: background || !checking,
                    },
                    DriverEvent::UpToDate => current(),
                    DriverEvent::Found(release) => AppUpdate::Available(Box::new(release)),
                    DriverEvent::Downloading { downloaded, total } => AppUpdate::Downloading {
                        percent: total
                            .filter(|n| *n > 0)
                            .map(|t| (downloaded * 100 / t).min(100) as u8),
                    },
                    DriverEvent::Installing => AppUpdate::Installing,
                    DriverEvent::ReadyToRelaunch(_) => AppUpdate::ReadyToRestart { path: None },
                    DriverEvent::Failed(error) if checking => {
                        eprintln!("Desktop update check failed: {error}");
                        check_failed()
                    }
                    DriverEvent::Failed(error) => AppUpdate::failed(
                        if matches!(self.state, AppUpdate::Installing) {
                            "Couldn’t install the update."
                        } else {
                            "Couldn’t download the update."
                        },
                        error,
                    ),
                    DriverEvent::Settled
                        if matches!(
                            self.state,
                            AppUpdate::Checking { .. }
                                | AppUpdate::Downloading { .. }
                                | AppUpdate::Installing
                        ) =>
                    {
                        AppUpdate::Idle
                    }
                    _ => return,
                };
            }
        }
    }
    pub fn defer_restart(&mut self, cx: &mut gpui_kit::App) -> bool {
        let AppUpdate::ReadyToRestart { path } = &self.state else {
            return false;
        };
        if let Some(path) = path {
            cx.set_restart_path(path.clone());
        }
        match &self.backend {
            #[cfg(target_os = "macos")]
            UpdateBackend::Sparkle(driver) => {
                let driver = driver.clone();
                cx.defer(move |_| driver.borrow_mut().relaunch());
            }
            #[cfg(not(target_os = "macos"))]
            UpdateBackend::Native(_) => cx.defer(|cx| cx.restart()),
            UpdateBackend::Unavailable => return false,
        }
        self.state = AppUpdate::Restarting;
        true
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn an_unavailable_backend_cannot_start_a_manual_or_automatic_check() {
        use super::{AppUpdate, UpdateBackend, Updates};
        let (_, events) = async_channel::unbounded();
        let mut updates = Updates {
            backend: UpdateBackend::Unavailable,
            events,
            state: AppUpdate::Failed("Couldn’t start app updates.".into()),
        };
        assert!(!updates.can_act());
        for automatic in [false, true] {
            updates.check(automatic);
            assert!(matches!(updates.state, AppUpdate::Failed(_)));
        }
    }

    use super::linux_bundle_root;
    #[test]
    fn linux_updates_replace_only_the_extracted_application() {
        let dir = tempfile::tempdir().unwrap();
        let exe = dir.path().join("stickwalker");
        std::fs::write(&exe, []).unwrap();
        assert_eq!(
            linux_bundle_root(&exe).unwrap_err().to_string(),
            "Extract the full Stickwalker folder to update."
        );
        for name in [
            "icon.png",
            "60-stickwalker.rules",
            "install-launcher.py",
            concat!(env!("STICKWALKER_APP_ID"), ".desktop"),
        ] {
            std::fs::write(dir.path().join(name), []).unwrap();
        }
        assert_eq!(linux_bundle_root(&exe).unwrap(), dir.path());
        std::fs::write(dir.path().join(".directory"), []).unwrap();
        let cache = dir.path().join("__pycache__");
        std::fs::create_dir(&cache).unwrap();
        std::fs::write(cache.join("install-launcher.cpython-313.pyc"), []).unwrap();
        assert_eq!(linux_bundle_root(&exe).unwrap(), dir.path());
        std::fs::write(cache.join("travel.eeprom"), []).unwrap();
        assert!(
            linux_bundle_root(&exe)
                .unwrap_err()
                .to_string()
                .contains("__pycache__/travel.eeprom")
        );
        std::fs::remove_file(cache.join("travel.eeprom")).unwrap();
        std::fs::write(dir.path().join("eeprom.bin"), []).unwrap();
        assert_eq!(
            linux_bundle_root(&exe).unwrap_err().to_string(),
            "Move “eeprom.bin” out of the Stickwalker folder to update."
        );
    }
}
