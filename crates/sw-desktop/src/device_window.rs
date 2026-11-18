pub(crate) mod render;
use crate::palette_window::selector::{PaletteSelected, PaletteSelector};
use crate::palette_window::{PaletteAction, PaletteChanged, PaletteEditor};
use crate::updater::{AppUpdate, Updates};
use crate::*;

use host::worker::{Command, ConnectionState, Presentation, Worker};
use std::{path::PathBuf, time::Duration};

#[derive(Clone, Copy)]
pub(super) enum FileAction {
    Export,
    Import,
    Package,
}
pub struct StickView {
    worker: Worker,
    window: AnyWindowHandle,
    pub(super) state: Presentation,
    pub(super) focus: FocusHandle,
    pub(super) palette: Entity<PaletteSelector>,
    editor: Option<(AnyWindowHandle, Entity<PaletteEditor>)>,
    _editor_subscription: Option<Subscription>,
    _tasks: Vec<Task<()>>,
    #[cfg(debug_assertions)]
    pub(super) preview: Option<u8>,
    #[cfg(debug_assertions)]
    pub(super) live_state: Presentation,
    #[cfg(debug_assertions)]
    pub(super) live_update: Option<AppUpdate>,
    close_requested: bool,
    local_revision: u64,
    pub(super) updates: Updates,
    _subscriptions: Vec<Subscription>,
}
impl StickView {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        #[cfg(debug_assertions)]
        let worker = if cfg!(test) || std::env::args().any(|a| a == "--preview") {
            Worker::preview()
        } else {
            Worker::start()
        };
        #[cfg(not(debug_assertions))]
        let worker = Worker::start();
        let updates = Updates::new();
        cx.set_global(AppState {
            hardware_busy: worker.hardware_busy.clone(),
            menu: MenuState {
                install_label: "No Device Connected".into(),
                update_label: updates.action_label(),
                updates_supported: updates.can_act(),
                ..Default::default()
            },
            menu_bar: None,
            editor_bar: None,
            main: cx.weak_entity(),
            about: None,
            main_window: window.window_handle(),
        });
        refresh_menus(cx);
        let menu_bar = gpui_kit::component::menu::AppMenuBar::new(cx);
        cx.global_mut::<AppState>().menu_bar = Some(menu_bar);
        let palette = cx.new(|cx| PaletteSelector::new(window, cx));
        let mut subscriptions =
            vec![
                cx.subscribe(&palette, |view, _, event: &PaletteSelected, cx| {
                    if let Some(info) = view.state.connection.info() {
                        let mut settings = info.settings;
                        settings.active_palette = event.0;
                        view.change_palette(settings, cx);
                    }
                }),
            ];
        let mut tasks = Vec::new();
        let events = worker.events.clone();
        tasks.push(cx.spawn_in(window, async move |view, cx| {
            while let Ok(state) = events.recv().await {
                if view
                    .update_in(cx, |view, w, cx| view.receive(state, w, cx))
                    .is_err()
                {
                    break;
                }
            }
        }));
        let events = updates.events.clone();
        tasks.push(cx.spawn_in(window, async move |view, cx| {
            while let Ok(event) = events.recv().await {
                if view
                    .update(cx, |view, cx| {
                        #[cfg(debug_assertions)]
                        if let Some(live) = &mut view.live_update {
                            std::mem::swap(live, &mut view.updates.state);
                            view.updates.accept(event);
                            std::mem::swap(live, &mut view.updates.state);
                            return;
                        }
                        view.updates.accept(event);
                        view.refresh_shell(cx);
                    })
                    .is_err()
                {
                    break;
                }
            }
        }));
        tasks.push(cx.spawn(async move |view, cx| {
            cx.background_executor()
                .timer(Duration::from_secs(15))
                .await;
            loop {
                if view
                    .update(cx, |view, cx| {
                        if preferences::Preferences::load().automatic_updates {
                            view.updates.check(true);
                            view.refresh_shell(cx);
                        }
                    })
                    .is_err()
                {
                    break;
                }
                cx.background_executor()
                    .timer(Duration::from_secs(24 * 60 * 60))
                    .await;
            }
        }));
        let owner = cx.weak_entity();
        window.on_window_should_close(cx, move |_, cx| {
            if cx.global::<AppState>().locked() {
                return false;
            }
            let close = owner
                .update(cx, |view, cx| view.prepare_close(cx))
                .unwrap_or(true);
            if close {
                cx.defer(|cx| cx.quit());
            }
            close
        });
        let focus = cx.focus_handle();
        window.focus(&focus, cx);
        subscriptions.push(cx.observe_window_appearance(window, |_, w, cx| {
            if preferences::Preferences::load().dark.is_none() {
                Theme::change(ThemeMode::from(w.appearance()), Some(w), cx);
            }
        }));
        subscriptions
            .push(cx.observe_window_activation(window, |_, _, cx| cx.defer(refresh_menus)));
        preferences::apply(window, cx);
        Self {
            worker,
            window: window.window_handle(),
            state: Presentation::default(),
            focus,
            palette,
            editor: None,
            _editor_subscription: None,
            _tasks: tasks,
            #[cfg(debug_assertions)]
            preview: None,
            #[cfg(debug_assertions)]
            live_state: Presentation::default(),
            #[cfg(debug_assertions)]
            live_update: None,
            close_requested: false,
            local_revision: 0,
            updates,
            _subscriptions: subscriptions,
        }
    }
    pub(crate) fn prepare_close(&mut self, cx: &mut Context<Self>) -> bool {
        if self.locked() {
            return false;
        }
        if let Some((handle, editor)) = self.editor.clone()
            && let Ok(settings) = handle.update(cx, |_, w, cx| {
                editor.update(cx, |e, cx| {
                    e.commit_inputs(w, cx);
                    e.settings()
                })
            })
            && self
                .state
                .connection
                .info()
                .is_some_and(|i| i.settings != settings)
        {
            self.change_palette(settings, cx);
        }
        self.close_requested = self.state.acked_revision < self.local_revision && self.editable();
        !self.close_requested
    }
    pub(super) fn receive(&mut self, state: Presentation, w: &mut Window, cx: &mut Context<Self>) {
        #[cfg(debug_assertions)]
        {
            self.live_state = state.clone();
            if self.preview.is_some() {
                return;
            }
        }
        self.present(state, w, cx);
    }
    pub(super) fn present(
        &mut self,
        mut state: Presentation,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let previous = self.state.connection.info().map(|i| &i.mac);
        let incoming = state.connection.info().map(|i| &i.mac);
        let device_changed = previous != incoming;
        if device_changed {
            self.local_revision = 0;
            if let Some((handle, _)) = self.editor.take() {
                self._editor_subscription = None;
                let _ = handle.update(cx, |_, w, _| w.remove_window());
            }
        }
        if state.error.is_some() {
            self.local_revision = state.acked_revision;
        }
        if !device_changed
            && state.acked_revision < self.local_revision
            && let Some(previous) = self.state.connection.info()
            && let Some(next) = state.connection.info_mut()
        {
            next.settings.active_palette = previous.settings.active_palette;
            next.settings.slots = previous.settings.slots;
        }
        let settings = if device_changed || state.acked_revision >= self.local_revision {
            state.connection.info().map(|i| i.settings)
        } else {
            None
        };
        if self.close_requested {
            if device_changed || state.error.is_some() {
                self.close_requested = false;
            } else if state.acked_revision >= self.local_revision {
                cx.quit();
            }
        }
        self.state = state;
        let enabled = self.editable();
        self.palette
            .update(cx, |editor, cx| editor.receive(settings, enabled, w, cx));
        if let Some((handle, editor)) = self.editor.clone()
            && handle
                .update(cx, |_, w, cx| {
                    editor.update(cx, |e, cx| e.receive(settings, enabled, w, cx))
                })
                .is_err()
        {
            self.editor = None;
            self._editor_subscription = None;
        }
        self.refresh_shell(cx);
    }
    pub(super) fn refresh_shell(&mut self, cx: &mut Context<Self>) {
        if !self.worker.hardware_busy.load(Ordering::SeqCst) {
            self.updates.install_downloaded();
        }
        let can_install = self.state.connection.port().is_some()
            && (self.state.connection.info().is_none() || self.update_available())
            && !matches!(
                self.state.connection,
                ConnectionState::LinkingInGame { .. }
                    | ConnectionState::Reconnecting { .. }
                    | ConnectionState::Unavailable { .. }
                    | ConnectionState::Flashing { .. }
            )
            && !self.locked();
        let install_label = self.install_label();
        let app = cx.global_mut::<AppState>();
        let next = MenuState {
            app_installing: self.updates.is_installing(),
            update_label: self.updates.action_label(),
            updates_supported: self.updates.can_act(),
            checking_firmware: self.state.checking_firmware,
            managed: self.state.connection.info().is_some(),
            ready: matches!(self.state.connection, ConnectionState::Ready { .. }),
            selected: matches!(
                self.state.connection,
                ConnectionState::Ready { .. }
                    | ConnectionState::Unmanaged { .. }
                    | ConnectionState::SaveFault { .. }
            ),
            linking: matches!(self.state.connection, ConnectionState::LinkingInGame { .. }),
            hardware_busy: self.state.hardware_busy,
            can_install,
            install_label,
        };
        if app.menu != next {
            app.menu = next;
            refresh_menus(cx);
        }
        cx.notify();
    }
    pub(super) fn locked(&self) -> bool {
        self.worker.hardware_busy.load(Ordering::SeqCst) || self.updates.is_installing()
    }
    pub(super) fn editable(&self) -> bool {
        !self.locked() && matches!(self.state.connection, ConnectionState::Ready { .. })
    }
    pub(super) fn select(
        &mut self,
        port: host::usb::Port,
        _w: &mut Window,
        _cx: &mut Context<Self>,
    ) {
        if self.state.connection.port() == Some(&port) || self.locked() {
            return;
        }
        #[cfg(debug_assertions)]
        if self.preview == Some(8) {
            let mut next = self.state.clone();
            let Some(info) = next.connection.info().cloned() else {
                return;
            };
            next.connection = ConnectionState::Ready {
                info: host::usb::Info {
                    mac: port.serial.clone(),
                    ..info
                },
                port,
            };
            self.present(next, _w, _cx);
            return;
        }
        self.send(Command::Select(port));
    }
    pub(super) fn send(&self, command: Command) {
        #[cfg(debug_assertions)]
        if self.preview.is_some() {
            return;
        }
        if !self.locked() {
            let _ = self
                .worker
                .commands
                .send(self.state.connection.port().cloned(), command);
        }
    }
    pub(super) fn app_update(&mut self, cx: &mut Context<Self>) {
        if self.locked() {
            return;
        }
        #[cfg(debug_assertions)]
        if self.preview.is_some() {
            return;
        }
        match self.updates.state {
            AppUpdate::ReadyToRestart { .. } => self.updates.restart(cx),
            AppUpdate::Available(_) => self.updates.install(),
            _ => self.updates.check(false),
        }
        self.refresh_shell(cx);
    }
    pub(super) fn files(&self, action: FileAction, window: &mut Window, cx: &mut Context<Self>) {
        let available = match action {
            FileAction::Package => cx.global::<AppState>().menu.selected,
            FileAction::Export => self.editable(),
            FileAction::Import => self
                .state
                .connection
                .info()
                .is_some_and(|info| info.mode != host::usb::DeviceMode::Linking),
        };
        if self.locked() || !available {
            return;
        }
        let directory = directories::UserDirs::new()
            .map(|dirs| dirs.document_dir().unwrap_or(dirs.home_dir()).to_owned())
            .unwrap_or_default();
        let target = self.state.connection.port().cloned();
        cx.spawn_in(window, async move |view, cx| {
            let result = match action {
                FileAction::Export => {
                    let Ok(prompt) = cx
                        .update(|_, cx| cx.prompt_for_new_path(&directory, Some("walker.eeprom")))
                    else {
                        return;
                    };
                    prompt.await
                }
                _ => {
                    let Ok(prompt) = cx.update(|_, cx| {
                        cx.prompt_for_paths(PathPromptOptions {
                            files: true,
                            directories: false,
                            multiple: false,
                            prompt: Some(
                                match action {
                                    FileAction::Import => "Import EEPROM",
                                    _ => "Open firmware.json",
                                }
                                .into(),
                            ),
                        })
                    }) else {
                        return;
                    };
                    prompt
                        .await
                        .map(|r| r.map(|p| p.and_then(|p| p.into_iter().next())))
                }
            };
            if let Ok(Ok(Some(path))) = result {
                let _ = view.update_in(cx, |view, window, cx| {
                    if matches!(action, FileAction::Package)
                        || target.as_ref() == view.state.connection.port()
                    {
                        view.selected_file(action, path, window, cx)
                    }
                });
            }
        })
        .detach();
    }

    fn selected_file(
        &mut self,
        action: FileAction,
        path: PathBuf,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match action {
            FileAction::Export => self.send(Command::Export(path)),
            FileAction::Package => self.send(Command::OpenPackage(path)),
            FileAction::Import => {
                let name = path
                    .file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned();
                let stick = self.device_label();
                self.confirm(
                    "Replace current save?",
                    &format!("Import “{name}” onto {stick}."),
                    "Import",
                    Command::Import(path),
                    w,
                    cx,
                );
            }
        }
    }
    pub(super) fn reset(&self, erase: bool, w: &mut Window, cx: &mut Context<Self>) {
        if self.locked()
            || self.state.connection.port().is_none()
            || matches!(self.state.connection, ConnectionState::LinkingInGame { .. })
        {
            return;
        }
        if !cx.global::<AppState>().menu.selected
            || (!erase && self.state.connection.info().is_none())
        {
            return;
        }
        self.confirm(
            if erase {
                "Erase and install Stickwalker?"
            } else {
                "Reset walker?"
            },
            if erase {
                "Clears the save and device settings on this StickS3."
            } else {
                "Clears this save and its DS pairing."
            },
            if erase { "Erase and install" } else { "Reset" },
            if erase {
                Command::CleanInstall
            } else {
                Command::Reset
            },
            w,
            cx,
        );
    }
    fn confirm(
        &self,
        title: &str,
        detail: &str,
        action: &str,
        command: Command,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let port = self.state.connection.port().cloned();
        let answer = w.prompt(
            PromptLevel::Warning,
            title,
            Some(detail),
            &["Cancel", action],
            cx,
        );
        cx.spawn_in(w, async move |view, cx| {
            if answer.await == Ok(1) {
                let _ = view.update(cx, |v, _| {
                    if v.state.connection.port() == port.as_ref() && !v.locked() {
                        v.send(command);
                    }
                });
            }
        })
        .detach();
    }
    fn change_palette(&mut self, settings: host::DeviceSettings, cx: &mut Context<Self>) {
        if self.editable()
            && self
                .state
                .connection
                .info()
                .is_some_and(|i| i.settings != settings)
        {
            self.local_revision += 1;
            if let Some(info) = self.state.connection.info_mut() {
                info.settings = settings;
            }
            let picker = self.palette.clone();
            let handle = self.window;
            let owner = cx.weak_entity();
            cx.defer(move |cx| {
                let Some(entity) = owner.upgrade() else {
                    return;
                };
                let view = entity.read(cx);
                if let Some(info) = view.state.connection.info() {
                    let latest = info.settings;
                    let enabled = view.editable();
                    let _ = handle.update(cx, |_, w, cx| {
                        picker.update(cx, |p, cx| p.receive(Some(latest), enabled, w, cx))
                    });
                }
            });
            #[cfg(debug_assertions)]
            if self.preview.is_some() {
                self.state.acked_revision = self.local_revision;
                cx.notify();
                return;
            }
            self.send(Command::SetSettings {
                revision: self.local_revision,
                settings,
            });
        }
    }
    pub(super) fn palette_file(
        &mut self,
        action: PaletteAction,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.editable() {
            return;
        }
        let target = self.state.connection.port().cloned();
        let settings = self.state.connection.info().unwrap().settings;
        let contents = settings
            .slots
            .iter()
            .map(host::palette::format)
            .collect::<Vec<_>>()
            .join("\n");
        cx.spawn_in(w, async move |view, cx| {
            let result = crate::palette_window::palette_file(action, contents, cx).await;
            let _ = view.update(cx, |v, cx| {
                if !v.editable() || v.state.connection.port() != target.as_ref() {
                    return;
                }
                match result.and_then(|s| s.map(|s| host::palette::parse_many(&s)).transpose()) {
                    Ok(Some(palettes)) => {
                        let mut current = v.state.connection.info().unwrap().settings;
                        for (slot, p) in current.slots.iter_mut().zip(palettes) {
                            if let Some(name) = p.name {
                                slot.name = name;
                            }
                            slot.colors = p.colors;
                        }
                        v.change_palette(current, cx);
                    }
                    Err(e) => {
                        v.state.error = Some(e.to_string());
                        cx.notify();
                    }
                    Ok(None) => {}
                }
            });
        })
        .detach();
    }
    pub(super) fn open_palette(&mut self, paste: bool, cx: &mut Context<Self>) {
        if !self.editable() {
            return;
        }
        if let Some((handle, editor)) = self.editor.clone()
            && handle
                .update(cx, |_, w, cx| {
                    w.activate_window();
                    if paste {
                        editor.update(cx, |e, cx| e.paste(w, cx));
                    }
                })
                .is_ok()
        {
            return;
        }
        let mut settings = self.state.connection.info().unwrap().settings;
        if paste {
            let input = cx
                .read_from_clipboard()
                .and_then(|item| item.text())
                .unwrap_or_default();
            match host::palette::parse(&input) {
                Ok(p) => {
                    let slot = &mut settings.slots[settings.active_palette as usize];
                    if let Some(name) = p.name {
                        slot.name = name;
                    }
                    slot.colors = p.colors;
                    self.change_palette(settings, cx);
                }
                Err(e) => {
                    self.state.error = Some(e.to_string());
                    cx.notify();
                }
            }
            return;
        }
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                point(px(230.), px(190.)),
                size(px(716.), px(464.)),
            ))),
            window_min_size: Some(size(px(680.), px(430.))),
            titlebar: Some(TitlebarOptions {
                title: Some("Palettes".into()),
                ..TitleBar::title_bar_options()
            }),
            ..TitleBar::window_options()
        };
        match gpui_kit::open_window(options, cx, |w, cx| {
            cx.new(|cx| PaletteEditor::new(settings, self.window, w, cx))
        }) {
            Ok((handle, editor)) => {
                self._editor_subscription =
                    Some(cx.subscribe(&editor, |v, _, event: &PaletteChanged, cx| {
                        v.change_palette(event.0, cx)
                    }));
                let _ = handle.update(cx, |_, w, cx| {
                    if paste {
                        editor.update(cx, |e, cx| e.paste(w, cx));
                    }
                });
                self.editor = Some((handle, editor));
            }
            Err(error) => {
                self.state.error = Some(error.to_string());
                cx.notify();
            }
        }
    }
    pub(super) fn copy_palette(&self, cx: &mut App) {
        if let Some(info) = self.state.connection.info() {
            cx.write_to_clipboard(ClipboardItem::new_string(host::palette::format(
                &info.settings.slots[info.settings.active_palette as usize],
            )));
        }
    }
    pub(super) fn update_available(&self) -> bool {
        self.state.firmware.as_ref().is_some_and(|f| f.local())
            || self
                .state
                .connection
                .info()
                .zip(self.state.firmware.as_ref())
                .is_some_and(|(i, v)| {
                    match (
                        v.version().parse::<host::release::Version>(),
                        i.version.parse::<host::release::Version>(),
                    ) {
                        (Ok(new), Ok(old)) => new > old,
                        _ => false,
                    }
                })
    }
    pub(super) fn device_label(&self) -> String {
        self.state
            .connection
            .port()
            .map(|p| self.state.port_label(p))
            .unwrap_or_default()
    }
    pub(super) fn install_label(&self) -> String {
        let version = self
            .state
            .firmware
            .as_ref()
            .map(|f| f.version())
            .unwrap_or("");
        if self.state.connection.port().is_none() {
            return "No Device Connected".into();
        }
        let Some(info) = self.state.connection.info() else {
            return format!("Install Stickwalker {version}").trim_end().into();
        };
        if self.state.firmware.as_ref().is_some_and(|f| f.local()) {
            if info.version == version {
                format!("Reinstall {version}")
            } else {
                format!("Install {version}")
            }
        } else if self.update_available() {
            format!("Update Firmware to {version}")
        } else {
            "Firmware Up to Date".into()
        }
    }
    pub(super) fn install(&self, w: &mut Window, cx: &mut Context<Self>) {
        if !cx.global::<AppState>().menu.can_install {
            return;
        }
        if matches!(self.state.connection, ConnectionState::Unmanaged { .. }) {
            self.confirm(&format!("Install Stickwalker on {}?",self.device_label()), "This replaces the device’s current software. Existing Stickwalker saves and settings are kept.", "Install", Command::Update, w, cx);
            return;
        }
        if matches!(
            self.state.connection,
            ConnectionState::Reconnecting { .. } | ConnectionState::LinkingInGame { .. }
        ) {
            return;
        }
        if self.state.connection.port().is_some()
            && (self.state.connection.info().is_none() || self.update_available())
        {
            self.send(Command::Update);
        }
    }
}
