pub(crate) mod color_field;
mod render;
mod scene;
pub(crate) mod selector;
#[cfg(test)]
mod tests;
use crate::assets;
use anyhow::Result;
use gpui_kit::base::StyledExt;
use gpui_kit::component::{
    ActiveTheme, Disableable, Selectable, Sizable,
    button::{Button, ButtonGroup, ButtonVariants},
    input::{Input, InputEvent, InputState},
    native_menu::NativeMenu,
    select::SelectItem,
};
use gpui_kit::prelude::FluentBuilder;
use gpui_kit::*;
use host::{DeviceSettings, PaletteName, PaletteSlot, palette as text, rgb888};
#[derive(Clone)]
pub(crate) struct Choice {
    pub value: usize,
    pub title: SharedString,
    pub colors: [u16; 4],
}
impl Choice {
    pub(crate) fn row(&self, cx: &App) -> AnyElement {
        div()
            .h_flex()
            .items_center()
            .w_full()
            .min_w_0()
            .gap_2()
            .child(
                div()
                    .h_flex()
                    .flex_shrink_0()
                    .border_1()
                    .border_color(cx.theme().border)
                    .rounded_sm()
                    .overflow_hidden()
                    .children(
                        self.colors
                            .map(|color| div().w_3().h_4().bg(rgb(rgb888(color)))),
                    ),
            )
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .truncate()
                    .child(self.title.clone()),
            )
            .into_any_element()
    }
}
#[derive(IntoElement)]
struct PaletteChoice(Choice);
impl RenderOnce for PaletteChoice {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        self.0.row(cx)
    }
}
impl SelectItem for Choice {
    type Value = usize;
    fn title(&self) -> SharedString {
        self.title.clone()
    }
    fn value(&self) -> &usize {
        &self.value
    }
    fn display_title(&self) -> Option<AnyElement> {
        Some(PaletteChoice(self.clone()).into_any_element())
    }
    fn render(&self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        self.row(cx)
    }
}

pub struct PaletteChanged(pub DeviceSettings);
#[derive(Clone, Copy)]
pub enum PaletteAction {
    CopyAll,
    PasteSet,
    Import,
    Export,
    ResetAll,
}
#[derive(Clone, Copy)]
enum Midtones {
    Manual,
    Ramp(Option<[u16; 2]>),
}
impl Midtones {
    fn automatic(self) -> bool {
        matches!(self, Self::Ramp(_))
    }
    fn from_colors(colors: [u16; 4]) -> Self {
        if text::is_ramp(colors) {
            Self::Ramp(None)
        } else {
            Self::Manual
        }
    }
}
pub struct PaletteEditor {
    #[cfg(not(target_os = "macos"))]
    menu_bar: Entity<gpui_kit::component::menu::AppMenuBar>,
    settings: DeviceSettings,
    owner: AnyWindowHandle,
    picker: crate::palette_window::color_field::Picker,
    dragging: Option<crate::palette_window::color_field::Surface>,
    hex: Entity<InputState>,
    shade: usize,
    focus: FocusHandle,
    name: Entity<InputState>,

    midtones: [Midtones; 6],
    enabled: bool,
    error: Option<String>,
    _subscriptions: Vec<Subscription>,
}
impl EventEmitter<PaletteChanged> for PaletteEditor {}
pub(crate) async fn palette_file(
    action: PaletteAction,
    contents: String,
    cx: &mut AsyncWindowContext,
) -> Result<Option<String>> {
    if matches!(action, PaletteAction::Export) {
        let directory = directories::UserDirs::new()
            .map(|d| d.home_dir().to_owned())
            .unwrap_or_default();
        if let Some(path) = cx
            .update(|_, cx| cx.prompt_for_new_path(&directory, Some("palettes.txt")))?
            .await??
        {
            cx.background_executor()
                .spawn(async move { host::save::atomic_write(&path, contents.as_bytes()) })
                .await?;
        }
        Ok(None)
    } else {
        let paths = cx
            .update(|_, cx| {
                cx.prompt_for_paths(PathPromptOptions {
                    files: true,
                    directories: false,
                    multiple: false,
                    prompt: Some("Import palettes".into()),
                })
            })?
            .await??;
        let Some(path) = paths.and_then(|p| p.into_iter().next()) else {
            return Ok(None);
        };
        cx.background_executor()
            .spawn(async move {
                let bytes = host::save::read_limited(&path, 64 * 1024)?;
                Ok(Some(String::from_utf8(bytes)?))
            })
            .await
    }
}
impl PaletteEditor {
    pub fn new(
        settings: DeviceSettings,
        owner: AnyWindowHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        crate::preferences::apply(window, cx);
        let hex = cx.new(|cx| InputState::new(window, cx).placeholder("#RRGGBB"));
        let name = cx.new(|cx| InputState::new(window, cx).placeholder("Palette name"));
        let subscriptions = [&hex, &name]
            .into_iter()
            .map(|input| {
                cx.subscribe_in(input, window, |v: &mut Self, _, event, w, cx| {
                    if matches!(event, InputEvent::PressEnter { .. } | InputEvent::Blur) {
                        v.commit(w, cx, matches!(event, InputEvent::PressEnter { .. }));
                    }
                })
            })
            .collect();
        let owner_view = cx.weak_entity();
        window.on_window_should_close(cx, move |w, cx| {
            let _ = owner_view.update(cx, |v: &mut Self, cx| v.commit_inputs(w, cx));
            true
        });
        #[cfg(not(target_os = "macos"))]
        let menu_bar = {
            let bar = gpui_kit::component::menu::AppMenuBar::new(cx);
            if cx.has_global::<crate::AppState>() {
                cx.global_mut::<crate::AppState>().editor_bar = Some(bar.downgrade());
            }
            bar
        };
        let mut editor = Self {
            #[cfg(not(target_os = "macos"))]
            menu_bar,
            settings,
            owner,
            picker: crate::palette_window::color_field::Picker::new(),
            dragging: None,
            hex,
            shade: 0,
            focus: cx.focus_handle(),
            name,
            midtones: settings.slots.map(|s| Midtones::from_colors(s.colors)),
            enabled: true,
            error: None,
            _subscriptions: subscriptions,
        };
        window.focus(&editor.focus, cx);
        editor.sync(window, cx);
        editor.sync_name(window, cx);
        editor
    }
    pub fn receive(
        &mut self,
        settings: Option<DeviceSettings>,
        enabled: bool,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let mut changed = self.enabled != enabled;
        self.enabled = enabled;
        if !enabled {
            self.dragging = None;
        }
        if let Some(settings) = settings
            && settings != self.settings
        {
            for (i, slot) in settings.slots.iter().enumerate() {
                if slot.colors != self.settings.slots[i].colors {
                    self.midtones[i] = Midtones::from_colors(slot.colors);
                }
            }
            let slot_changed = settings.active_palette != self.settings.active_palette;
            let old_slot = self.settings.slots[self.settings.active_palette.index()];
            let new_slot = settings.slots[settings.active_palette.index()];
            let old_shade = self.shade;
            self.settings = settings;
            self.sync(w, cx);
            if slot_changed
                || old_shade != self.shade
                || old_slot.colors[self.shade] != new_slot.colors[self.shade]
            {
                self.sync_hex(w, cx);
            }
            if slot_changed
                || old_slot.name != new_slot.name
                || !self.name.read(cx).focus_handle(cx).is_focused(w)
            {
                self.sync_name(w, cx);
            }
            changed = true;
        }
        if changed {
            cx.notify();
        }
    }
    fn sync_name(&self, w: &mut Window, cx: &mut Context<Self>) {
        let value = self.settings.slots[self.settings.active_palette.index()]
            .name
            .as_str();
        if self.name.read(cx).value().as_ref() != value {
            self.name
                .update(cx, |input, cx| input.set_value(value, w, cx));
        }
    }
    fn sync(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        if self.midtones[self.settings.active_palette.index()].automatic()
            && matches!(self.shade, 1 | 2)
        {
            self.shade = 0;
        }
        self.picker.sync_color(self.settings.colors()[self.shade]);
        if !self.hex.read(cx).focus_handle(cx).is_focused(w) {
            self.sync_hex(w, cx);
        }
    }
    pub(crate) fn commit_inputs(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        self.commit(w, cx, false);
    }
    fn commit(&mut self, w: &mut Window, cx: &mut Context<Self>, validate: bool) {
        let previous = self.settings;
        self.apply_inputs(w, cx, validate);
        if previous != self.settings {
            self.changed(w, cx);
        }
    }
    pub(crate) fn take_committed_settings(
        &mut self,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) -> DeviceSettings {
        self.apply_inputs(w, cx, false);
        self.settings
    }
    fn apply_inputs(&mut self, w: &mut Window, cx: &mut Context<Self>, validate: bool) {
        if !self.enabled {
            return;
        }
        let i = self.settings.active_palette.index();
        if let Some(name) = PaletteName::from_user_input(&self.name.read(cx).value()) {
            self.settings.slots[i].name = name;
        }
        let hex = self.hex.read(cx).value();
        let invalid_hex = "Enter a six-digit hex color.";
        let color = text::parse_hex(hex.trim()).ok();
        if let Some(color) = color {
            self.settings.slots[i].colors[self.shade] = color.rgb565();
            if self.midtones[i].automatic() {
                self.blend();
            }
            if self.error.as_deref() == Some(invalid_hex) {
                self.error = None;
                cx.notify();
            }
        }
        self.sync_name(w, cx);
        if color.is_none() {
            if validate {
                self.show_error(invalid_hex.into(), w, cx);
            } else {
                self.sync_hex(w, cx);
                if self.error.as_deref() == Some(invalid_hex) {
                    self.error = None;
                    cx.notify();
                }
            }
        }
    }
    fn sync_hex(&self, w: &mut Window, cx: &mut Context<Self>) {
        let color = rgb888(self.settings.colors()[self.shade]);
        self.hex.update(cx, |input, cx| {
            input.set_value(format!("#{color:06X}"), w, cx)
        });
    }
    fn show_error(&mut self, error: String, _: &mut Window, cx: &mut Context<Self>) {
        self.error = Some(error);
        cx.notify();
    }
    fn set_color(&mut self, color: u16, w: &mut Window, cx: &mut Context<Self>) {
        let slot = self.settings.active_palette.index();
        self.settings.slots[slot].colors[self.shade] = color;
        if self.midtones[slot].automatic() {
            self.blend();
        }
        self.changed(w, cx);
    }
    fn set_two_color(&mut self, enabled: bool, w: &mut Window, cx: &mut Context<Self>) {
        let slot = self.settings.active_palette.index();
        self.commit_inputs(w, cx);
        if self.midtones[slot].automatic() == enabled {
            return;
        }
        if enabled {
            let [_, light, dark, _] = self.settings.slots[slot].colors;
            self.midtones[slot] = Midtones::Ramp(Some([light, dark]));
            if matches!(self.shade, 1 | 2) {
                self.shade = 0;
            }
            self.blend();
        } else {
            if let Midtones::Ramp(Some(pair)) = self.midtones[slot] {
                self.settings.slots[slot].colors[1..3].copy_from_slice(&pair);
            }
            self.midtones[slot] = Midtones::Manual;
        }
        self.changed(w, cx);
    }
    fn changed(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        self.error = None;
        self.sync(w, cx);
        self.sync_hex(w, cx);
        cx.emit(PaletteChanged(self.settings));
        cx.notify();
    }
    fn blend(&mut self) {
        let colors = &mut self.settings.slots[self.settings.active_palette.index()].colors;
        *colors = text::ramp(colors[0], colors[3]);
    }
    pub fn copy(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        self.commit_inputs(w, cx);
        cx.write_to_clipboard(ClipboardItem::new_string(text::format(
            &self.settings.slots[self.settings.active_palette.index()],
        )));
    }
    pub fn paste(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        if !self.enabled {
            return;
        }
        let result = cx
            .read_from_clipboard()
            .and_then(|item| item.text())
            .ok_or_else(|| anyhow::anyhow!("Clipboard does not contain a palette."))
            .and_then(|s| text::parse(&s));
        match result {
            Ok(palette) => {
                self.apply(self.settings.active_palette.index(), palette);
                self.sync_name(w, cx);
                self.changed(w, cx);
            }
            Err(error) => {
                self.show_error(error.to_string(), w, cx);
            }
        }
    }
    fn apply(&mut self, index: usize, palette: text::ParsedPalette) {
        if let Some(name) = palette.name {
            self.settings.slots[index].name = name;
        }
        self.settings.slots[index].colors = palette.colors;
        self.midtones[index] = if palette.two_color {
            Midtones::Ramp(None)
        } else {
            Midtones::Manual
        };
    }
    fn apply_set(&mut self, contents: &str, w: &mut Window, cx: &mut Context<Self>) {
        match text::parse_many(contents) {
            Ok(palettes) => {
                for (index, palette) in palettes.into_iter().enumerate() {
                    self.apply(index, palette);
                }
                self.sync_name(w, cx);
                self.changed(w, cx);
            }
            Err(error) => {
                self.show_error(format!("{error:#}"), w, cx);
            }
        }
    }
    fn all_text(&self) -> String {
        self.settings
            .slots
            .iter()
            .map(text::format)
            .collect::<Vec<_>>()
            .join("\n")
    }
    pub fn action(&mut self, action: PaletteAction, w: &mut Window, cx: &mut Context<Self>) {
        if !self.enabled {
            return;
        }
        self.commit_inputs(w, cx);
        match action {
            PaletteAction::CopyAll => {
                cx.write_to_clipboard(ClipboardItem::new_string(self.all_text()))
            }
            PaletteAction::PasteSet => {
                let contents = cx
                    .read_from_clipboard()
                    .and_then(|item| item.text())
                    .unwrap_or_default();
                self.apply_set(&contents, w, cx);
            }
            PaletteAction::ResetAll => {
                let answer = w.prompt(
                    PromptLevel::Warning,
                    "Restore all palette defaults?",
                    Some("Replaces the names and colors of all six palettes."),
                    &["Cancel", "Restore defaults"],
                    cx,
                );
                cx.spawn_in(w, async move |view, cx| {
                    if answer.await == Ok(1) {
                        let _ = view.update_in(cx, |v, w, cx| {
                            if v.enabled {
                                v.settings.slots = DeviceSettings::default().slots;
                                v.midtones =
                                    v.settings.slots.map(|s| Midtones::from_colors(s.colors));
                                v.sync_name(w, cx);
                                v.changed(w, cx);
                            }
                        });
                    }
                })
                .detach();
            }
            PaletteAction::Import | PaletteAction::Export => self.file(action, w, cx),
        }
    }
    fn file(&self, action: PaletteAction, w: &mut Window, cx: &mut Context<Self>) {
        let contents = self.all_text();
        cx.spawn_in(w, async move |view, cx| {
            let result = palette_file(action, contents, cx).await;
            let _ = view.update_in(cx, |v, w, cx| match result {
                Ok(Some(contents)) if v.enabled => v.apply_set(&contents, w, cx),
                Err(error) => {
                    v.show_error(error.to_string(), w, cx);
                }
                _ => {}
            });
        })
        .detach();
    }
    fn more(&self, cx: &Context<Self>) -> impl IntoElement {
        Button::new("palette-more")
            .small()
            .ghost()
            .icon(assets::icon("more"))
            .label("Palette set")
            .accessibility_label("More palette actions")
            .disabled(!self.enabled)
            .on_click(cx.listener(|_, _, w, cx| {
                NativeMenu::new()
                    .menu("Import Palettes…", Box::new(crate::ImportPalettes))
                    .menu("Export Palettes…", Box::new(crate::ExportPalettes))
                    .separator()
                    .menu("Copy All Palettes", Box::new(crate::CopyAllPalettes))
                    .menu("Paste Palette Set", Box::new(crate::PastePaletteSet))
                    .separator()
                    .menu(
                        "Restore Default Palettes…",
                        Box::new(crate::ResetAllPalettes),
                    )
                    .show(w.mouse_position(), w, cx);
            }))
    }
}
