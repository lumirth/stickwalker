#![cfg_attr(
    all(target_os = "windows", not(debug_assertions)),
    windows_subsystem = "windows"
)]
extern crate gpui_kit as gpui;
mod device_window;
mod menus;
const APP_ID: &str = env!("STICKWALKER_APP_ID");
use menus::refresh_menus;
mod assets;

#[cfg(debug_assertions)]
mod debug_states;
mod palette_window;

mod preferences;
mod updater;

use gpui_kit::base::StyledExt;
use gpui_kit::component::{
    ActiveTheme, Disableable, Sizable, Theme, ThemeMode, TitleBar,
    button::{Button, ButtonVariants},
    menu::AppMenuBar,
};
use gpui_kit::prelude::FluentBuilder;
use gpui_kit::*;
use host::worker::DeviceAccess;

actions!(
    stickwalker,
    [
        Quit,
        CloseWindow,
        About,
        AppearanceSystem,
        AppearanceLight,
        AppearanceDark,
        EditPalette,
        CopyAllPalettes,
        PastePaletteSet,
        ImportPalettes,
        ExportPalettes,
        ResetAllPalettes,
        CopyPalette,
        PastePalette,
        ResetWalker,
        EraseInstall,
        CheckAppUpdates,
        CheckFirmware,
        Install,
        ExportEeprom,
        ImportEeprom,
        OpenPackage
    ]
);
#[derive(Default, PartialEq, Eq)]
struct MenuState {
    locked: bool,
    update_label: String,
    updates_supported: bool,
    access: DeviceAccess,
    checking_firmware: bool,
    can_install: bool,
    install_label: String,
}
struct AppState {
    menu: MenuState,
    about: Option<AnyWindowHandle>,
    main_window: AnyWindowHandle,
    main: WeakEntity<device_window::StickView>,
    menu_bar: Option<Entity<AppMenuBar>>,
    editor_bar: Option<WeakEntity<AppMenuBar>>,
}
impl Global for AppState {}
fn shortcut(key: &str, action: impl Action, context: Option<&str>, cx: &App) -> KeyBinding {
    KeyBinding::load(
        key,
        Box::new(action),
        context.map(|s| KeyBindingContextPredicate::parse(s).unwrap().into()),
        false,
        None,
        cx,
    )
    .expect("valid application shortcut")
}
fn main() {
    Application::with_platform(gpui_kit::platform::current_platform(false))
        .with_assets(assets::Assets)
        .run(|cx| {
            gpui_kit::init(cx);
            preferences::start(cx);
            let keys = [
                shortcut("secondary-w", CloseWindow, None, cx),
                shortcut("escape", CloseWindow, Some("About"), cx),
                shortcut("secondary-e", EditPalette, Some("Stickwalker"), cx),
                shortcut("secondary-q", Quit, None, cx),
                shortcut("secondary-o", ImportEeprom, Some("Stickwalker"), cx),
                shortcut("secondary-s", ExportEeprom, Some("Stickwalker"), cx),
                shortcut("secondary-shift-c", CopyPalette, Some("Stickwalker"), cx),
                shortcut("secondary-shift-v", PastePalette, Some("Stickwalker"), cx),
            ];
            cx.bind_keys(keys);
            #[cfg(debug_assertions)]
            debug_states::bind(cx);
            cx.on_action(|_: &Quit, cx| {
                let main = cx.global::<AppState>().main.clone();
                cx.defer(move |cx| {
                    let _ = main.update(cx, |v, cx| {
                        v.request_exit(device_window::PendingExit::Quit, cx)
                    });
                });
            });
            cx.on_action(|_: &About, cx| menus::open_about(cx));
            let options = WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(480.), px(310.)),
                    cx,
                ))),
                titlebar: Some(TitlebarOptions {
                    title: Some("Stickwalker".into()),
                    ..TitleBar::title_bar_options()
                }),
                window_min_size: Some(size(px(460.), px(310.))),
                app_id: Some(APP_ID.into()),
                ..TitleBar::window_options()
            };
            if let Err(error) = gpui_kit::open_window(options, cx, |w, cx| {
                cx.new(|cx| device_window::StickView::new(w, cx))
            }) {
                eprintln!("Could not open Stickwalker: {error}");
                cx.quit();
            }
            cx.activate(true);
        });
}
