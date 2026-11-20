//! Debug-only presentations. All device writes are suppressed while a preview is active.
use crate::device_window::StickView;
use crate::*;
use host::{
    DeviceSettings,
    usb::{DeviceMode, Info, Port},
    worker::{ConnectionState, Presentation},
};

#[derive(Clone, Debug, PartialEq, Action)]
#[action(no_json)]
pub struct ShowState(pub u8);
pub fn bind(cx: &mut App) {
    for (number, key) in [
        "secondary-0",
        "secondary-1",
        "secondary-2",
        "secondary-3",
        "secondary-4",
        "secondary-5",
        "secondary-6",
        "secondary-7",
        "secondary-8",
    ]
    .into_iter()
    .enumerate()
    {
        let binding = shortcut(key, ShowState(number as u8), Some("Stickwalker"), cx);
        cx.bind_keys([binding]);
    }
}
impl StickView {
    pub(super) fn show_state(&mut self, number: u8, w: &mut Window, cx: &mut Context<Self>) {
        if self.locked() {
            return;
        }
        if let Some(live) = self.live_update.take() {
            self.updates.state = live;
        }
        if number != 0 {
            self.live_update = Some(self.updates.state.clone());
            let state = if number == 5 {
                crate::updater::AppUpdate::ReadyToRestart { path: None }
            } else {
                crate::updater::AppUpdate::Idle
            };
            self.updates
                .accept(crate::updater::UpdateEvent::Native(state));
        }
        self.preview = (number != 0).then_some(number);
        if number == 0 {
            self.present(self.live_state.clone(), w, cx);
            return;
        }
        let port = Port {
            path: "Preview StickS3".into(),
            serial: "70:04:1D:DC:1A:20".into(),
        };
        let info = Info {
            version: env!("CARGO_PKG_VERSION").into(),
            build: "preview".into(),
            mac: port.serial.clone(),
            mode: DeviceMode::Ready,
            millivolts: 4190,
            settings: DeviceSettings::default(),
            power: 1,
        };
        let mut state = Presentation {
            ports: vec![port.clone()],
            firmware: Some(host::worker::Firmware {
                version: env!("CARGO_PKG_VERSION").parse().unwrap(),
            }),
            ..Default::default()
        };
        state.connection = match number {
            1 => {
                state.ports.clear();
                ConnectionState::Disconnected
            }
            2 => ConnectionState::Unmanaged { port },
            3 => ConnectionState::Flashing {
                port,
                title: "Installing Stickwalker…".into(),
                step: "Writing firmware · 62%".into(),
                percent: Some(62),
            },
            6 => ConnectionState::LinkingInGame {
                port,
                info: Info {
                    mode: DeviceMode::Linking,
                    ..info
                },
            },
            7 => ConnectionState::SaveFault {
                port,
                info: Info {
                    mode: DeviceMode::SaveFault,
                    ..info
                },
            },
            _ => ConnectionState::Ready { port, info },
        };
        if number == 5 {
            let current = host::release::Version::parse(env!("CARGO_PKG_VERSION")).unwrap();
            let newer = host::release::Version::new(current.major, current.minor + 1, 0);
            state.firmware = Some(host::worker::Firmware { version: newer });
        }
        if number == 8 {
            state.ports.push(Port {
                path: "Second preview StickS3".into(),
                serial: "70:04:1D:DC:8B:04".into(),
            });
            state.error =
                Some("Export interrupted. Reconnect your Stick and try again.".into());
        }
        self.present(state, w, cx);
    }
}
