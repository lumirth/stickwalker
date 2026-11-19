use super::*;
pub(crate) fn refresh_menus(cx: &mut App) {
    let appearance = preferences::current(cx).dark;
    let state = cx.global::<AppState>();
    let locked = state.menu.locked;
    let state = &state.menu;
    cx.set_menus(vec![
        Menu {
            disabled: false,
            name: "Stickwalker".into(),
            items: vec![
                MenuItem::action("About Stickwalker", About),
                MenuItem::submenu(Menu::new("Appearance").items([
                    MenuItem::action("System", AppearanceSystem).checked(appearance.is_none()),
                    MenuItem::action("Light", AppearanceLight).checked(appearance == Some(false)),
                    MenuItem::action("Dark", AppearanceDark).checked(appearance == Some(true)),
                ])),
                MenuItem::action(state.update_label.clone(), CheckAppUpdates)
                    .disabled(!state.updates_supported || locked),
                MenuItem::separator(),
                MenuItem::action("Close Window", CloseWindow).disabled(
                    locked && cx.active_window() == Some(cx.global::<AppState>().main_window),
                ),
                MenuItem::action("Quit Stickwalker", Quit).disabled(locked),
            ],
        },
        Menu {
            disabled: false,
            name: "File".into(),
            items: vec![
                MenuItem::action("Import EEPROM…", ImportEeprom)
                    .disabled(!state.access.can_write_save() || locked),
                MenuItem::action("Export EEPROM…", ExportEeprom)
                    .disabled(state.access != DeviceAccess::Ready || locked),
                MenuItem::separator(),
                MenuItem::action("Import Palettes…", ImportPalettes)
                    .disabled(state.access != DeviceAccess::Ready || locked),
                MenuItem::action("Export Palettes…", ExportPalettes)
                    .disabled(state.access != DeviceAccess::Ready || locked),
            ],
        },
        Menu::new("Edit").items([
            MenuItem::action("Copy Palette", CopyPalette)
                .disabled(state.access != DeviceAccess::Ready),
            MenuItem::action("Paste Palette", PastePalette)
                .disabled(state.access != DeviceAccess::Ready || locked),
            MenuItem::action("Edit Palettes", EditPalette)
                .disabled(state.access != DeviceAccess::Ready || locked),
        ]),
        Menu {
            disabled: false,
            name: "Device".into(),
            items: vec![
                MenuItem::action("Check for Firmware Updates", CheckFirmware)
                    .disabled(locked || state.checking_firmware),
                MenuItem::action("Open Firmware…", OpenPackage).disabled(locked),
                MenuItem::action(state.install_label.clone(), Install).disabled(!state.can_install),
                MenuItem::separator(),
                MenuItem::action("Reset Walker…", ResetWalker)
                    .disabled(!state.access.can_write_save() || locked),
                MenuItem::action("Erase and Install…", EraseInstall)
                    .disabled(!state.access.can_install() || locked),
            ],
        },
    ]);
    if let Some(menus) = cx.get_menus() {
        gpui_kit::base::GlobalState::global_mut(cx).set_app_menus(menus);
    }
    if let Some(bar) = cx
        .global::<AppState>()
        .editor_bar
        .as_ref()
        .and_then(WeakEntity::upgrade)
    {
        bar.update(cx, |bar, cx| bar.reload(cx));
    }
    if let Some(bar) = cx.global::<AppState>().menu_bar.clone() {
        bar.update(cx, |bar, cx| bar.reload(cx));
    }
}

fn forward(mut root: Div, owner: AnyWindowHandle, actions: Vec<Box<dyn Action>>) -> Div {
    for action in actions {
        root = root.on_boxed_action(action.as_ref(), move |action, _, cx| {
            let _ = owner.update(cx, |_, w, cx| {
                w.activate_window();
                w.dispatch_action(action.boxed_clone(), cx);
            });
        });
    }
    root
}
pub fn forward_device_actions(root: Div, owner: AnyWindowHandle) -> Div {
    forward(
        root,
        owner,
        vec![
            Box::new(EditPalette),
            Box::new(ExportEeprom),
            Box::new(ImportEeprom),
            Box::new(ResetWalker),
            Box::new(EraseInstall),
            Box::new(CheckFirmware),
            Box::new(Install),
            Box::new(OpenPackage),
            Box::new(CheckAppUpdates),
        ],
    )
}
pub fn open_about(cx: &mut App) {
    if let Some(handle) = cx.global::<AppState>().about
        && handle.update(cx, |_, w, _| w.activate_window()).is_ok()
    {
        return;
    }
    let options = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
            None,
            size(px(284.), px(222.)),
            cx,
        ))),
        app_id: Some(APP_ID.into()),
        is_resizable: false,
        titlebar: Some(TitlebarOptions {
            title: Some("About Stickwalker".into()),
            ..Default::default()
        }),
        ..Default::default()
    };
    if let Ok((handle, _)) = gpui_kit::open_window(options, cx, |w, cx| {
        cx.new(|cx| {
            preferences::apply(w, cx);
            let focus = cx.focus_handle();
            w.focus(&focus, cx);
            AboutWindow(focus)
        })
    }) {
        cx.global_mut::<AppState>().about = Some(handle);
    }
}
struct AboutWindow(FocusHandle);
impl Render for AboutWindow {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .v_flex()
            .items_center()
            .justify_center()
            .gap_2()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .track_focus(&self.0)
            .key_context("Stickwalker About")
            .map(|root| {
                let main = cx.global::<AppState>().main_window;
                forward(
                    forward_device_actions(root, main),
                    main,
                    vec![
                        Box::new(CopyPalette),
                        Box::new(PastePalette),
                        Box::new(ImportPalettes),
                        Box::new(ExportPalettes),
                    ],
                )
            })
            .on_action(|_: &CloseWindow, w, _| w.remove_window())
            .on_action(|_: &AppearanceDark, w, cx| preferences::set_appearance(Some(true), w, cx))
            .on_action(|_: &AppearanceLight, w, cx| preferences::set_appearance(Some(false), w, cx))
            .on_action(|_: &AppearanceSystem, w, cx| preferences::set_appearance(None, w, cx))
            .child(img("stickwalker.png").size_16())
            .child(div().font_weight(FontWeight::SEMIBOLD).child("Stickwalker"))
            .child(
                div()
                    .text_sm()
                    .child(format!("Version {}", env!("CARGO_PKG_VERSION"))),
            )
            .child(
                Button::new("source")
                    .small()
                    .ghost()
                    .label("GitHub · MIT License")
                    .on_click(|_, _, cx| cx.open_url("https://github.com/lumirth/stickwalker")),
            )
    }
}
