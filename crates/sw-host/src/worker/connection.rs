use super::{ConnectionState, DeviceMode, Info, Port, Session, protocol, usb};
use anyhow::{Context, Result};
use std::time::{Duration, Instant};

/// The USB application handle exists only while its corresponding Stick is managed.
#[derive(Default)]
pub(super) enum ActiveDevice {
    #[default]
    Disconnected,
    Probing {
        port: Port,
        expected: Option<String>,
        until: Instant,
        next: Instant,
        identify: bool,
    },
    Unmanaged(Port),
    Unavailable {
        port: Port,
        message: String,
        next: Instant,
    },
    Managed {
        port: Port,
        session: Session,
        info: Box<Info>,
    },
    Flashing {
        port: Port,
        title: String,
        step: String,
        percent: Option<u8>,
    },
}

impl ActiveDevice {
    pub fn presentation(&self) -> ConnectionState {
        match self {
            Self::Disconnected => ConnectionState::Disconnected,
            Self::Probing { port, .. } => ConnectionState::Reconnecting { port: port.clone() },
            Self::Unmanaged(port) => ConnectionState::Unmanaged { port: port.clone() },
            Self::Unavailable { port, message, .. } => ConnectionState::Unavailable {
                port: port.clone(),
                message: message.clone(),
            },
            Self::Managed { port, info, .. } => {
                ConnectionState::managed(port.clone(), (**info).clone())
            }
            Self::Flashing {
                port,
                title,
                step,
                percent,
            } => ConnectionState::Flashing {
                port: port.clone(),
                title: title.clone(),
                step: step.clone(),
                percent: *percent,
            },
        }
    }

    fn port(&self) -> Option<&Port> {
        match self {
            Self::Disconnected => None,
            Self::Probing { port, .. }
            | Self::Unmanaged(port)
            | Self::Unavailable { port, .. }
            | Self::Managed { port, .. }
            | Self::Flashing { port, .. } => Some(port),
        }
    }

    pub fn probe(port: Port, expected: Option<String>, identify: bool) -> Self {
        let now = Instant::now();
        Self::Probing {
            port,
            expected,
            until: now + Duration::from_secs(3),
            next: now,
            identify,
        }
    }

    pub fn discover(&mut self, ports: &[Port]) -> bool {
        if self.port().is_some_and(|p| ports.contains(p)) {
            return false;
        }
        // Retain a briefly missing endpoint while CDC re-enumerates. Missing
        // descriptors are resolved by STATUS before binding a replacement port.
        if let Self::Probing {
            expected: Some(_),
            until,
            ..
        } = self
            && Instant::now() < *until
        {
            return false;
        }
        let mac = match self {
            Self::Managed { info, .. } => usb::mac_id(&info.mac),
            _ => self.port().and_then(|p| usb::mac_id(&p.serial)),
        };
        let old = self.port().cloned();
        let next = old
            .as_ref()
            .and_then(|p| ports.iter().find(|candidate| p.same_device(candidate)))
            .or_else(|| {
                ports
                    .iter()
                    .find(|p| mac.is_some() && usb::mac_id(&p.serial) == mac)
            })
            .cloned();
        if let (Some(port), Some(expected)) = (old.as_ref(), mac.as_ref())
            && matches!(self, Self::Managed { .. })
            && next.is_none()
        {
            *self = Self::probe(port.clone(), Some(expected.clone()), false);
            return true;
        }
        let choice = next.or_else(|| ports.first().cloned());
        if choice == old {
            return false;
        }
        *self = choice
            .map(|port| Self::probe(port, None, false))
            .unwrap_or_default();
        true
    }

    pub fn deadline(&self) -> Option<Instant> {
        match self {
            Self::Probing { next, .. } | Self::Unavailable { next, .. } => Some(*next),
            _ => None,
        }
    }

    pub fn try_open(&mut self, ports: &[Port]) {
        let now = Instant::now();
        if let Self::Unavailable { port, next, .. } = self
            && now >= *next
        {
            *self = Self::probe(port.clone(), None, false);
        }
        let Self::Probing {
            port,
            expected,
            until,
            next,
            identify,
        } = self
        else {
            return;
        };
        if now < *next {
            return;
        }
        *next = now + Duration::from_millis(400);
        let mut opened = false;
        let mut access_denied = false;
        // Prefer the selected port. Only probe alternatives to find a known MAC.
        let candidates = ports
            .iter()
            .filter(|p| *p == port)
            .chain(ports.iter().filter(|p| *p != port && expected.is_some()));
        for candidate in candidates {
            if let (Some(mac), Some(serial)) = (expected.as_ref(), usb::mac_id(&candidate.serial))
                && mac != &serial
            {
                continue;
            }
            match Session::open(&candidate.path) {
                Ok(mut session) => {
                    opened |= candidate == port;
                    if let Ok(info) = Info::read(&mut session) {
                        if expected
                            .as_ref()
                            .is_some_and(|id| usb::mac_id(&info.mac).as_ref() != Some(id))
                        {
                            continue;
                        }
                        if *identify && info.mode != DeviceMode::Linking {
                            let _ = session.request(protocol::IDENTIFY, &[]);
                        }
                        *self = Self::Managed {
                            port: candidate.clone(),
                            session,
                            info: Box::new(info),
                        };
                        return;
                    }
                }
                Err(error) => {
                    access_denied |= error.downcast_ref::<serialport::Error>().is_some_and(|e| {
                        e.kind() == serialport::ErrorKind::Io(std::io::ErrorKind::PermissionDenied)
                    })
                }
            }
        }
        if now < *until {
            return;
        }
        let port = port.clone();
        *self = if !ports.contains(&port) {
            ports
                .first()
                .cloned()
                .map(|p| Self::probe(p, None, false))
                .unwrap_or_default()
        } else if expected.is_some() {
            // A different board may now occupy the same COM number.
            Self::probe(port, None, false)
        } else if opened {
            Self::Unmanaged(port)
        } else {
            let message = if cfg!(target_os = "linux") && access_denied {
                "Install the included 60-stickwalker.rules file, then reconnect the Stick.".into()
            } else {
                format!(
                    "Close other apps using {} and reconnect the Stick.",
                    port.path
                )
            };
            Self::Unavailable {
                port,
                message,
                next: now + Duration::from_secs(8),
            }
        };
    }

    pub fn with_session<T>(
        &mut self,
        f: impl FnOnce(&mut Session, &mut Info) -> Result<T>,
    ) -> Result<T> {
        let Self::Managed {
            port,
            session,
            info,
        } = self
        else {
            anyhow::bail!("Connect a Stickwalker first.")
        };
        let result = f(session, info);
        if result
            .as_ref()
            .is_err_and(|error| error.downcast_ref::<std::io::Error>().is_some())
        {
            *self = Self::probe(port.clone(), usb::mac_id(&info.mac), false);
        }
        result
    }

    pub fn poll(&mut self) {
        if let Self::Managed {
            port,
            session,
            info,
        } = self
        {
            match Info::read(session) {
                Ok(new) if new.mac == info.mac => **info = new,
                _ => *self = Self::probe(port.clone(), usb::mac_id(&info.mac), false),
            }
        }
    }

    pub fn start_flash(&mut self, title: String) -> Result<(Port, Option<Session>)> {
        let port = self.port().context("Connect a StickS3 first")?.clone();
        let flashing = Self::Flashing {
            port: port.clone(),
            title,
            step: "Preparing Stickwalker".into(),
            percent: None,
        };
        let session = match std::mem::replace(self, flashing) {
            Self::Managed { session, .. } => Some(session),
            _ => None,
        };
        Ok((port, session))
    }
}
