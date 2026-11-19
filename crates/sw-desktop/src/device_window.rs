pub(crate) mod render;
use crate::palette_window::selector::{PaletteSelected, PaletteSelector};
use crate::palette_window::{PaletteAction, PaletteChanged, PaletteEditor};
use crate::updater::{AppUpdate, Updates};
use crate::*;

use host::worker::{Command, ConnectionState, Presentation, Worker};
use std::{sync::atomic::Ordering, time::Duration};

#[derive(Clone, Copy)]
pub(super) enum FileAction {
    Export,
    Import,
    Package,
}
#[derive(Clone, Copy)]
pub(crate) enum PendingExit {
    Quit,
    Restart,
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
    pending_exit: Option<PendingExit>,
    device_mac: Option<String>,
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
                        if preferences::current(cx).automatic_updates {
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
            let _ = owner.update(cx, |view, cx| view.request_exit(PendingExit::Quit, cx));
            false
        });
        let focus = cx.focus_handle();
        window.focus(&focus, cx);
        subscriptions.push(cx.observe_window_appearance(window, |_, w, cx| {
            if preferences::current(cx).dark.is_none() {
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
            pending_exit: None,
            device_mac: None,
            local_revision: 0,
            updates,
            _subscriptions: subscriptions,
        }
    }
    pub(crate) fn request_exit(&mut self, exit: PendingExit, cx: &mut Context<Self>) {
        if self.locked() {
            return;
        }
        if let Some((handle, editor)) = self.editor.clone()
            && let Ok(settings) = handle.update(cx, |_, w, cx| {
                editor.update(cx, |editor, cx| editor.take_committed_settings(w, cx))
            })
        {
            self.change_palette(settings, cx);
        }
        self.pending_exit = Some(exit);
        self.finish_exit(cx);
    }
    fn finish_exit(&mut self, cx: &mut Context<Self>) {
        if self.locked() || (self.editable() && self.state.acked_revision < self.local_revision) {
            return;
        }
        match self.pending_exit.take() {
            Some(PendingExit::Quit) => cx.defer(|cx| cx.quit()),
            Some(PendingExit::Restart) => self.updates.restart(cx),
            None => {}
        }
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
        let incoming = state.connection.info().map(|i| i.mac.clone());
        let device_changed = incoming.is_some() && self.device_mac != incoming;
        if device_changed {
            self.device_mac = incoming;
            self.local_revision = state.acked_revision;
            if let Some((handle, _)) = self.editor.take() {
                self._editor_subscription = None;
                let _ = handle.update(cx, |_, w, _| w.remove_window());
            }
        }
        if state.failed_revision == Some(self.local_revision) || state.connection.info().is_none() {
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
        self.finish_exit(cx);
    }
    pub(super) fn refresh_shell(&mut self, cx: &mut Context<Self>) {
        if !self.worker.hardware_busy.load(Ordering::SeqCst) {
            self.updates.install_downloaded();
        }
        let access = self.state.connection.access();
        let locked = self.locked() || self.state.hardware_busy;
        let can_install = access.can_install()
            && !locked
            && (access == DeviceAccess::InstallOnly || self.update_available());
        let install_label = self.install_label();
        let app = cx.global_mut::<AppState>();
        let next = MenuState {
            locked,
            update_label: self.updates.action_label(),
            updates_supported: self.updates.can_act(),
            checking_firmware: self.state.checking_firmware,
            access,
            can_install,
            install_label,
        };
        if app.menu.locked != next.locked
            && let Some((handle, editor)) = &self.editor
        {
            let enabled = next.access == DeviceAccess::Ready && !next.locked;
            let (handle, editor) = (*handle, editor.clone());
            cx.defer(move |cx| {
                let _ = handle.update(cx, |_, w, cx| {
                    editor.update(cx, |e, cx| e.receive(None, enabled, w, cx))
                });
            });
        }
        let app = cx.global_mut::<AppState>();
        if app.menu != next {
            app.menu = next;
            refresh_menus(cx);
        }
        cx.notify();
    }
    pub(super) fn locked(&self) -> bool {
        self.worker.hardware_busy.load(Ordering::SeqCst) || self.updates.blocks_hardware()
    }
    pub(super) fn editable(&self) -> bool {
        !self.locked() && self.state.connection.access() == DeviceAccess::Ready
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
        if !self.locked() && command.allowed(&self.state.connection) {
            let _ = self
                .worker
                .commands
                .send(self.state.connection.target(), command);
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
            AppUpdate::ReadyToRestart { .. } => self.request_exit(PendingExit::Restart, cx),
            AppUpdate::Available(_) => self.updates.install(),
            _ => self.updates.check(false),
        }
        self.refresh_shell(cx);
    }
    pub(super) fn files(&self, action: FileAction, window: &mut Window, cx: &mut Context<Self>) {
        let access = self.state.connection.access();
        let available = match action {
            FileAction::Package => access.can_install(),
            FileAction::Export => access == DeviceAccess::Ready,
            FileAction::Import => access.can_write_save(),
        };
        if self.locked() || !available {
            return;
        }
        let directory = directories::UserDirs::new()
            .map(|dirs| dirs.document_dir().unwrap_or(dirs.home_dir()).to_owned())
            .unwrap_or_default();
        let target = self.state.connection.target();
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
                let image = if matches!(action, FileAction::Import) {
                    let source = path.clone();
                    Some(
                        cx.background_executor()
                            .spawn(async move { host::save::EepromImage::load(&source) })
                            .await,
                    )
                } else {
                    None
                };
                let _ = view.update_in(cx, |view, window, cx| {
                    if target != view.state.connection.target() || view.locked() {
                        return;
                    }
                    match image {
                        Some(Ok(image)) => {
                            let command = Command::Import(image);
                            if command.allowed(&view.state.connection) {
                                let name = path.file_name().unwrap_or_default().to_string_lossy();
                                view.confirm(
                                    "Replace current save?",
                                    &format!("Import “{name}” onto {}.", view.device_label()),
                                    "Import",
                                    command,
                                    window,
                                    cx,
                                );
                            }
                        }
                        Some(Err(error)) => {
                            view.state.error = Some(error.to_string());
                            cx.notify();
                        }
                        None if matches!(action, FileAction::Package) => {
                            view.send(Command::OpenPackage(path))
                        }
                        None => view.send(Command::Export(path)),
                    }
                });
            } else if let Ok(Err(error)) = result {
                let _ = view.update(cx, |view, cx| {
                    view.state.error = Some(error.to_string());
                    cx.notify();
                });
            }
        })
        .detach();
    }

    pub(super) fn reset(&self, erase: bool, w: &mut Window, cx: &mut Context<Self>) {
        let command = if erase {
            Command::CleanInstall
        } else {
            Command::Reset
        };
        if self.locked() || !command.allowed(&self.state.connection) {
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
            command,
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
        let target = self.state.connection.target();
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
                    if v.state.connection.target() == target && !v.locked() {
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
        let target = self.state.connection.target();
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
                if !v.editable() || v.state.connection.target() != target {
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
                    let slot = &mut settings.slots[settings.active_palette.index()];
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
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                None,
                size(px(716.), px(464.)),
                cx,
            ))),
            app_id: Some(APP_ID.into()),
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
                &info.settings.slots[info.settings.active_palette.index()],
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
        self.send(Command::Update);
    }
}
