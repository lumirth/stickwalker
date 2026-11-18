use super::*;
#[derive(serde::Serialize, serde::Deserialize)]
pub struct Preferences {
    #[serde(default)]
    pub dark: Option<bool>,
    #[serde(default = "enabled")]
    pub automatic_updates: bool,
}
fn enabled() -> bool {
    true
}
impl Default for Preferences {
    fn default() -> Self {
        Self {
            dark: None,
            automatic_updates: true,
        }
    }
}
impl Preferences {
    fn save(&self) {
        if let Ok(bytes) = serde_json::to_vec(self) {
            let _ = host::save::atomic_write(&host::data_dir().join("preferences.json"), &bytes);
        }
    }
    pub fn load() -> Self {
        std::fs::read(host::data_dir().join("preferences.json"))
            .ok()
            .and_then(|v| serde_json::from_slice(&v).ok())
            .unwrap_or_default()
    }
}
pub fn set_appearance(dark: Option<bool>, window: &mut Window, cx: &mut App) {
    let mut preferences = Preferences::load();
    preferences.dark = dark;
    preferences.save();
    apply(window, cx);
    cx.refresh_windows();
    refresh_menus(cx);
}
pub fn apply(window: &Window, cx: &mut App) {
    let dark = Preferences::load().dark;
    cx.set_window_appearance(dark.map(|dark| {
        if dark {
            WindowAppearance::Dark
        } else {
            WindowAppearance::Light
        }
    }));
    let mode = match dark {
        Some(true) => ThemeMode::Dark,
        Some(false) => ThemeMode::Light,
        None => window.appearance().into(),
    };
    Theme::change(mode, None, cx);
}
