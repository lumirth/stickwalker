use crate::palette_window::Choice;
use crate::*;
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use host::DeviceSettings;

pub struct PaletteSelected(pub u8);
pub struct PaletteSelector {
    choices: Entity<SelectState<Vec<Choice>>>,
    enabled: bool,
    settings: DeviceSettings,
    _subscription: Subscription,
}
impl EventEmitter<PaletteSelected> for PaletteSelector {}
impl PaletteSelector {
    pub fn new(w: &mut Window, cx: &mut Context<Self>) -> Self {
        let choices = cx.new(|cx| SelectState::new(Vec::<Choice>::new(), None, w, cx));
        let subscription = cx.subscribe(&choices, |v: &mut Self, _, event, cx| {
            if v.enabled
                && let SelectEvent::Confirm(Some(slot)) = event
            {
                cx.emit(PaletteSelected(*slot as u8));
            }
        });
        Self {
            choices,
            enabled: false,
            settings: DeviceSettings::default(),
            _subscription: subscription,
        }
    }
    pub fn receive(
        &mut self,
        settings: Option<DeviceSettings>,
        enabled: bool,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let changed = self.enabled != enabled || settings.is_some_and(|s| s != self.settings);
        self.enabled = enabled;
        if let Some(settings) = settings {
            self.settings = settings;
        }
        if changed || self.choices.read(cx).selected_value().is_none() {
            let choices = self
                .settings
                .slots
                .iter()
                .enumerate()
                .map(|(value, slot)| Choice {
                    value,
                    title: slot.name.as_str().to_owned().into(),
                    colors: slot.colors,
                })
                .collect();
            self.choices.update(cx, |state, cx| {
                state.set_items(choices, w, cx);
                state.set_selected_value(&(self.settings.active_palette as usize), w, cx);
            });
            cx.notify();
        }
    }
}
impl Render for PaletteSelector {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        Select::new(&self.choices)
            .small()
            .disabled(!self.enabled)
            .accessibility_label("Palette")
    }
}
