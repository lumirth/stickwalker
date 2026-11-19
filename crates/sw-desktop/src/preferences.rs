use super::*;
#[derive(Clone, Copy, serde::Serialize, serde::Deserialize)]
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
struct Store {
    value: Preferences,
    appearance: async_channel::Sender<Option<bool>>,
    edited: bool,
}
impl Global for Store {}

pub fn current(cx: &App) -> Preferences {
    cx.try_global::<Store>()
        .map(|store| store.value)
        .unwrap_or_default()
}

pub fn start(cx: &mut App) {
    let (writes, rx) = async_channel::unbounded::<Option<bool>>();
    let (initial, loaded) = async_channel::bounded(1);
    let (finished, done) = async_channel::bounded(1);
    cx.set_global(Store {
        value: Preferences::default(),
        appearance: writes.clone(),
        edited: false,
    });
    cx.background_executor()
        .spawn(async move {
            let path = host::data_dir().map(|root| root.join("preferences.json"));
            let mut preferences: Preferences = path
                .as_ref()
                .and_then(|p| host::save::read_limited(p, 4096).ok())
                .and_then(|bytes| serde_json::from_slice(&bytes).ok())
                .unwrap_or_default();
            let _ = initial.send(preferences).await;
            while let Ok(mut dark) = rx.recv().await {
                while let Ok(newer) = rx.try_recv() {
                    dark = newer;
                }
                preferences.dark = dark;
                if let (Some(path), Ok(bytes)) = (&path, serde_json::to_vec(&preferences)) {
                    let _ = host::save::atomic_write(path, &bytes);
                }
            }
            let _ = finished.send(()).await;
        })
        .detach();
    cx.spawn(async move |cx| {
        if let Ok(value) = loaded.recv().await {
            cx.update(|cx| {
                let store = cx.global_mut::<Store>();
                store.value.automatic_updates = value.automatic_updates;
                if !store.edited {
                    store.value.dark = value.dark;
                }
                if let Some(window) = cx
                    .active_window()
                    .or_else(|| cx.try_global::<AppState>().map(|app| app.main_window))
                {
                    let _ = window.update(cx, |_, w, cx| apply(w, cx));
                    cx.refresh_windows();
                    if cx.has_global::<AppState>() {
                        refresh_menus(cx);
                    }
                }
            });
        }
    })
    .detach();
    cx.on_app_quit(move |_| {
        writes.close();
        let done = done.clone();
        async move {
            let _ = done.recv().await;
        }
    })
    .detach();
}
pub fn set_appearance(dark: Option<bool>, window: &mut Window, cx: &mut App) {
    let store = cx.global_mut::<Store>();
    store.value.dark = dark;
    store.edited = true;
    let _ = store.appearance.try_send(dark);
    apply(window, cx);
    cx.refresh_windows();
    refresh_menus(cx);
}
pub fn apply(window: &Window, cx: &mut App) {
    let dark = current(cx).dark;
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
