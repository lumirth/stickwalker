use gpui_updater_core::{
    Artifact, DriverEvent, DriverEvents, EngineConfig, OwnershipHints, Release,
    StaticManifestSource, UpdateDriver, UpdateEngine, Verification,
};
use gpui_updater_sparkle::{SparkleDriver, SparkleOptions, StagedUpdatePolicy};
use std::path::PathBuf;

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
        version: String,
        percent: Option<u8>,
    },
    ReadyToInstall(Artifact),
    Installing,
    ReadyToRestart {
        path: Option<PathBuf>,
    },
    Failed(String),
}
pub enum UpdateEvent {
    Driver(Box<DriverEvent>),
    Native(AppUpdate),
}
pub struct Updates {
    driver: Option<SparkleDriver>,
    pub events: async_channel::Receiver<UpdateEvent>,
    native_tx: async_channel::Sender<UpdateEvent>,
    pub state: AppUpdate,
}
fn engine() -> anyhow::Result<UpdateEngine<StaticManifestSource>> {
    let key = option_env!("STICKWALKER_UPDATE_KEY")
        .filter(|s| !s.is_empty())
        .ok_or_else(|| anyhow::anyhow!("This development build has no signed app update feed."))?;
    let source = StaticManifestSource::new(option_env!("STICKWALKER_APP_FEED").unwrap_or(FEED))
        .os(std::env::consts::OS)
        .arch(std::env::consts::ARCH);
    let config = EngineConfig::new(env!("CARGO_PKG_VERSION").parse()?)
        .ed25519_public_key(key)
        .verification(Verification::Strict);
    let engine = UpdateEngine::new(source, config);
    let ownership = engine.ownership(&OwnershipHints::new());
    anyhow::ensure!(
        ownership.is_self_update(),
        "Update this installation with its package manager. {}",
        ownership.update_command().unwrap_or_default()
    );
    Ok(engine)
}
impl Updates {
    pub fn new() -> Self {
        let (native_tx, events) = async_channel::unbounded();
        let tx = native_tx.clone();
        let (driver, state) = match SparkleDriver::load(
            SparkleOptions::new().staged_updates(StagedUpdatePolicy::HostRelaunches),
        ) {
            Ok(Some(mut driver)) => {
                driver.start(DriverEvents::new(move |event| {
                    let _ = tx.try_send(UpdateEvent::Driver(Box::new(event)));
                }));
                driver.set_automatically_checks_for_updates(false);
                (Some(driver), AppUpdate::Idle)
            }
            Ok(None)
                if !cfg!(target_os = "macos")
                    && option_env!("STICKWALKER_UPDATE_KEY").is_some() =>
            {
                (None, AppUpdate::Idle)
            }
            Ok(None) => (None, AppUpdate::Unsupported),
            Err(error) => (None, AppUpdate::Failed(error.to_string())),
        };
        if let Ok(root) = gpui_updater_core::install::current_install_root() {
            gpui_updater_core::install::remove_leftovers(&root);
        }
        Self {
            driver,
            events,
            native_tx,
            state,
        }
    }
    pub fn can_act(&self) -> bool {
        matches!(
            self.state,
            AppUpdate::Idle
                | AppUpdate::UpToDate
                | AppUpdate::Available(_)
                | AppUpdate::Failed(_)
                | AppUpdate::ReadyToRestart { .. }
        )
    }
    pub fn is_installing(&self) -> bool {
        matches!(self.state, AppUpdate::Installing)
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
        self.state = AppUpdate::Checking { background };
        if let Some(driver) = &mut self.driver {
            if background {
                driver.check_in_background();
            } else {
                driver.check();
            }
        } else {
            let tx = self.native_tx.clone();
            std::thread::spawn(move || {
                let state = match engine().and_then(|engine| Ok(engine.check()?)) {
                    Ok(Some(release)) => AppUpdate::Available(Box::new(release)),
                    Ok(None) => AppUpdate::Idle,
                    Err(error) => AppUpdate::Failed(error.to_string()),
                };
                let _ = tx.try_send(UpdateEvent::Native(state));
            });
        }
    }
    pub fn install(&mut self) {
        let AppUpdate::Available(release) = &self.state else {
            return;
        };
        let release = release.clone();
        self.state = AppUpdate::Downloading {
            version: release.version.to_string(),
            percent: None,
        };
        if let Some(driver) = &mut self.driver {
            driver.install();
            return;
        }
        let tx = self.native_tx.clone();
        std::thread::spawn(move || {
            let result = (|| -> anyhow::Result<_> {
                let engine = engine()?;
                Ok(engine.download(&release, |n, total| {
                    let _ = tx.try_send(UpdateEvent::Native(AppUpdate::Downloading {
                        version: release.version.to_string(),
                        percent: total
                            .filter(|n| *n > 0)
                            .map(|total| (n * 100 / total).min(100) as u8),
                    }));
                })?)
            })();
            let _ = tx.try_send(UpdateEvent::Native(match result {
                Ok(artifact) => AppUpdate::ReadyToInstall(artifact),
                Err(error) => AppUpdate::Failed(error.to_string()),
            }));
        });
    }
    // Called on the UI thread only after the active hardware job has finished.
    pub fn install_downloaded(&mut self) {
        let AppUpdate::ReadyToInstall(artifact) = &self.state else {
            return;
        };
        let artifact = artifact.clone();
        self.state = AppUpdate::Installing;
        let tx = self.native_tx.clone();
        std::thread::spawn(move || {
            let state = match engine().and_then(|engine| Ok(engine.install(&artifact)?)) {
                Ok(installed) => AppUpdate::ReadyToRestart {
                    path: installed.restart_path,
                },
                Err(error) => AppUpdate::Failed(error.to_string()),
            };
            let _ = tx.try_send(UpdateEvent::Native(state));
        });
    }
    pub fn accept(&mut self, event: UpdateEvent) {
        let checking = matches!(self.state, AppUpdate::Checking { .. });
        let background = matches!(self.state, AppUpdate::Checking { background: true });
        let check_failed = || {
            if background {
                AppUpdate::Idle
            } else {
                AppUpdate::Failed("Couldn’t check for updates.".into())
            }
        };
        let current = || {
            if background {
                AppUpdate::Idle
            } else {
                AppUpdate::UpToDate
            }
        };
        let UpdateEvent::Driver(event) = event else {
            if let UpdateEvent::Native(state) = event {
                self.state = match state {
                    AppUpdate::Failed(_) if checking => check_failed(),
                    AppUpdate::Idle if checking => current(),
                    state => state,
                };
            }
            return;
        };
        self.state = match *event {
            DriverEvent::Checking => AppUpdate::Checking {
                background: background || !checking,
            },
            DriverEvent::UpToDate => current(),
            DriverEvent::Found(release) => AppUpdate::Available(Box::new(release)),
            DriverEvent::Downloading { downloaded, total } => {
                let version = match &self.state {
                    AppUpdate::Available(release) => release.version.to_string(),
                    AppUpdate::Downloading { version, .. } => version.clone(),
                    _ => String::new(),
                };
                AppUpdate::Downloading {
                    version,
                    percent: total
                        .filter(|n| *n > 0)
                        .map(|t| (downloaded * 100 / t).min(100) as u8),
                }
            }
            DriverEvent::Installing => AppUpdate::Installing,
            DriverEvent::ReadyToRelaunch(_) => AppUpdate::ReadyToRestart { path: None },
            DriverEvent::Failed(_) if checking => check_failed(),
            DriverEvent::Failed(error) => AppUpdate::Failed(error.to_string()),
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
    pub fn restart(&mut self, cx: &mut gpui_kit::App) {
        let AppUpdate::ReadyToRestart { path } = &self.state else {
            return;
        };
        if let Some(driver) = &mut self.driver {
            driver.relaunch();
        } else {
            if let Some(path) = path {
                cx.set_restart_path(path.clone());
            }
            cx.restart();
        }
    }
}
