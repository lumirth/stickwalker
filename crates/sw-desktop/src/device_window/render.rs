use crate::{
    device_window::{CheckFeedback, FileAction, PendingExit, StickView},
    updater::AppUpdate,
    *,
};
use gpui_kit::component::{progress::Progress, tooltip::Tooltip};
use host::worker::{Command, ConnectionState};
use std::sync::Arc;

#[derive(Clone, Debug, PartialEq, Action)]
#[action(no_json)]
struct SelectStick(host::usb::Port);
// Reserve the same line in each window, including when there is no message.
pub(crate) fn feedback(message: Option<String>, error: bool, cx: &App) -> Div {
    div()
        .h_flex()
        .h_8()
        .w_full()
        .flex_shrink_0()
        .px_3()
        .gap_2()
        .when_some(message, |row, text| {
            row.border_t_1().border_color(cx.theme().border).child(
                div()
                    .id("feedback-message")
                    .debug_selector(|| "feedback-message".into())
                    .role(if error { Role::Alert } else { Role::Status })
                    .aria_label(text.clone())
                    .flex_1()
                    .min_w_0()
                    .when_else(
                        error,
                        |d| d.line_clamp(2).line_height(rems(0.9)),
                        |d| d.truncate(),
                    )
                    .text_xs()
                    .text_color(if error {
                        cx.theme().danger
                    } else {
                        cx.theme().muted_foreground
                    })
                    .child(text.clone())
                    .tooltip(move |w, cx| Tooltip::new(text.clone()).build(w, cx)),
            )
        })
}
impl StickView {
    fn identity(&self, cx: &Context<Self>) -> AnyElement {
        let label = self.device_label();
        if self.state.ports.len() < 2 {
            return div().child(label).into_any_element();
        }
        Button::new("select-stick")
            .small()
            .ghost()
            .label(label)
            .child(assets::icon("chevron"))
            .disabled(self.locked())
            .on_click(cx.listener(|view, _, w, cx| {
                view.clear_local_error(cx);
                let mut menu = gpui_kit::component::native_menu::NativeMenu::new();
                for p in &view.state.ports {
                    menu = menu.menu(view.state.port_label(p), Box::new(SelectStick(p.clone())));
                }
                menu.show(w.mouse_position(), w, cx);
            }))
            .into_any_element()
    }
    fn band(&self, cx: &App) -> Div {
        div()
            .v_flex()
            .px_4()
            .py_4()
            .gap_3()
            .border_t_1()
            .border_color(cx.theme().border)
    }
    fn device_panel(&self, cx: &mut Context<Self>) -> AnyElement {
        if let ConnectionState::Flashing {
            title,
            step,
            percent,
            ..
        } = &self.state.connection
        {
            return div()
                .v_flex()
                .flex_1()
                .justify_center()
                .items_center()
                .text_center()
                .p_4()
                .gap_4()
                .child(title.clone())
                .child(
                    Progress::new("firmware-progress")
                        .value(percent.unwrap_or(0) as f32)
                        .loading(percent.is_none()),
                )
                .child(
                    div()
                        .text_sm()
                        .text_color(cx.theme().muted_foreground)
                        .child(step.clone()),
                )
                .into_any_element();
        }
        let Some(info) = self.state.connection.info() else {
            let connected = self.state.connection.port().is_some();
            let reconnecting =
                matches!(self.state.connection, ConnectionState::Reconnecting { .. });
            let unavailable =
                if let ConnectionState::Unavailable { message, .. } = &self.state.connection {
                    Some(message.as_str())
                } else {
                    None
                };
            let heading = if unavailable.is_some() {
                "Can’t open this Stick"
            } else if reconnecting {
                "Reconnecting…"
            } else if connected {
                "Set up Stickwalker"
            } else {
                "Connect your StickS3"
            };
            let explanation = if let Some(message) = unavailable {
                message
            } else if reconnecting || connected {
                ""
            } else {
                "Use a USB data cable to get started."
            };
            return div()
                .v_flex()
                .flex_1()
                .justify_center()
                .items_center()
                .text_center()
                .p_4()
                .gap_3()
                .when(connected, |d| d.child(self.identity(cx)))
                .child(div().font_weight(FontWeight::MEDIUM).child(heading))
                .child(
                    div()
                        .text_sm()
                        .text_color(cx.theme().muted_foreground)
                        .child(explanation.to_owned()),
                )
                .when(connected && !reconnecting && unavailable.is_none(), |d| {
                    d.child(
                        Button::new("install")
                            .primary()
                            .label(self.install_label())
                            .disabled(!cx.global::<AppState>().menu.can_install)
                            .on_click(cx.listener(|v, _, w, cx| v.install(w, cx))),
                    )
                })
                .into_any_element();
        };
        let linking = matches!(self.state.connection, ConnectionState::LinkingInGame { .. });
        let fault = matches!(self.state.connection, ConnectionState::SaveFault { .. });
        let version = format!("Firmware {}", info.version);
        let voltage = format!("Battery voltage: {:.2} V", info.millivolts as f32 / 1000.);
        let header = div()
            .v_flex()
            .px_4()
            .py_4()
            .gap_3()
            .child(
                div()
                    .h_flex()
                    .gap_3()
                    .child(
                        div()
                            .h_flex()
                            .gap_2()
                            .flex_1()
                            .child(div().size_2().rounded_full().bg(if fault {
                                cx.theme().danger
                            } else if linking {
                                cx.theme().warning
                            } else {
                                cx.theme().success
                            }))
                            .child(self.identity(cx))
                            .when(linking, |d| {
                                d.child(div().text_xs().child("Linking over IR…"))
                            }),
                    )
                    .child(
                        div()
                            .id("battery")
                            .h_flex()
                            .gap_1()
                            .child(
                                assets::battery(info.battery_percent(), info.power & 1 != 0)
                                    .size(px(16.)),
                            )
                            .text_xs()
                            .font_features(FontFeatures(Arc::new(vec![("tnum".into(), 1)])))
                            .text_color(cx.theme().muted_foreground)
                            .child(format!("{}%", info.battery_percent()))
                            .tooltip(move |w, cx| Tooltip::new(voltage.clone()).build(w, cx)),
                    ),
            )
            .child(
                div()
                    .h_flex()
                    .gap_3()
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .text_xs()
                            .text_color(cx.theme().muted_foreground)
                            .child(if self.state.firmware.is_some() && !self.offer_update() {
                                format!("{version} · Up to date")
                            } else {
                                version
                            }),
                    )
                    .when(self.offer_update(), |d| {
                        d.child(
                            Button::new("update")
                                .primary()
                                .small()
                                .label(self.install_label())
                                .disabled(!cx.global::<AppState>().menu.can_install)
                                .on_click(cx.listener(|v, _, w, cx| v.install(w, cx))),
                        )
                    }),
            );
        let mut body = div().v_flex().child(header);
        if fault {
            return body
                .child(
                    self.band(cx)
                        .child("Save data couldn’t be opened")
                        .child(
                            div()
                                .text_sm()
                                .text_color(cx.theme().muted_foreground)
                                .child("Import a saved EEPROM or reset your Stick to start fresh."),
                        )
                        .child(
                            div()
                                .h_flex()
                                .items_center()
                                .gap_2()
                                .child(
                                    Button::new("recovery-import")
                                        .small()
                                        .label("Import EEPROM")
                                        .disabled(self.locked())
                                        .on_click(cx.listener(|v, _, w, cx| {
                                            v.files(FileAction::Import, w, cx)
                                        })),
                                )
                                .child(
                                    Button::new("reset")
                                        .small()
                                        .label("Reset Walker")
                                        .disabled(self.locked())
                                        .on_click(cx.listener(|v, _, w, cx| v.reset(false, w, cx))),
                                ),
                        ),
                )
                .into_any_element();
        }
        body = body.child(
            self.band(cx).child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(div().flex_1().child("Palette"))
                    .child(
                        div()
                            .w(rems(13.))
                            .flex_shrink_0()
                            .child(self.palette.clone()),
                    )
                    .child(
                        Button::new("edit-palette")
                            .small()
                            .label("Edit")
                            .disabled(!self.editable())
                            .on_click(cx.listener(|v, _, _, cx| v.open_palette(false, cx))),
                    ),
            ),
        );
        body.child(
            self.band(cx).child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(div().flex_1().child("Save"))
                    .child(
                        Button::new("export")
                            .debug_selector(|| "export-save".into())
                            .small()
                            .label("Export EEPROM")
                            .disabled(!self.editable())
                            .on_click(
                                cx.listener(|v, _, w, cx| v.files(FileAction::Export, w, cx)),
                            ),
                    )
                    .child(
                        Button::new("import")
                            .small()
                            .label("Import EEPROM")
                            .disabled(!self.editable())
                            .on_click(
                                cx.listener(|v, _, w, cx| v.files(FileAction::Import, w, cx)),
                            ),
                    ),
            ),
        )
        .into_any_element()
    }

    fn footer_message(&self) -> Option<(FooterKind, String)> {
        use FooterKind::{Error, Restart, Status, Update};
        if !self.updates.blocks_hardware() {
            if let Some(error) = self.local_error.as_ref().or(self.state.error.as_ref()) {
                return Some((Error, error.clone()));
            }
            if let Some(status) = self.state.status.as_deref() {
                return Some((Status, status.into()));
            }
            if let Some(install) = &self.pending_install {
                return Some((Status, install.status().into()));
            }
            if self.state.checking_firmware {
                return Some((Status, "Checking for firmware updates…".into()));
            }
        }
        let update = &self.updates.state;
        let message = match update {
            AppUpdate::Available(release) => {
                return Some((
                    Update,
                    format!("Stickwalker Desktop {} is available", release.version),
                ));
            }
            AppUpdate::ReadyToRestart { .. } => return Some((Restart, "App update ready".into())),
            AppUpdate::Failed(error) => return Some((Error, error.clone())),
            AppUpdate::Restarting => "Restarting to update…".into(),
            AppUpdate::Downloading { percent, .. } => percent
                .map(|p| format!("Downloading app update · {p}%"))
                .unwrap_or_else(|| "Downloading app update…".into()),
            #[cfg(not(target_os = "macos"))]
            AppUpdate::ReadyToInstall(_) => "App update downloaded".into(),
            AppUpdate::Installing => "Installing app update…".into(),
            AppUpdate::Checking { background: false }
                if self.latest_check == CheckFeedback::App =>
            {
                "Checking for app updates…".into()
            }
            AppUpdate::UpToDate if self.latest_check == CheckFeedback::App => {
                "Stickwalker Desktop is up to date".into()
            }
            _ if self.latest_check == CheckFeedback::Firmware => {
                self.state.firmware_feedback.clone()?
            }
            _ => return None,
        };
        Some((Status, message))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FooterKind {
    Status,
    Error,
    Update,
    Restart,
}

impl Render for StickView {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let content = self.device_panel(cx);
        let (kind, message) = self.footer_message().unzip();
        let has_error = kind == Some(FooterKind::Error);
        let show_update = matches!(kind, Some(FooterKind::Update | FooterKind::Restart));
        let footer = feedback(message, has_error, cx)
            .when(has_error, |row| {
                row.child(
                    Button::new("dismiss-error")
                        .xsmall()
                        .ghost()
                        .label("Dismiss")
                        .on_click(cx.listener(move |v, _, _, cx| {
                            if v.local_error.take().is_none() {
                                if v.state.error.take().is_some() {
                                    v.send(Command::DismissError);
                                } else {
                                    v.updates.state = AppUpdate::Idle;
                                }
                            }
                            v.refresh_shell(cx);
                        })),
                )
            })
            .when(show_update, |row| {
                row.child(
                    Button::new("app-update")
                        .xsmall()
                        .label(if kind == Some(FooterKind::Restart) {
                            "Restart"
                        } else {
                            "Update app"
                        })
                        .disabled(self.locked())
                        .on_click(cx.listener(|v, _, _, cx| v.app_update(cx))),
                )
            });
        div()
            .v_flex()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .text_sm()
            .track_focus(&self.focus)
            .key_context("Stickwalker")
            .on_action(cx.listener(|v, _: &CloseWindow, _w, cx| {
                v.request_exit(PendingExit::Quit, cx);
            }))
            .on_action(
                cx.listener(|v, action: &SelectStick, w, cx| v.select(action.0.clone(), w, cx)),
            )
            .on_action(cx.listener(|v, _: &ExportEeprom, w, cx| v.files(FileAction::Export, w, cx)))
            .on_action(cx.listener(|v, _: &ImportEeprom, w, cx| v.files(FileAction::Import, w, cx)))
            .on_action(cx.listener(|v, _: &AppearanceSystem, w, cx| {
                v.clear_local_error(cx);
                preferences::set_appearance(None, w, cx)
            }))
            .on_action(cx.listener(|v, _: &AppearanceLight, w, cx| {
                v.clear_local_error(cx);
                preferences::set_appearance(Some(false), w, cx)
            }))
            .on_action(cx.listener(|v, _: &AppearanceDark, w, cx| {
                v.clear_local_error(cx);
                preferences::set_appearance(Some(true), w, cx)
            }))
            .on_action(cx.listener(|v, _: &CopyPalette, _, cx| v.copy_palette(cx)))
            .on_action(cx.listener(|v, _: &PastePalette, _, cx| v.open_palette(true, cx)))
            .on_action(cx.listener(|v, _: &EditPalette, _, cx| v.open_palette(false, cx)))
            .on_action(cx.listener(|v, _: &ImportPalettes, w, cx| {
                v.palette_file(crate::palette_window::PaletteAction::Import, w, cx)
            }))
            .on_action(cx.listener(|v, _: &ExportPalettes, w, cx| {
                v.palette_file(crate::palette_window::PaletteAction::Export, w, cx)
            }))
            .map(|d| {
                #[cfg(debug_assertions)]
                let d = d.on_action(cx.listener(
                    |v, action: &crate::debug_states::ShowState, w, cx| {
                        v.show_state(action.0, w, cx)
                    },
                ));
                d
            })
            .on_action(cx.listener(|v, _: &ResetWalker, w, cx| v.reset(false, w, cx)))
            .on_action(cx.listener(|v, _: &EraseInstall, w, cx| v.reset(true, w, cx)))
            .on_action(cx.listener(|v, _: &OpenPackage, w, cx| v.files(FileAction::Package, w, cx)))
            .on_action(cx.listener(|v, _: &CheckFirmware, _, cx| {
                if !v.locked() && !v.state.checking_firmware {
                    v.clear_local_error(cx);
                    v.latest_check = CheckFeedback::Firmware;
                    v.send(Command::CheckFirmware);
                    cx.notify();
                }
            }))
            .on_action(cx.listener(|v, _: &Install, w, cx| v.install(w, cx)))
            .on_action(cx.listener(|v, _: &CheckAppUpdates, _, cx| v.app_update(cx)))
            .child(
                TitleBar::new()
                    .on_close_window(
                        cx.listener(|v, _, _, cx| v.request_exit(PendingExit::Quit, cx)),
                    )
                    .when(cfg!(target_os = "macos"), |bar| {
                        bar.child(div().flex_1().child({
                            #[cfg(debug_assertions)]
                            {
                                self.preview
                                    .map(|n| format!("Stickwalker · Preview {n}"))
                                    .unwrap_or_else(|| "Stickwalker".into())
                            }
                            #[cfg(not(debug_assertions))]
                            {
                                "Stickwalker".to_owned()
                            }
                        }))
                    })
                    .when(!cfg!(target_os = "macos"), |bar| {
                        bar.child(cx.global::<AppState>().menu_bar.clone().unwrap())
                    }),
            )
            .child(
                div()
                    .v_flex()
                    .flex_1()
                    .min_h_0()
                    .id("content")
                    .overflow_y_scroll()
                    .child(content),
            )
            .child(footer)
    }
}

#[cfg(all(test, debug_assertions))]
mod tests {
    use crate::{device_window::StickView, updater::AppUpdate};
    use gpui_kit::{
        AppContext, Bounds, Point, TestAppContext, VisualTestContext, WindowBounds, WindowOptions,
        px, size,
    };

    #[gpui_kit::test]
    fn footer_prioritizes_firmware_activity_and_blocking_app_updates(cx: &mut TestAppContext) {
        use super::FooterKind;
        use crate::device_window::tests::open_device;
        use gpui_updater_core::{Asset, Release};
        use host::worker::{Command, Presentation};

        let (window, view) = open_device(cx);
        let mut ui = VisualTestContext::from_window(window, cx);
        let release = Release::new("1.2.3".parse().unwrap(), Asset::new("Stickwalker", "", 0));
        let banner = Some((
            FooterKind::Update,
            "Stickwalker Desktop 1.2.3 is available".into(),
        ));
        ui.update(|w, cx| {
            view.update(cx, |v, cx| {
                v.show_state(4, w, cx);
                v.preview = None;
                v.state.firmware = None;
                v.updates.state = AppUpdate::Available(Box::new(release));
                assert_eq!(v.footer_message(), banner);
                v.state.checking_firmware = true;
                assert_eq!(
                    v.footer_message(),
                    Some((FooterKind::Status, "Checking for firmware updates…".into()))
                );
                v.state.checking_firmware = false;
                cx.notify();
            });
            w.draw(cx).clear(cx);
        });
        let banner_bounds = ui.debug_bounds("feedback-message").unwrap();
        let (_download, result) = async_channel::bounded(1);
        ui.update(|w, cx| {
            view.update(cx, |v, cx| {
                v.await_firmware(
                    v.state.connection.target().unwrap(),
                    Command::Update,
                    result,
                    cx,
                );
                v.refresh_shell(cx);
                assert_eq!(
                    v.footer_message(),
                    Some((FooterKind::Status, "Downloading firmware…".into()))
                );
            });
            w.draw(cx).clear(cx);
        });
        assert!(ui.debug_bounds("feedback-message").unwrap().size.width > banner_bounds.size.width);
        ui.update(|w, cx| {
            view.update(cx, |v, cx| {
                let available = std::mem::replace(&mut v.updates.state, AppUpdate::Installing);
                assert_eq!(
                    v.footer_message(),
                    Some((FooterKind::Status, "Installing app update…".into()))
                );
                v.updates.state = AppUpdate::Restarting;
                assert_eq!(
                    v.footer_message(),
                    Some((FooterKind::Status, "Restarting to update…".into()))
                );
                v.updates.state = available;
                v.present(Presentation::default(), w, cx);
                assert_eq!(v.footer_message(), banner);
            });
            w.draw(cx).clear(cx);
        });
        assert_eq!(ui.debug_bounds("feedback-message").unwrap(), banner_bounds);
    }

    #[gpui_kit::test]
    fn errors_and_update_notices_leave_save_actions_in_place(cx: &mut TestAppContext) {
        let (handle, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                        Point::default(),
                        size(px(460.), px(310.)),
                    ))),
                    ..Default::default()
                },
                cx,
                |w, cx| cx.new(|cx| StickView::new(w, cx)),
            )
            .unwrap()
        });
        let mut ui = VisualTestContext::from_window(handle, cx);
        ui.update(|w, cx| {
            view.update(cx, |v, cx| v.show_state(4, w, cx));
            w.draw(cx).clear(cx);
        });
        let before = ui.debug_bounds("export-save").unwrap();
        ui.update(|w, cx| {
            view.update(cx, |v, cx| {
                v.updates
                    .accept(crate::updater::UpdateEvent::Native(AppUpdate::Failed(
                        "The update feed is unavailable.".into(),
                    )));
                v.state.error = Some("The selected file could not be opened. ".repeat(8));
                cx.notify();
            });
            w.draw(cx).clear(cx);
        });
        assert_eq!(ui.debug_bounds("export-save").unwrap(), before);
        assert!(before.bottom() < px(310. - 32.));
        let message = ui.update(|w, cx| {
            view.update(cx, |v, cx| {
                v.state.error = None;
                v.updates.state = AppUpdate::Checking { background: false };
                v.updates
                    .accept(crate::updater::UpdateEvent::Driver(Box::new(
                        gpui_updater_core::DriverEvent::Failed(
                            gpui_updater_core::UpdateError::new(
                                gpui_updater_core::ErrorKind::Other,
                                "An error occurred in retrieving update information.",
                            ),
                        ),
                    )));
                cx.notify();
            });
            w.draw(cx).clear(cx);
            let AppUpdate::Failed(text) = &view.read(cx).updates.state else {
                panic!("manual check should show feedback")
            };
            assert_eq!(text, "Couldn’t check for updates.");
            w.text_system()
                .shape_line(
                    text.clone().into(),
                    px(16.),
                    &[w.text_style().to_run(text.len())],
                    None,
                )
                .width
        });
        assert!(message <= ui.debug_bounds("feedback-message").unwrap().size.width);
    }
    #[gpui_kit::test]
    fn device_actions_follow_the_connection(cx: &mut TestAppContext) {
        let (window, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
                cx.new(|cx| StickView::new(w, cx))
            })
            .unwrap()
        });
        for (state, install, open, export, caption) in [
            (1, false, false, false, "No Device Connected"),
            (2, true, true, false, "Install Stickwalker "),
            (4, false, true, true, "Firmware Up to Date"),
            (5, true, true, true, "Update Firmware to "),
        ] {
            cx.update_window(window, |_, w, cx| {
                view.update(cx, |v, cx| v.show_state(state, w, cx));
                w.draw(cx).clear(cx);
                let menus = cx.get_menus().unwrap();
                let item = |menu_name: &str, action: &dyn gpui_kit::Action| {
                    menus
                        .iter()
                        .find(|menu| menu.name.as_ref() == menu_name)
                        .unwrap()
                        .items
                        .iter()
                        .find_map(|item| match item {
                            gpui_kit::OwnedMenuItem::Action {
                                action: bound,
                                name,
                                disabled,
                                ..
                            } if bound.partial_eq(action) => Some((name.as_str(), !disabled)),
                            _ => None,
                        })
                        .unwrap()
                };
                let (name, enabled) = item("Device", &crate::Install);
                assert!(name.starts_with(caption), "{name}");
                assert_eq!(enabled, install);
                assert_eq!(item("Device", &crate::OpenPackage).1, open);
                assert_eq!(item("File", &crate::ExportEeprom).1, export);
                assert_eq!(item("Edit", &crate::EditPalette).1, export);
            })
            .unwrap();
        }
        cx.update_window(window, |_, w, cx| {
            view.update(cx, |v, cx| {
                v.show_state(4, w, cx);
                let mut state = v.state.clone();
                state.firmware = None;
                v.present(state, w, cx);
                let menu = &cx.global::<crate::AppState>().menu;
                assert!(!menu.can_install);
                assert_eq!(menu.install_label, "Update Firmware");
            });
        })
        .unwrap();
    }

    #[gpui_kit::test]
    fn the_editor_survives_unplugging_its_stick(cx: &mut TestAppContext) {
        use host::worker::ConnectionState;
        let (handle, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
                cx.new(|cx| StickView::new(w, cx))
            })
            .unwrap()
        });
        cx.update_window(handle, |_, w, cx| {
            view.update(cx, |v, cx| {
                v.show_state(4, w, cx);
                v.open_palette(false, cx);
                let original = v.editor.as_ref().unwrap().0;
                let mut snapshot = v.state.clone();
                let ready = snapshot.connection.clone();
                snapshot.connection = ConnectionState::Disconnected;
                v.present(snapshot.clone(), w, cx);
                assert_eq!(v.editor.as_ref().unwrap().0, original);
                snapshot.connection = ready;
                v.present(snapshot.clone(), w, cx);
                assert_eq!(v.editor.as_ref().unwrap().0, original);
                snapshot.connection.info_mut().unwrap().mac = "0A:00:01:02:03:04".into();
                v.present(snapshot, w, cx);
                assert!(v.editor.is_none());
            });
        })
        .unwrap();
    }

    #[gpui_kit::test]
    fn close_respects_the_hardware_lock_and_a_draining_restart(cx: &mut TestAppContext) {
        use crate::device_window::{ExitState, PendingExit};
        let (handle, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
                cx.new(|cx| StickView::new(w, cx))
            })
            .unwrap()
        });
        cx.update_window(handle, |_, w, cx| {
            view.update(cx, |v, cx| {
                v.show_state(5, w, cx);
                v.state.hardware_busy = true;
                v.request_exit(PendingExit::Quit, cx);
                assert_eq!(v.exit, ExitState::Running);
                v.state.hardware_busy = false;
                v.pending_revision = Some(v.state.acked_revision + 1);
                v.request_exit(PendingExit::Restart, cx);
                v.request_exit(PendingExit::Quit, cx);
                assert_eq!(v.exit, ExitState::Draining(PendingExit::Restart));
            });
        })
        .unwrap();
    }

    #[gpui_kit::test]
    fn an_invalid_save_is_rejected_before_asking_to_replace_the_walk(cx: &mut TestAppContext) {
        let (handle, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
                cx.new(|cx| StickView::new(w, cx))
            })
            .unwrap()
        });
        cx.update_window(handle, |_, w, cx| {
            view.update(cx, |v, cx| v.show_state(4, w, cx));
            w.dispatch_action(Box::new(crate::ImportEeprom), cx);
        })
        .unwrap();
        cx.run_until_parked();
        let file = tempfile::NamedTempFile::new().unwrap();
        cx.simulate_path_prompt_response(|_| Some(vec![file.path().to_owned()]));
        cx.run_until_parked();
        assert!(!cx.has_pending_prompt());
        cx.update_window(handle, |_, w, cx| {
            view.update(cx, |v, cx| {
                let mut next = v.state.clone();
                next.connection.info_mut().unwrap().millivolts += 1;
                v.preview = None;
                v.receive(next, w, cx);
                assert!(
                    v.local_error
                        .as_deref()
                        .unwrap()
                        .contains("Pokéwalker save")
                );
                v.copy_palette(cx);
                assert!(v.local_error.is_none());
            });
        })
        .unwrap();
    }

    #[gpui_kit::test]
    fn file_menu_import_opens_only_the_file_picker(cx: &mut TestAppContext) {
        let (handle, view) = cx.update(|cx| {
            gpui_kit::init(cx);
            gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
                cx.new(|cx| StickView::new(w, cx))
            })
            .unwrap()
        });
        cx.update_window(handle, |_, w, cx| {
            view.update(cx, |v, cx| v.show_state(4, w, cx));
            w.dispatch_action(Box::new(crate::ImportPalettes), cx);
        })
        .unwrap();
        cx.run_until_parked();
        assert!(cx.did_prompt_for_paths());
        cx.update(|cx| assert_eq!(cx.windows().len(), 1));
    }
}
