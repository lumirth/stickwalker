mod connection;
use crate::{
    DeviceSettings,
    protocol::{self, Session},
    release::Package,
    save,
    usb::{self, DeviceMode, Info, Port},
};
use anyhow::{Context, Result};
use connection::ActiveDevice;
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
    Shutdown,
    Select(Port),
    DismissError,
    CheckFirmware,
    FirmwareChecked(Result<Package>),
    OpenPackage(PathBuf),
    Update,
    CleanInstall,
    Export(PathBuf),
    Reset,
    Import(save::EepromImage),
    SetSettings {
        revision: u64,
        settings: DeviceSettings,
    },
}
impl Command {
    pub fn allowed(&self, connection: &ConnectionState) -> bool {
        let access = connection.access();
        match self {
            Self::SetSettings { .. } | Self::Export(_) => access == DeviceAccess::Ready,
            Self::Import(_) | Self::Reset => access.can_write_save(),
            Self::Update | Self::CleanInstall | Self::OpenPackage(_) => access.can_install(),
            _ => true,
        }
    }
    fn targets_device(&self) -> bool {
        self.hardware_operation() || matches!(self, Self::SetSettings { .. } | Self::OpenPackage(_))
    }
    fn hardware_operation(&self) -> bool {
        matches!(
            self,
            Self::Update | Self::CleanInstall | Self::Export(_) | Self::Import(_) | Self::Reset
        )
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceTarget {
    port: Port,
    mac: Option<String>,
}
struct Work {
    target: Option<DeviceTarget>,
    command: Command,
    operation: Option<HardwareOperation>,
}
#[derive(Clone)]
pub struct Commands {
    tx: Sender<Work>,
    busy: Arc<AtomicBool>,
}
impl Commands {
    pub fn send(&self, target: Option<DeviceTarget>, command: Command) -> Result<()> {
        let operation = command
            .hardware_operation()
            .then(|| HardwareOperation::begin(&self.busy))
            .transpose()?;
        self.tx
            .send(Work {
                target,
                command,
                operation,
            })
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
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DeviceAccess {
    #[default]
    None,
    InstallOnly,
    Recovery,
    Ready,
}
impl DeviceAccess {
    pub fn can_write_save(self) -> bool {
        matches!(self, Self::Ready | Self::Recovery)
    }
    pub fn can_install(self) -> bool {
        self != Self::None
    }
}

impl ConnectionState {
    pub fn access(&self) -> DeviceAccess {
        match self {
            Self::Ready { .. } => DeviceAccess::Ready,
            Self::Unmanaged { .. } => DeviceAccess::InstallOnly,
            Self::SaveFault { .. } => DeviceAccess::Recovery,
            _ => DeviceAccess::None,
        }
    }
    pub fn target(&self) -> Option<DeviceTarget> {
        Some(DeviceTarget {
            port: self.port()?.clone(),
            mac: self.info().map(|info| info.mac.clone()),
        })
    }
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
    pub failed_revision: Option<u64>,
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
}
pub struct Worker {
    pub commands: Commands,
    pub events: async_channel::Receiver<Presentation>,
    pub hardware_busy: Arc<AtomicBool>,
    stopping: Arc<AtomicBool>,
}
impl Worker {
    #[cfg(debug_assertions)]
    pub fn preview() -> Self {
        let (tx, _) = mpsc::channel();
        let (_, events) = async_channel::unbounded();
        Self {
            commands: Commands {
                tx,
                busy: Default::default(),
            },
            stopping: Default::default(),
            events,
            hardware_busy: Default::default(),
        }
    }
    pub fn start() -> Self {
        let (commands, rx) = mpsc::channel();
        let (tx, events) = async_channel::unbounded();
        let hardware_busy = Arc::new(AtomicBool::new(false));
        let stopping = Arc::new(AtomicBool::new(false));
        let stop = stopping.clone();
        let wake = commands.clone();
        thread::spawn(move || {
            loop {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(&rx, &wake, &tx, &stop)
                }));
                if result.is_ok() || tx.is_closed() || stop.load(Ordering::Acquire) {
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
            commands: Commands {
                tx: commands,
                busy: hardware_busy.clone(),
            },
            stopping,
            events,
            hardware_busy,
        }
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        self.stopping.store(true, Ordering::Release);
        let _ = self.commands.send(None, Command::Shutdown);
    }
}
struct HardwareOperation(Arc<AtomicBool>);
impl HardwareOperation {
    fn begin(flag: &Arc<AtomicBool>) -> Result<Self> {
        flag.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .map_err(|_| anyhow::anyhow!("The Stick is busy."))?;
        Ok(Self(flag.clone()))
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
    rx: &Receiver<Work>,
    wake: &Sender<Work>,
    tx: &async_channel::Sender<Presentation>,
    stop: &AtomicBool,
) {
    let mut state = Presentation::default();
    let mut active = ActiveDevice::Disconnected;
    let mut package = Package::bundled().ok();
    state.firmware = package
        .as_ref()
        .map(|p| Firmware::Release(p.manifest.version.clone()));
    let mut last_sent = None;
    let mut discover = Instant::now();
    let mut poll = Instant::now();
    let mut deferred = None;

    let mut last_settings = Instant::now() - Duration::from_millis(33);
    loop {
        if tx.is_closed() || stop.load(Ordering::Acquire) {
            break;
        }
        let mut next_wake = discover;
        if matches!(active, ActiveDevice::Managed { .. }) {
            next_wake = next_wake.min(poll);
        }
        if let Some(deadline) = active.deadline() {
            next_wake = next_wake.min(deadline);
        }
        match deferred
            .take()
            .map(Ok)
            .unwrap_or_else(|| rx.recv_timeout(next_wake.saturating_duration_since(Instant::now())))
        {
            Ok(Work {
                target,
                mut command,
                operation,
            }) => {
                if matches!(command, Command::Shutdown) {
                    return;
                }
                if command.targets_device()
                    && (target != state.connection.target() || !command.allowed(&state.connection))
                {
                    if let Command::SetSettings { revision, .. } = command {
                        state.failed_revision = Some(revision);
                    }
                    state.error = Some("The Stick is no longer available for this action.".into());
                    drop(operation);
                    publish(tx, &state, &mut last_sent);
                    continue;
                }
                if matches!(command, Command::SetSettings { .. }) {
                    thread::sleep(
                        Duration::from_millis(33).saturating_sub(last_settings.elapsed()),
                    );
                    last_settings = Instant::now();
                    while let Ok(next) = rx.try_recv() {
                        if next.target == target
                            && matches!(next.command, Command::SetSettings { .. })
                        {
                            command = next.command;
                        } else {
                            deferred = Some(next);
                            break;
                        }
                    }
                }
                state.error = None;
                state.status = None;
                let revision = match &command {
                    Command::SetSettings { revision, .. } => Some(*revision),
                    _ => None,
                };
                state.hardware_busy = operation.is_some();
                publish(tx, &state, &mut last_sent);
                let result = (|| -> Result<()> {
                    match command {
                        Command::Shutdown => return Ok(()),
                        Command::DismissError => {}
                        Command::Select(port) => {
                            if !state.ports.contains(&port) {
                                return Ok(());
                            }
                            active = ActiveDevice::probe(port, None, true);
                            state.acked_revision = 0;
                            state.failed_revision = None;
                            discover = Instant::now();
                        }
                        Command::CheckFirmware => {
                            if !state.checking_firmware {
                                state.checking_firmware = true;
                                state.firmware_feedback =
                                    Some("Checking for firmware updates…".into());
                                let wake = wake.clone();
                                thread::spawn(move || {
                                    let _ = wake.send(Work {
                                        target: None,
                                        command: Command::FirmwareChecked(Package::latest()),
                                        operation: None,
                                    });
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
                            active.with_session(|session, info| {
                                session.request(protocol::SETTINGS, &settings.palette_bytes())?;
                                info.settings.active_palette = settings.active_palette;
                                info.settings.slots = settings.slots;
                                Ok(())
                            })?;
                            state.acked_revision = revision;
                            state.failed_revision = None;
                        }
                        Command::Export(path) => {
                            state.status = Some("Exporting EEPROM…".into());
                            publish(tx, &state, &mut last_sent);
                            let bytes =
                                active.with_session(|session, _| usb::read_eeprom(session))?;
                            save::atomic_write(&path, &bytes)?;
                        }
                        Command::Import(image) => {
                            state.status = Some("Importing EEPROM…".into());
                            publish(tx, &state, &mut last_sent);
                            active.with_session(|session, info| {
                                usb::write_eeprom(session, &image)?;
                                *info = Info::read(session)?;
                                Ok(())
                            })?;
                        }
                        Command::Reset => active.with_session(|session, info| {
                            usb::reset_walker(session)?;
                            *info = Info::read(session)?;
                            Ok(())
                        })?,
                        Command::Update | Command::CleanInstall => {
                            let fresh = matches!(command, Command::CleanInstall);
                            let expected =
                                state.connection.info().and_then(|i| usb::mac_id(&i.mac));
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
                            let (port, active_session) = active.start_flash(title.into())?;
                            state.connection = active.presentation();
                            publish(tx, &state, &mut last_sent);
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
                                    active = ActiveDevice::Flashing {
                                        title: title.into(),
                                        port: port.clone(),
                                        step,
                                        percent,
                                    };
                                    state.connection = active.presentation();
                                    publish(tx, &state, &mut last_sent);
                                },
                            );
                            active = match result {
                                Ok((port, session, info)) => ActiveDevice::Managed {
                                    port,
                                    session,
                                    info: Box::new(info),
                                },
                                Err(error) => {
                                    active = ActiveDevice::probe(port, expected, false);
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
                    if revision.is_some() {
                        state.failed_revision = revision;
                    }
                    state.error = Some(error.to_string());
                }
                state.connection = active.presentation();
                drop(operation);
                state.hardware_busy = false;
                if !publish(tx, &state, &mut last_sent) {
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
                    if active.discover(&ports) {
                        state.firmware_feedback = None;
                    }
                    state.ports = ports;
                }
                Err(error) => state.error = Some(error.to_string()),
            }
        }
        if active
            .deadline()
            .is_some_and(|deadline| Instant::now() >= deadline)
        {
            active.try_open(&state.ports);
            poll = Instant::now() + Duration::from_millis(500);
        }
        if Instant::now() >= poll {
            active.poll();
            poll = Instant::now() + Duration::from_millis(500);
        }
        state.connection = active.presentation();
        if !publish(tx, &state, &mut last_sent) {
            break;
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
        let mut state = ActiveDevice::default();
        state.discover(&[a.clone(), b.clone()]);
        assert_eq!(state.presentation().port(), Some(&a));
        state.discover(&[b.clone(), a.clone()]);
        assert_eq!(state.presentation().port(), Some(&a));
        let moved = Port {
            path: "COM12".into(),
            serial: "70041ddc1a20".into(),
        };
        state.discover(&[b.clone(), moved.clone()]);
        assert_eq!(state.presentation().port(), Some(&moved));
        state.discover(std::slice::from_ref(&b));
        assert_eq!(state.presentation().port(), Some(&b));
        assert_eq!(b.label(std::slice::from_ref(&b)), "Stick 8B04");
        state.discover(&[]);
        assert!(matches!(
            state.presentation(),
            ConnectionState::Disconnected
        ));
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
            let _operation = HardwareOperation::begin(&flag).unwrap();
            assert!(flag.load(Ordering::SeqCst));
            panic!("operation failed");
        });
        assert!(!flag.load(Ordering::SeqCst));
    }
}
