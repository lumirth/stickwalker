mod connection;
mod firmware;

use crate::{
    DeviceSettings,
    protocol::{self, Session},
    release::{Package, Version},
    save,
    usb::{self, DeviceMode, Info, Port},
};
use anyhow::{Context, Result};
use connection::ActiveDevice;
use firmware::{FirmwareUpdates, Request};
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
            Self::Update | Self::CleanInstall => access.can_install(),
            _ => true,
        }
    }
    fn targets_device(&self) -> bool {
        self.hardware_operation() || matches!(self, Self::SetSettings { .. })
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
enum Message {
    Command(Box<Work>),
    FirmwareReady,
}

#[derive(Clone)]
pub struct Commands {
    tx: Sender<Message>,
    busy: Arc<AtomicBool>,
}
impl Commands {
    pub fn send(&self, target: Option<DeviceTarget>, command: Command) -> Result<()> {
        let operation = command
            .hardware_operation()
            .then(|| HardwareOperation::begin(&self.busy))
            .transpose()?;
        self.tx
            .send(Message::Command(Box::new(Work {
                target,
                command,
                operation,
            })))
            .map_err(|_| anyhow::anyhow!("The Stick connection closed."))
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
    pub fn installable_on(&self, installed: &str) -> bool {
        self.local()
            || Version::parse(self.version())
                .ok()
                .zip(Version::parse(installed).ok())
                .is_none_or(|(next, current)| next.cmp_precedence(&current).is_gt())
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
        let busy = hardware_busy.clone();
        let wake = commands.clone();
        thread::spawn(move || {
            loop {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(&rx, &wake, &tx, &stop, &busy)
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
) {
    if last.as_ref() != Some(state) && tx.try_send(state.clone()).is_ok() {
        *last = Some(state.clone());
    }
}

struct WorkerState<'a> {
    active: ActiveDevice,
    firmware: FirmwareUpdates,
    state: Presentation,
    last_sent: Option<Presentation>,
    discover: Instant,
    poll: Instant,
    events: &'a async_channel::Sender<Presentation>,
    wake: &'a Sender<Message>,
    busy: &'a AtomicBool,
}

impl WorkerState<'_> {
    fn fetch(&mut self, reason: Request) {
        self.firmware.request(reason, self.wake, Package::latest);
    }

    fn publish(&mut self) {
        self.state.connection = self.active.presentation();
        self.state.firmware = self.firmware.selection();
        self.state.hardware_busy = self.busy.load(Ordering::Acquire);
        publish(self.events, &self.state, &mut self.last_sent);
    }

    fn deadline(&self) -> Instant {
        let next = if matches!(self.active, ActiveDevice::Managed { .. }) {
            self.discover.min(self.poll)
        } else {
            self.discover
        };
        self.active.deadline().map_or(next, |time| time.min(next))
    }

    fn tick(&mut self) {
        let now = Instant::now();
        if now >= self.discover {
            self.discover = now + Duration::from_secs(2);
            match usb::ports() {
                Ok(ports) => {
                    if ports.iter().any(|port| !self.state.ports.contains(port)) {
                        self.fetch(Request::Background);
                    }
                    if self.active.discover(&ports) {
                        self.state.firmware_feedback = None;
                    }
                    self.state.ports = ports;
                }
                Err(error) => self.state.error = Some(error.to_string()),
            }
        }
        if self
            .active
            .deadline()
            .is_some_and(|deadline| now >= deadline)
        {
            self.active.try_open(&self.state.ports);
            self.poll = Instant::now() + Duration::from_millis(500);
        }
        if now >= self.poll {
            self.active.poll();
            self.poll = Instant::now() + Duration::from_millis(500);
        }
    }

    fn firmware_ready(&mut self) -> Option<Box<Work>> {
        let (reason, result) = self.firmware.finish()?;
        self.state.checking_firmware = false;
        match reason {
            Request::Background => {}
            Request::Check => {
                self.state.firmware_feedback = None;
                match result {
                    Ok(()) => {
                        let latest = self.firmware.release_version()?;
                        let feedback = match self.state.connection.info() {
                            Some(info)
                                if Version::parse(&info.version)
                                    .is_ok_and(|v| !latest.cmp_precedence(&v).is_gt()) =>
                            {
                                format!("Firmware {} is up to date", info.version)
                            }
                            _ => format!("Firmware {latest} is available"),
                        };
                        self.state.firmware_feedback = Some(feedback);
                    }
                    Err(_) => {
                        self.state.error = Some("Couldn’t check for firmware updates.".into())
                    }
                }
            }
            Request::Install(work) => {
                self.state.status = None;
                if result.is_ok() {
                    self.discover = Instant::now();
                    self.poll = self.discover;
                    return Some(work);
                }
                self.state.error = Some("Couldn’t download firmware. Check your internet connection or open a local firmware file.".into());
            }
        }
        None
    }

    fn handle_command(&mut self, work: Box<Work>) {
        if work.command.targets_device()
            && (work.target != self.state.connection.target()
                || !work.command.allowed(&self.state.connection))
        {
            if let Command::SetSettings { revision, .. } = work.command {
                self.state.failed_revision = Some(revision);
            }
            self.state.error = Some("The Stick is no longer available for this action.".into());
            return;
        }
        self.state.error = None;
        if matches!(work.command, Command::Update | Command::CleanInstall)
            && self.firmware.selected().is_none()
        {
            self.state.status = Some("Downloading firmware…".into());
            self.state.firmware_feedback = None;
            self.fetch(Request::Install(work));
            return;
        }
        if work.operation.is_some() {
            self.state.status = match work.command {
                Command::Export(_) => Some("Exporting EEPROM…".into()),
                Command::Import(_) => Some("Importing EEPROM…".into()),
                _ => None,
            };
            self.publish();
        }
        let revision = match work.command {
            Command::SetSettings { revision, .. } => Some(revision),
            _ => None,
        };
        if let Err(error) = self.execute(work.command) {
            if let Some(revision) = revision {
                self.state.failed_revision = Some(revision);
            }
            self.state.error = Some(error.to_string());
        }
        if work.operation.is_some() {
            self.state.status = None;
        }
    }

    fn execute(&mut self, command: Command) -> Result<()> {
        match command {
            Command::Shutdown | Command::DismissError => {}
            Command::Select(port) => {
                if self.state.ports.contains(&port) {
                    self.active = ActiveDevice::probe(port, None, true);
                    self.state.acked_revision = 0;
                    self.state.failed_revision = None;
                    self.discover = Instant::now();
                }
            }
            Command::CheckFirmware => {
                self.state.checking_firmware = true;
                self.state.firmware_feedback = Some("Checking for firmware updates…".into());
                self.fetch(Request::Check);
            }
            Command::OpenPackage(path) => {
                self.firmware.open(&path)?;
                self.state.firmware_feedback = None;
            }
            Command::SetSettings { revision, settings } => {
                self.active.with_session(|session, info| {
                    session.request(protocol::SETTINGS, &settings.palette_bytes())?;
                    info.settings.active_palette = settings.active_palette;
                    info.settings.slots = settings.slots;
                    Ok(())
                })?;
                self.state.acked_revision = revision;
                self.state.failed_revision = None;
            }
            Command::Export(path) => {
                let bytes = self
                    .active
                    .with_session(|session, _| usb::read_eeprom(session))?;
                save::atomic_write(&path, &bytes)?;
            }
            Command::Import(image) => self.active.with_session(|session, info| {
                usb::write_eeprom(session, &image)?;
                *info = Info::read(session)?;
                Ok(())
            })?,
            Command::Reset => self.active.with_session(|session, info| {
                usb::reset_walker(session)?;
                *info = Info::read(session)?;
                Ok(())
            })?,
            Command::Update => self.flash(false)?,
            Command::CleanInstall => self.flash(true)?,
        }
        Ok(())
    }

    fn flash(&mut self, fresh: bool) -> Result<()> {
        let package = self
            .firmware
            .selected()
            .context("Choose a firmware file to install.")?;
        let info = self.state.connection.info();
        if !fresh
            && let Some(info) = info
            && self
                .firmware
                .selection()
                .is_some_and(|f| !f.installable_on(&info.version))
        {
            self.state.firmware_feedback = Some(format!("Firmware {} is up to date", info.version));
            return Ok(());
        }
        let expected = info.and_then(|i| usb::mac_id(&i.mac));
        let title = if fresh || info.is_none() {
            "Installing Stickwalker…"
        } else if info.is_some_and(|i| i.version == package.manifest.version.to_string()) {
            "Reinstalling Stickwalker…"
        } else {
            "Updating Stickwalker…"
        };
        let (port, session) = self.active.start_flash(title.into())?;
        self.state.firmware_feedback = None;
        self.state.connection = self.active.presentation();
        publish(self.events, &self.state, &mut self.last_sent);
        let result =
            crate::flash::install(&port, session, package, fresh, &mut |phase, percent| {
                let step = match percent {
                    Some(p) => format!("{phase} · {p}%"),
                    None => phase.into(),
                };
                self.state.connection = ConnectionState::Flashing {
                    title: title.into(),
                    port: port.clone(),
                    step,
                    percent,
                };
                publish(self.events, &self.state, &mut self.last_sent);
            });
        self.active = match result {
            Ok((port, session, info)) => ActiveDevice::Managed {
                port,
                session,
                info: Box::new(info),
            },
            Err(error) => {
                self.active = ActiveDevice::probe(port, expected, false);
                return Err(error);
            }
        };
        self.discover = Instant::now();
        Ok(())
    }
}

fn run(
    rx: &Receiver<Message>,
    wake: &Sender<Message>,
    events: &async_channel::Sender<Presentation>,
    stop: &AtomicBool,
    busy: &AtomicBool,
) {
    let mut worker = WorkerState {
        active: ActiveDevice::Disconnected,
        firmware: FirmwareUpdates::new(crate::data_dir().map(|d| d.join("firmware"))),
        state: Presentation::default(),
        last_sent: None,
        discover: Instant::now(),
        poll: Instant::now(),
        events,
        wake,
        busy,
    };
    worker.fetch(Request::Background);
    let mut deferred = None;
    let frame = Duration::from_millis(33);
    let mut last_settings = Instant::now() - frame;
    while !events.is_closed() && !stop.load(Ordering::Acquire) {
        let wait = worker.deadline().saturating_duration_since(Instant::now());
        match deferred
            .take()
            .map(Ok)
            .unwrap_or_else(|| rx.recv_timeout(wait))
        {
            Ok(Message::FirmwareReady) => {
                deferred = worker.firmware_ready().map(Message::Command);
            }
            Ok(Message::Command(mut work)) => {
                if matches!(work.command, Command::Shutdown) {
                    return;
                }
                if matches!(work.command, Command::SetSettings { .. }) {
                    thread::sleep(frame.saturating_sub(last_settings.elapsed()));
                    last_settings = Instant::now();
                    while let Ok(next) = rx.try_recv() {
                        match next {
                            Message::Command(newer)
                                if newer.target == work.target
                                    && matches!(newer.command, Command::SetSettings { .. }) =>
                            {
                                work = newer
                            }
                            other => {
                                deferred = Some(other);
                                break;
                            }
                        }
                    }
                }
                worker.handle_command(work);
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        worker.tick();
        worker.publish();
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
