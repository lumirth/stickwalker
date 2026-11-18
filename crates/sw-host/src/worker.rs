use crate::{
    DeviceSettings,
    protocol::{self, Session},
    release::Package,
    save,
    usb::{self, DeviceMode, Info, Port},
};
use anyhow::{Context, Result};
use std::{
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, Sender},
    },
    thread,
    time::{Duration, Instant},
};

pub enum Command {
    Select(Port),
    DismissError,
    CheckFirmware,
    FirmwareChecked(Result<Package>),
    OpenPackage(PathBuf),
    Update,
    CleanInstall,
    Export(PathBuf),
    Reset,
    Import(PathBuf),
    SetSettings {
        revision: u64,
        settings: DeviceSettings,
    },
}
impl Command {
    fn hardware_operation(&self) -> bool {
        matches!(
            self,
            Self::Update | Self::CleanInstall | Self::Export(_) | Self::Import(_) | Self::Reset
        )
    }
}
#[derive(Clone)]
pub struct Commands {
    tx: Sender<(Option<Port>, Command)>,
}
impl Commands {
    pub fn send(&self, port: Option<Port>, command: Command) -> Result<()> {
        self.tx
            .send((port, command))
            .map_err(|_| anyhow::anyhow!("Device worker closed"))
    }
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum ConnectionState {
    #[default]
    Disconnected,
    Unmanaged {
        port: Port,
    },
    Unavailable {
        port: Port,
        message: String,
    },
    Reconnecting {
        port: Port,
    },
    Ready {
        port: Port,
        info: Info,
    },
    LinkingInGame {
        port: Port,
        info: Info,
    },
    SaveFault {
        port: Port,
        info: Info,
    },
    Flashing {
        port: Port,
        title: String,
        step: String,
        percent: Option<u8>,
    },
}
impl ConnectionState {
    pub fn port(&self) -> Option<&Port> {
        match self {
            Self::Disconnected => None,
            Self::Unmanaged { port }
            | Self::Unavailable { port, .. }
            | Self::Reconnecting { port }
            | Self::Ready { port, .. }
            | Self::LinkingInGame { port, .. }
            | Self::SaveFault { port, .. }
            | Self::Flashing { port, .. } => Some(port),
        }
    }
    pub fn info(&self) -> Option<&Info> {
        match self {
            Self::Ready { info, .. }
            | Self::LinkingInGame { info, .. }
            | Self::SaveFault { info, .. } => Some(info),
            _ => None,
        }
    }
    pub fn info_mut(&mut self) -> Option<&mut Info> {
        match self {
            Self::Ready { info, .. }
            | Self::LinkingInGame { info, .. }
            | Self::SaveFault { info, .. } => Some(info),
            _ => None,
        }
    }
    fn managed(port: Port, info: Info) -> Self {
        match info.mode {
            DeviceMode::Linking => Self::LinkingInGame { port, info },
            DeviceMode::SaveFault => Self::SaveFault { port, info },
            DeviceMode::Ready => Self::Ready { port, info },
        }
    }
}
#[derive(Clone, PartialEq, Eq)]
pub enum Firmware {
    Release(String),
    Local(String),
}
impl Firmware {
    pub fn version(&self) -> &str {
        match self {
            Self::Release(v) | Self::Local(v) => v,
        }
    }
    pub fn local(&self) -> bool {
        matches!(self, Self::Local(_))
    }
}
#[derive(Clone, Default, PartialEq, Eq)]
pub struct Presentation {
    pub ports: Vec<Port>,
    pub connection: ConnectionState,
    pub firmware: Option<Firmware>,
    pub status: Option<String>,
    pub firmware_feedback: Option<String>,
    pub checking_firmware: bool,
    pub error: Option<String>,
    pub acked_revision: u64,
    pub hardware_busy: bool,
}
impl Presentation {
    pub fn port_label(&self, port: &Port) -> String {
        let reported = self
            .connection
            .port()
            .zip(self.connection.info().map(|info| info.mac.as_str()));
        port.label_with_mac(&self.ports, reported)
    }
    fn discover(&mut self, ports: Vec<Port>) -> bool {
        self.ports = ports;
        if self
            .connection
            .port()
            .is_some_and(|p| self.ports.contains(p))
        {
            return false;
        }
        let next = self
            .connection
            .port()
            .and_then(|old| self.ports.iter().find(|p| p.same_device(old)))
            .or_else(|| self.ports.first())
            .cloned();
        if next.as_ref() == self.connection.port() {
            return false;
        }
        self.firmware_feedback = None;
        self.connection = next
            .map(|port| ConnectionState::Unmanaged { port })
            .unwrap_or_default();
        self.acked_revision = 0;
        true
    }
}
pub struct Worker {
    pub commands: Commands,
    pub events: async_channel::Receiver<Presentation>,
    pub hardware_busy: Arc<AtomicBool>,
}
impl Worker {
    #[cfg(debug_assertions)]
    pub fn preview() -> Self {
        let (tx, _) = mpsc::channel();
        let (_, events) = async_channel::unbounded();
        Self {
            commands: Commands { tx },
            events,
            hardware_busy: Default::default(),
        }
    }
    pub fn start() -> Self {
        let (commands, rx) = mpsc::channel();
        let (tx, events) = async_channel::unbounded();
        let hardware_busy = Arc::new(AtomicBool::new(false));
        let flag = hardware_busy.clone();
        let wake = commands.clone();
        thread::spawn(move || {
            loop {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(&rx, wake.clone(), tx.clone(), flag.clone())
                }));
                if result.is_ok() || tx.is_closed() {
                    break;
                }
                let _ = tx.try_send(Presentation {
                    error: Some("The connection was interrupted. Reconnecting…".into()),
                    ..Default::default()
                });
                thread::sleep(Duration::from_millis(300));
            }
        });
        Self {
            commands: Commands { tx: commands },
            events,
            hardware_busy,
        }
    }
}
struct HardwareOperation(Arc<AtomicBool>);
impl HardwareOperation {
    fn begin(flag: &Arc<AtomicBool>) -> Self {
        flag.store(true, Ordering::SeqCst);
        Self(flag.clone())
    }
}
impl Drop for HardwareOperation {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

fn publish(
    tx: &async_channel::Sender<Presentation>,
    state: &Presentation,
    last: &mut Option<Presentation>,
) -> bool {
    if last.as_ref() == Some(state) {
        return true;
    }
    if tx.try_send(state.clone()).is_err() {
        return false;
    }
    *last = Some(state.clone());
    true
}
fn run(
    rx: &Receiver<(Option<Port>, Command)>,
    wake: Sender<(Option<Port>, Command)>,
    tx: async_channel::Sender<Presentation>,
    hardware_busy: Arc<AtomicBool>,
) {
    let mut state = Presentation::default();
    let mut session: Option<Session> = None;
    let mut package = Package::bundled().ok();
    state.firmware = package
        .as_ref()
        .map(|p| Firmware::Release(p.manifest.version.clone()));
    let mut last_sent = None;
    let mut discover = Instant::now();
    let mut poll = Instant::now();
    let mut deferred = None;
    let mut identify = None;
    let mut retry_until = Instant::now();
    let mut last_settings = Instant::now() - Duration::from_millis(33);
    loop {
        if tx.is_closed() {
            break;
        }
        let next_wake = if session.is_some() {
            discover.min(poll)
        } else {
            discover
        };
        match deferred
            .take()
            .map(Ok)
            .unwrap_or_else(|| rx.recv_timeout(next_wake.saturating_duration_since(Instant::now())))
        {
            Ok((target, mut command)) => {
                if (command.hardware_operation() || matches!(command, Command::SetSettings { .. }))
                    && target.as_ref() != state.connection.port()
                {
                    continue;
                }
                if matches!(command, Command::SetSettings { .. }) {
                    thread::sleep(
                        Duration::from_millis(33).saturating_sub(last_settings.elapsed()),
                    );
                    last_settings = Instant::now();
                    while let Ok(next) = rx.try_recv() {
                        if next.0 == target && matches!(next.1, Command::SetSettings { .. }) {
                            command = next.1;
                        } else {
                            deferred = Some(next);
                            break;
                        }
                    }
                }
                state.error = None;
                state.status = None;
                let operation = command.hardware_operation();
                let operation_guard = operation.then(|| HardwareOperation::begin(&hardware_busy));
                state.hardware_busy = operation;
                publish(&tx, &state, &mut last_sent);
                let result = (|| -> Result<()> {
                    match command {
                        Command::DismissError => {}
                        Command::Select(port) => {
                            if !state.ports.contains(&port) {
                                return Ok(());
                            }
                            identify = Some(port.clone());
                            retry_until = Instant::now();
                            session = None;
                            state.connection = ConnectionState::Reconnecting { port };
                            state.acked_revision = 0;
                            discover = Instant::now();
                        }
                        Command::CheckFirmware => {
                            if !state.checking_firmware {
                                state.checking_firmware = true;
                                state.firmware_feedback =
                                    Some("Checking for firmware updates…".into());
                                let wake = wake.clone();
                                thread::spawn(move || {
                                    let _ = wake
                                        .send((None, Command::FirmwareChecked(Package::latest())));
                                });
                            }
                        }
                        Command::FirmwareChecked(result) => {
                            state.checking_firmware = false;
                            state.firmware_feedback = None;
                            let p = result?;
                            let available =
                                p.manifest.version.parse::<crate::release::Version>()?;
                            if package
                                .as_ref()
                                .and_then(|p| {
                                    p.manifest.version.parse::<crate::release::Version>().ok()
                                })
                                .is_none_or(|v| v <= available)
                            {
                                state.firmware =
                                    Some(Firmware::Release(p.manifest.version.clone()));
                                package = Some(p);
                            }
                            let latest = state
                                .firmware
                                .as_ref()
                                .context("Firmware unavailable")?
                                .version();
                            state.firmware_feedback = Some(match state.connection.info() {
                                Some(info)
                                    if info.version.parse::<crate::release::Version>().ok()
                                        >= latest.parse().ok() =>
                                {
                                    format!("Firmware {} is up to date", info.version)
                                }
                                _ => format!("Firmware {latest} is available"),
                            });
                        }
                        Command::OpenPackage(path) => {
                            let p = Package::open(&path)?;
                            state.firmware = Some(Firmware::Local(p.manifest.version.clone()));
                            package = Some(p);
                        }
                        Command::SetSettings { revision, settings } => {
                            let s = session
                                .as_mut()
                                .context("Connect a Stickwalker to change its colors")?;
                            s.request(protocol::SETTINGS, &settings.palette_bytes())?;
                            state.acked_revision = revision;
                            let port = state.connection.port().unwrap().clone();
                            let mut info = state.connection.info().unwrap().clone();
                            info.settings.active_palette = settings.active_palette;
                            info.settings.slots = settings.slots;
                            state.connection = ConnectionState::managed(port, info);
                        }
                        Command::Export(path) => {
                            state.status = Some("Exporting EEPROM…".into());
                            publish(&tx, &state, &mut last_sent);
                            let s = session
                                .as_mut()
                                .context("Connect a Stickwalker to export EEPROM")?;
                            save::atomic_write(&path, &usb::read_eeprom(s)?)?;
                        }
                        Command::Import(path) => {
                            let bytes = save::read_eeprom(&path)?;
                            state.status = Some("Importing EEPROM…".into());
                            publish(&tx, &state, &mut last_sent);
                            let s = session
                                .as_mut()
                                .context("Connect a Stickwalker to import EEPROM")?;
                            usb::write_eeprom(s, &bytes)?;
                            let port = state.connection.port().unwrap().clone();
                            state.connection = ConnectionState::managed(port, Info::read(s)?);
                        }
                        Command::Reset => {
                            let s = session
                                .as_mut()
                                .context("Connect a Stickwalker to reset it")?;
                            usb::reset_walker(s)?;
                            let port = state.connection.port().unwrap().clone();
                            state.connection = ConnectionState::managed(port, Info::read(s)?);
                        }
                        Command::Update | Command::CleanInstall => {
                            let fresh = matches!(command, Command::CleanInstall);
                            let port = state
                                .connection
                                .port()
                                .cloned()
                                .context("Connect a StickS3 first")?;
                            if package.is_none() {
                                package = Some(Package::latest()?);
                            }
                            let title = if fresh || state.connection.info().is_none() {
                                "Installing Stickwalker…"
                            } else if state.connection.info().is_some_and(|i| {
                                Some(i.version.as_str())
                                    == state.firmware.as_ref().map(Firmware::version)
                            }) {
                                "Reinstalling Stickwalker…"
                            } else {
                                "Updating Stickwalker…"
                            };
                            let active_session = session.take();
                            state.connection = ConnectionState::Flashing {
                                title: title.into(),
                                port: port.clone(),
                                step: "Preparing Stickwalker".into(),
                                percent: None,
                            };
                            publish(&tx, &state, &mut last_sent);
                            let result = crate::flash::install(
                                &port,
                                active_session,
                                package.as_ref().unwrap(),
                                fresh,
                                &mut |phase, percent| {
                                    let step = match percent {
                                        Some(p) => format!("{phase} · {p}%"),
                                        None => phase.into(),
                                    };
                                    state.connection = ConnectionState::Flashing {
                                        title: title.into(),
                                        port: port.clone(),
                                        step,
                                        percent,
                                    };
                                    publish(&tx, &state, &mut last_sent);
                                },
                            );
                            state.connection = match result {
                                Ok((boot_port, info)) => ConnectionState::managed(boot_port, info),
                                Err(error) => {
                                    state.connection = ConnectionState::Unmanaged { port };
                                    return Err(error);
                                }
                            };
                            state.firmware = package
                                .as_ref()
                                .map(|p| Firmware::Release(p.manifest.version.clone()));
                            discover = Instant::now();
                        }
                    }
                    Ok(())
                })();
                state.status = None;
                if let Err(error) = result {
                    state.error = Some(error.to_string());
                }
                drop(operation_guard);
                state.hardware_busy = false;
                if !publish(&tx, &state, &mut last_sent) {
                    break;
                }
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if Instant::now() >= discover {
            discover = Instant::now() + Duration::from_secs(2);
            match usb::ports() {
                Ok(ports) => {
                    if state.discover(ports) {
                        session = None;
                        identify = None;
                    }
                    if session.is_none()
                        && let Some(port) = state.connection.port().cloned()
                    {
                        let retry =
                            matches!(state.connection, ConnectionState::Reconnecting { .. })
                                && Instant::now() < retry_until;
                        match Session::open(&port.path) {
                            Ok(mut opened) => match Info::read(&mut opened) {
                                Ok(info) => {
                                    if identify.take().as_ref() == Some(&port)
                                        && info.mode != DeviceMode::Linking
                                    {
                                        let _ = opened.request(protocol::IDENTIFY, &[]);
                                    }
                                    state.connection = ConnectionState::managed(port, info);
                                    session = Some(opened);
                                }
                                Err(_) if retry => {
                                    discover = Instant::now() + Duration::from_millis(350)
                                }
                                Err(_) => state.connection = ConnectionState::Unmanaged { port },
                            },
                            Err(_) if retry => {
                                discover = Instant::now() + Duration::from_millis(350)
                            }
                            Err(error) => {
                                let message = if cfg!(target_os = "linux")
                                    && error.downcast_ref::<serialport::Error>().is_some_and(|e| {
                                        e.kind()
                                            == serialport::ErrorKind::Io(
                                                std::io::ErrorKind::PermissionDenied,
                                            )
                                    }) {
                                    "Install the included 60-stickwalker.rules file, then reconnect the Stick.".into()
                                } else {
                                    format!(
                                        "Close other apps using {} and reconnect the Stick.",
                                        port.path
                                    )
                                };
                                state.connection = ConnectionState::Unavailable { port, message };
                            }
                        }
                    }
                }
                Err(error) => state.error = Some(error.to_string()),
            }
            if !publish(&tx, &state, &mut last_sent) {
                break;
            }
        }
        if Instant::now() >= poll {
            poll = Instant::now() + Duration::from_millis(500);
            if let Some(s) = &mut session {
                let port = state.connection.port().unwrap().clone();
                match Info::read(s) {
                    Ok(info) => state.connection = ConnectionState::managed(port, info),
                    Err(_) => {
                        session = None;
                        retry_until = Instant::now() + Duration::from_secs(4);
                        discover = Instant::now();
                        state.connection = ConnectionState::Reconnecting { port };
                    }
                }
                if !publish(&tx, &state, &mut last_sent) {
                    break;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn discovery_keeps_the_selected_stick_until_it_is_unplugged() {
        let a = Port {
            serial: "70:04:1D:DC:1A:20".into(),
            path: "COM4".into(),
        };
        let b = Port {
            serial: "70:04:1D:DC:8B:04".into(),
            path: "COM7".into(),
        };
        let mut state = Presentation::default();
        state.discover(vec![a.clone(), b.clone()]);
        assert_eq!(state.connection.port(), Some(&a));
        state.discover(vec![b.clone(), a.clone()]);
        assert_eq!(state.connection.port(), Some(&a));
        let moved = Port {
            path: "COM12".into(),
            serial: "70041ddc1a20".into(),
        };
        state.discover(vec![b.clone(), moved.clone()]);
        assert_eq!(state.connection.port(), Some(&moved));
        state.discover(vec![b.clone()]);
        assert_eq!(state.connection.port(), Some(&b));
        assert_eq!(b.label(&state.ports), "Stick 8B04");
        state.discover(vec![]);
        assert!(matches!(state.connection, ConnectionState::Disconnected));
    }
    #[test]
    fn port_labels_use_reported_identity_across_platforms() {
        let mac = "30:ed:a0:00:1a:20";
        for (path, serial) in [
            ("/dev/cu.usbmodem14101", "30EDA0001A20"),
            ("/dev/ttyACM0", mac),
        ] {
            let port = Port {
                path: path.into(),
                serial: serial.into(),
            };
            assert_eq!(port.label(std::slice::from_ref(&port)), "Stick 1A20");
        }

        let port = Port {
            path: "COM4".into(),
            serial: String::new(),
        };
        let mut state = Presentation {
            connection: ConnectionState::Unmanaged { port: port.clone() },
            ports: vec![port.clone()],
            ..Default::default()
        };
        assert_eq!(state.port_label(&port), "Stick COM4");
        state.connection = ConnectionState::Ready {
            port: port.clone(),
            info: Info {
                mac: mac.into(),
                mode: DeviceMode::Ready,
                version: "0.2.1".into(),
                build: String::new(),
                millivolts: 4100,
                settings: DeviceSettings::default(),
                power: 1,
            },
        };
        assert_eq!(state.port_label(&port), "Stick 1A20");
        let other = Port {
            path: "COM9".into(),
            serial: "AABBCCDD1A20".into(),
        };
        state.ports.push(other.clone());
        assert_eq!(state.port_label(&port), "Stick 1A20 (COM4)");
        assert_eq!(state.port_label(&other), "Stick 1A20 (COM9)");
    }
    #[test]
    fn hardware_lock_is_released_during_unwind() {
        let flag = Arc::new(AtomicBool::new(false));
        let _ = std::panic::catch_unwind(|| {
            let _operation = HardwareOperation::begin(&flag);
            assert!(flag.load(Ordering::SeqCst));
            panic!("operation failed");
        });
        assert!(!flag.load(Ordering::SeqCst));
    }
}
