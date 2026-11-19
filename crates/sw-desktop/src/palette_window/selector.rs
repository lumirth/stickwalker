use crate::palette_window::Choice;
use crate::*;
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use host::DeviceSettings;

pub struct PaletteSelected(pub host::PaletteIndex);
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
                && let Ok(index) = host::PaletteIndex::new(*slot)
            {
                cx.emit(PaletteSelected(index));
            }
        });
        Self {
            choices,
            enabled: false,
            settings: DeviceSettings::default(),
            _subscription: subscription,
        }
    }
    pub fn set_enabled(&mut self, enabled: bool, cx: &mut Context<Self>) {
        if self.enabled != enabled {
            self.enabled = enabled;
            cx.notify();
        }
    }
    pub fn receive(
        &mut self,
        settings: Option<DeviceSettings>,
        enabled: bool,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let changed = settings.is_some_and(|s| s != self.settings);
        self.set_enabled(enabled, cx);
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
                state.set_selected_value(&(self.settings.active_palette.index()), w, cx);
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

#[cfg(all(test, debug_assertions))]
mod tests {
    use gpui_kit::{AppContext, TestAppContext, WindowOptions};

    #[gpui_kit::test]
    fn an_app_install_locks_the_palette_selector_between_device_polls(cx: &mut TestAppContext) {
        use crate::{device_window::StickView, updater::AppUpdate};

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
                assert!(v.palette.read(cx).enabled);
                v.updates.state = AppUpdate::Installing;
                v.refresh_shell(cx);
                assert!(!v.palette.read(cx).enabled);
                v.updates.state = AppUpdate::Idle;
                v.refresh_shell(cx);
                assert!(v.palette.read(cx).enabled);
            });
        })
        .unwrap();
    }
}
