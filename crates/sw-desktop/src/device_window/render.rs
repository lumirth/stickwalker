use crate::{
    device_window::{FileAction, StickView},
    updater::AppUpdate,
    *,
};
use gpui_kit::component::{progress::Progress, tooltip::Tooltip};
use host::worker::{Command, ConnectionState};

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
                    .truncate()
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
            } else if reconnecting {
                ""
            } else if connected {
                "Existing Stickwalker saves and settings are kept."
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
                            .disabled(self.locked())
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
                            .child(if self.update_available() {
                                version
                            } else {
                                format!("{version} · Up to date")
                            }),
                    )
                    .when(self.update_available(), |d| {
                        d.child(
                            Button::new("update")
                                .primary()
                                .small()
                                .label(self.install_label())
                                .disabled(self.locked() || linking)
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
                                .child(
                                    "Import an EEPROM image or reset the walker to start fresh.",
                                ),
                        )
                        .child(
                            div()
                                .h_flex()
                                .items_center()
                                .gap_2()
                                .child(
                                    Button::new("recovery-import")
                                        .small()
                                        .label("Import")
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
                            .label("Export")
                            .disabled(!self.editable())
                            .on_click(
                                cx.listener(|v, _, w, cx| v.files(FileAction::Export, w, cx)),
                            ),
                    )
                    .child(
                        Button::new("import")
                            .small()
                            .label("Import")
                            .disabled(!self.editable())
                            .on_click(
                                cx.listener(|v, _, w, cx| v.files(FileAction::Import, w, cx)),
                            ),
                    ),
            ),
        )
        .into_any_element()
    }
}
impl Render for StickView {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let content = self.device_panel(cx);
        let update = self.updates.state.clone();
        let banner = match &update {
            AppUpdate::UpToDate => Some("Stickwalker Desktop is up to date".into()),
            AppUpdate::Available(release) => Some(format!(
                "Stickwalker Desktop {} is available",
                release.version
            )),
            AppUpdate::ReadyToRestart { .. } => Some("App update ready".into()),
            AppUpdate::Downloading { percent, .. } => Some(
                percent
                    .map(|p| format!("Downloading app update · {p}%"))
                    .unwrap_or_else(|| "Downloading app update…".into()),
            ),
            AppUpdate::ReadyToInstall(_) => Some("App update downloaded".into()),
            AppUpdate::Installing => Some("Installing app update…".into()),
            AppUpdate::Failed(error) => Some(error.clone()),
            _ => None,
        };
        let update_error = matches!(update, AppUpdate::Failed(_));
        let device_error = self.state.error.is_some();
        let has_error = device_error || update_error;
        let message = if device_error {
            self.state.error.clone()
        } else if update_error {
            banner
        } else {
            self.state
                .status
                .clone()
                .or(banner)
                .or_else(|| self.state.firmware_feedback.clone())
        };
        let show_update = !has_error
            && self.state.status.is_none()
            && matches!(
                update,
                AppUpdate::Available(_) | AppUpdate::ReadyToRestart { .. }
            );
        let footer = feedback(message, has_error, cx)
            .when(has_error, |row| {
                row.child(
                    Button::new("dismiss-error")
                        .xsmall()
                        .ghost()
                        .label("Dismiss")
                        .on_click(cx.listener(move |v, _, _, cx| {
                            if device_error {
                                v.state.error = None;
                                v.send(Command::DismissError);
                            } else {
                                v.updates.state = AppUpdate::Idle;
                            }
                            v.refresh_shell(cx);
                        })),
                )
            })
            .when(show_update, |row| {
                row.child(
                    Button::new("app-update")
                        .xsmall()
                        .label(if matches!(update, AppUpdate::ReadyToRestart { .. }) {
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
                if v.prepare_close(cx) {
                    cx.quit();
                }
            }))
            .on_action(
                cx.listener(|v, action: &SelectStick, w, cx| v.select(action.0.clone(), w, cx)),
            )
            .on_action(cx.listener(|v, _: &ExportEeprom, w, cx| v.files(FileAction::Export, w, cx)))
            .on_action(cx.listener(|v, _: &ImportEeprom, w, cx| v.files(FileAction::Import, w, cx)))
            .on_action(cx.listener(|_, _: &AppearanceSystem, w, cx| {
                preferences::set_appearance(None, w, cx)
            }))
            .on_action(cx.listener(|_, _: &AppearanceLight, w, cx| {
                preferences::set_appearance(Some(false), w, cx)
            }))
            .on_action(cx.listener(|_, _: &AppearanceDark, w, cx| {
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
            .on_action(cx.listener(|v, _: &CheckFirmware, _, _| v.send(Command::CheckFirmware)))
            .on_action(cx.listener(|v, _: &Install, w, cx| v.install(w, cx)))
            .on_action(cx.listener(|v, _: &CheckAppUpdates, _, cx| v.app_update(cx)))
            .child(
                TitleBar::new()
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
        for (state, install, export, caption) in [
            (1, false, false, "No Device Connected"),
            (2, true, false, "Install Stickwalker "),
            (4, false, true, "Firmware Up to Date"),
            (5, true, true, "Update Firmware to "),
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
                assert_eq!(item("File", &crate::ExportEeprom).1, export);
                assert_eq!(item("Edit", &crate::EditPalette).1, export);
            })
            .unwrap();
        }
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
