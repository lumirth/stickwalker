mod install;
pub(crate) mod render;
#[cfg(all(test, debug_assertions))]
mod tests;
use crate::palette_window::selector::{PaletteSelected, PaletteSelector};
use crate::palette_window::{PaletteAction, PaletteChanged, PaletteEditor};
use crate::updater::{AppUpdate, Updates};
use crate::*;

use host::worker::{Command, ConnectionState, Presentation, Worker};
use install::PendingInstall;
use std::{sync::atomic::Ordering, time::Duration};

#[derive(Clone, Copy)]
pub(super) enum FileAction {
    Export,
    Import,
    Package,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PendingExit {
    Quit,
    Restart,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ExitState {
    Running,
    Draining(PendingExit),
    Exiting(PendingExit),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum CheckFeedback {
    App,
    Firmware,
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
    exit: ExitState,
    local_error: Option<String>,
    device_mac: Option<String>,
    connection_epoch: u64,
    next_revision: u64,
    pending_revision: Option<u64>,
    pending_install: Option<PendingInstall>,
    latest_check: CheckFeedback,
    pub(super) updates: Updates,
    _subscriptions: Vec<Subscription>,
}
pub(super) const REPLACES_SOFTWARE: &str = "This replaces the software on the Stick.\n\n(If it runs an earlier Stickwalker, its save and settings are kept.)";

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
            owner
                .update(cx, |view, cx| {
                    if matches!(view.exit, ExitState::Exiting(_)) {
                        return true;
                    }
                    view.request_exit(PendingExit::Quit, cx);
                    false
                })
                .unwrap_or(true)
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
            exit: ExitState::Running,
            local_error: None,
            device_mac: None,
            connection_epoch: 0,
            next_revision: 0,
            pending_revision: None,
            pending_install: None,
            latest_check: CheckFeedback::Firmware,
            updates,
            _subscriptions: subscriptions,
        }
    }
    pub(crate) fn request_exit(&mut self, exit: PendingExit, cx: &mut Context<Self>) {
        if self.exit != ExitState::Running || self.locked() {
            return;
        }
        self.pending_install = None;
        self.exit = ExitState::Draining(exit);
        if let Some((handle, editor)) = self.editor.clone()
            && let Ok(settings) = handle.update(cx, |_, w, cx| {
                editor.update(cx, |editor, cx| editor.take_committed_settings(w, cx))
            })
        {
            self.submit_palette(settings, cx);
        }
        self.finish_exit(cx);
    }
    fn finish_exit(&mut self, cx: &mut Context<Self>) {
        let ExitState::Draining(exit) = self.exit else {
            return;
        };
        if self.locked() || self.pending_revision.is_some() {
            return;
        }
        self.exit = ExitState::Exiting(exit);
        match exit {
            PendingExit::Quit => cx.defer(|cx| cx.quit()),
            PendingExit::Restart => {
                if !self.updates.defer_restart(cx) {
                    self.exit = ExitState::Running;
                }
            }
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
        let previous = &self.state.connection;
        let attached = |c: &ConnectionState| {
            c.info().is_some() || matches!(c, ConnectionState::Unmanaged { .. })
        };
        // A replacement Stick can reuse the same USB port without reporting a serial number.
        if previous.port() != state.connection.port()
            || (device_changed && previous.info().is_some())
            || (attached(previous) && !attached(&state.connection))
        {
            self.connection_epoch += 1;
        }
        if device_changed {
            self.device_mac = incoming;
            if let Some((handle, _)) = self.editor.take() {
                self._editor_subscription = None;
                let _ = handle.update(cx, |_, w, _| w.remove_window());
            }
        }
        if device_changed
            || state.connection.info().is_none()
            || self
                .pending_revision
                .is_some_and(|r| state.acked_revision >= r || state.failed_revision == Some(r))
        {
            self.pending_revision = None;
        }
        if self.pending_revision.is_some()
            && let Some(previous) = self.state.connection.info()
            && let Some(next) = state.connection.info_mut()
        {
            next.settings.active_palette = previous.settings.active_palette;
            next.settings.slots = previous.settings.slots;
        }
        let settings = if self.pending_revision.is_none() {
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
    }
    pub(super) fn refresh_shell(&mut self, cx: &mut Context<Self>) {
        #[cfg(not(target_os = "macos"))]
        if !self.worker.hardware_busy.load(Ordering::SeqCst) {
            self.updates.install_downloaded();
        }
        if self.exit == ExitState::Exiting(PendingExit::Restart)
            && !matches!(self.updates.state, AppUpdate::Restarting)
        {
            self.exit = ExitState::Running;
        }
        self.resume_install();
        let access = self.state.connection.access();
        let locked = self.locked();
        let can_install = access.can_install()
            && self.pending_install.is_none()
            && !locked
            && (access == DeviceAccess::InstallOnly || self.offer_update());
        let install_label = self.install_label();
        let next = MenuState {
            locked,
            update_label: self.updates.action_label(),
            updates_supported: self.updates.can_act(),
            checking_firmware: self.state.checking_firmware,
            preparing_install: self.pending_install.is_some(),
            access,
            can_install,
            install_label,
        };
        if cx.global::<AppState>().menu.locked != next.locked {
            let enabled = next.access == DeviceAccess::Ready && !next.locked;
            self.palette.update(cx, |p, cx| p.set_enabled(enabled, cx));
            if let Some((handle, editor)) = self.editor.clone() {
                cx.defer(move |cx| {
                    let _ = handle.update(cx, |_, w, cx| {
                        editor.update(cx, |e, cx| e.receive(None, enabled, w, cx))
                    });
                });
            }
        }
        let app = cx.global_mut::<AppState>();
        if app.menu != next {
            app.menu = next;
            refresh_menus(cx);
        }
        self.finish_exit(cx);
        cx.notify();
    }
    pub(super) fn locked(&self) -> bool {
        self.state.hardware_busy
            || self.worker.hardware_busy.load(Ordering::SeqCst)
            || self.updates.blocks_hardware()
    }
    pub(super) fn editable(&self) -> bool {
        !self.locked() && self.state.connection.access() == DeviceAccess::Ready
    }
    fn clear_local_error(&mut self, cx: &mut Context<Self>) {
        if self.local_error.take().is_some() {
            cx.notify();
        }
    }

    pub(super) fn select(
        &mut self,
        port: host::usb::Port,
        _w: &mut Window,
        _cx: &mut Context<Self>,
    ) {
        self.clear_local_error(_cx);
        if self.state.connection.port() == Some(&port) || self.locked() {
            return;
        }
        self.pending_install = None;
        #[cfg(debug_assertions)]
        if self.preview == Some(8) {
            let mut next = self.state.clone();
            let Some(info) = next.connection.info().cloned() else {
                return;
            };
            next.connection = host::worker::ConnectionState::Ready {
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
    fn previewing(&self) -> bool {
        #[cfg(debug_assertions)]
        {
            self.preview.is_some()
        }
        #[cfg(not(debug_assertions))]
        {
            false
        }
    }

    pub(super) fn send(&self, command: Command) {
        if self.previewing() {
            return;
        }
        if (matches!(command, Command::DismissError) || !self.locked())
            && command.allowed(&self.state.connection)
        {
            let _ = self
                .worker
                .commands
                .send(self.state.connection.target(), command);
        }
    }
    pub(super) fn app_update(&mut self, cx: &mut Context<Self>) {
        self.clear_local_error(cx);
        if self.locked() {
            return;
        }
        if self.previewing() {
            return;
        }
        match self.updates.state {
            AppUpdate::ReadyToRestart { .. } => self.request_exit(PendingExit::Restart, cx),
            AppUpdate::Available(_) => self.updates.install(),
            _ => {
                self.updates.check(false);
                if matches!(
                    self.updates.state,
                    AppUpdate::Checking { background: false }
                ) {
                    self.latest_check = CheckFeedback::App;
                }
            }
        }
        self.refresh_shell(cx);
    }
    pub(super) fn files(
        &mut self,
        action: FileAction,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.clear_local_error(cx);
        let access = self.state.connection.access();
        let available = match action {
            FileAction::Package => access.can_install(),
            FileAction::Export => access == DeviceAccess::Ready,
            FileAction::Import => access.can_write_save(),
        };
        if self.locked() || !available {
            return;
        }
        if matches!(action, FileAction::Package) {
            self.pending_install = None;
            self.refresh_shell(cx);
        }
        let directory = directories::UserDirs::new()
            .map(|dirs| dirs.document_dir().unwrap_or(dirs.home_dir()).to_owned())
            .unwrap_or_default();
        let target = self.state.connection.target();
        cx.spawn_in(window, async move |view, cx| {
            let result = match action {
                FileAction::Export => {
                    let Ok(prompt) =
                        cx.update(|_, cx| cx.prompt_for_new_path(&directory, Some("eeprom.bin")))
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
                let package = if matches!(action, FileAction::Package) {
                    let source = path.clone();
                    Some(
                        cx.background_executor()
                            .spawn(async move { host::release::Package::open(&source) })
                            .await,
                    )
                } else {
                    None
                };
                let _ = view.update_in(cx, |view, window, cx| {
                    if view.locked() || target != view.state.connection.target() {
                        return;
                    }
                    match (image, package) {
                        (Some(Ok(image)), _) => {
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
                        (Some(Err(error)), _) | (_, Some(Err(error))) => {
                            view.local_error = Some(error.to_string());
                            cx.notify();
                        }
                        (_, Some(Ok(package))) => view.confirm_package(package, window, cx),
                        _ => view.send(Command::Export(path)),
                    }
                });
            } else if let Ok(Err(error)) = result {
                let _ = view.update(cx, |view, cx| {
                    view.local_error = Some(error.to_string());
                    cx.notify();
                });
            }
        })
        .detach();
    }

    pub(super) fn show_diagnostics(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        let Some(info) = self.state.connection.info() else {
            return;
        };
        let detail = match &info.diagnostics {
            None => "This firmware keeps no diagnostics. Update the Stick to record them.".to_string(),
            Some(diagnostics) => {
                let mut lines = Vec::new();
                if diagnostics.resets == 0 {
                    lines.push("No unexpected restarts.".to_string());
                } else {
                    lines.push(format!("Unexpected restarts: {}", diagnostics.resets));
                    for reset in &diagnostics.recent {
                        lines.push(format!(
                            "{} while {}, {} s after starting",
                            reset.cause, reset.activity, reset.uptime_s
                        ));
                    }
                }
                lines.push(String::new());
                lines.push(match &diagnostics.link {
                    None => "No connection since the Stick last lost power.".to_string(),
                    Some(link) => format!(
                        "Last connection: {}. {} packets received, {} damaged. Last command 0x{:02X}.",
                        link.outcome, link.packets, link.damaged, link.last_command
                    ),
                });
                lines.join("\n")
            }
        };
        let answer = w.prompt(PromptLevel::Info, "Diagnostics", Some(&detail), &["OK"], cx);
        cx.spawn_in(w, async move |_, _| {
            let _ = answer.await;
        })
        .detach();
    }
    pub(super) fn reset(&mut self, erase: bool, w: &mut Window, cx: &mut Context<Self>) {
        self.clear_local_error(cx);
        let command = if erase {
            Command::CleanInstall
        } else {
            Command::Reset
        };
        if self.locked()
            || (erase && self.pending_install.is_some())
            || !command.allowed(&self.state.connection)
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
            if erase { "Erase and Install" } else { "Reset" },
            command,
            w,
            cx,
        );
    }
    fn confirm_package(
        &self,
        package: host::release::Package,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let installed = self
            .state
            .connection
            .info()
            .is_some_and(|info| info.build == package.build());
        let title = format!(
            "Install {} on {}?",
            package.manifest.version,
            self.device_label()
        );
        let detail = if matches!(self.state.connection, ConnectionState::Unmanaged { .. }) {
            REPLACES_SOFTWARE
        } else if installed {
            "This build is already on the Stick. Your save and settings are kept."
        } else {
            "Your save and settings are kept."
        };
        self.confirm(
            &title,
            detail,
            "Install",
            Command::InstallPackage(Box::new(package)),
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
        let epoch = self.connection_epoch;
        let answer = w.prompt(
            PromptLevel::Warning,
            title,
            Some(detail),
            &["Cancel", action],
            cx,
        );
        cx.spawn_in(w, async move |view, cx| {
            if answer.await == Ok(1) {
                let _ = view.update(cx, |v, cx| {
                    if !target
                        .as_ref()
                        .zip(v.state.connection.target().as_ref())
                        .is_some_and(|(expected, current)| expected.matches(current))
                        || epoch != v.connection_epoch
                        || v.locked()
                        || !command.allowed(&v.state.connection)
                    {
                        return;
                    }
                    if matches!(command, Command::Update | Command::CleanInstall) {
                        v.start_install(command, cx);
                    } else {
                        v.send(command);
                    }
                });
            }
        })
        .detach();
    }
    fn change_palette(&mut self, settings: host::DeviceSettings, cx: &mut Context<Self>) {
        if self.editable()
            && let Some((handle, editor)) = self.editor.clone()
        {
            let _ = handle.update(cx, |_, w, cx| {
                editor.update(cx, |e, cx| e.receive(Some(settings), true, w, cx))
            });
        }
        self.submit_palette(settings, cx);
    }
    fn issue_palette_revision(&mut self) -> u64 {
        self.next_revision += 1;
        self.pending_revision = Some(self.next_revision);
        self.next_revision
    }

    fn submit_palette(&mut self, settings: host::DeviceSettings, cx: &mut Context<Self>) {
        if !self.editable()
            || self
                .state
                .connection
                .info()
                .is_none_or(|i| i.settings == settings)
        {
            return;
        }
        self.clear_local_error(cx);
        let revision = self.issue_palette_revision();
        if self.previewing() {
            self.state.acked_revision = revision;
            self.pending_revision = None;
        } else if let Err(error) = self.worker.commands.send(
            self.state.connection.target(),
            Command::SetSettings { revision, settings },
        ) {
            self.pending_revision = None;
            self.local_error = Some(error.to_string());
            cx.notify();
            return;
        }
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
        cx.notify();
    }
    pub(super) fn palette_file(
        &mut self,
        action: PaletteAction,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.clear_local_error(cx);
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
                        v.local_error = Some(format!("{e:#}"));
                        cx.notify();
                    }
                    Ok(None) => {}
                }
            });
        })
        .detach();
    }
    pub(super) fn open_palette(&mut self, paste: bool, cx: &mut Context<Self>) {
        self.clear_local_error(cx);
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
                    self.local_error = Some(e.to_string());
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
                        v.submit_palette(event.0, cx)
                    }));
                self.editor = Some((handle, editor));
            }
            Err(error) => {
                self.local_error = Some(error.to_string());
                cx.notify();
            }
        }
    }
    pub(super) fn copy_palette(&mut self, cx: &mut Context<Self>) {
        self.clear_local_error(cx);
        if let Some(info) = self.state.connection.info() {
            cx.write_to_clipboard(ClipboardItem::new_string(host::palette::format(
                &info.settings.slots[info.settings.active_palette.index()],
            )));
        }
    }
    pub(super) fn offer_update(&self) -> bool {
        self.state.connection.info().is_some_and(|info| {
            self.state
                .firmware
                .as_ref()
                .is_some_and(|f| f.newer_than(&info.version))
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
        if self.state.connection.port().is_none() {
            return "No Device Connected".into();
        }
        if self.state.connection.info().is_none() {
            return self.state.firmware.as_ref().map_or_else(
                || "Install Stickwalker".into(),
                |f| format!("Install Stickwalker {}", f.version),
            );
        }
        let Some(firmware) = &self.state.firmware else {
            return "Update Firmware".into();
        };
        if self.offer_update() {
            format!("Update Firmware to {}", firmware.version)
        } else {
            "Firmware Up to Date".into()
        }
    }
}
