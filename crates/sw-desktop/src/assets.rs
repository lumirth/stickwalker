use super::*;
use gpui_kit::component::Icon;
use std::borrow::Cow;

pub struct Assets;
impl AssetSource for Assets {
    fn load(&self, path: &str) -> anyhow::Result<Option<Cow<'static, [u8]>>> {
        let bytes: &'static [u8] = match path {
            "solar/copy" => include_bytes!("../assets/icons/copy.svg"),
            "solar/paste" => include_bytes!("../assets/icons/paste.svg"),
            "solar/more" => include_bytes!("../assets/icons/more.svg"),
            "solar/chevron" => include_bytes!("../assets/icons/chevron.svg"),
            "solar/charge" => include_bytes!("../assets/icons/charge.svg"),
            "solar/full" => include_bytes!("../assets/icons/full.svg"),
            "solar/half" => include_bytes!("../assets/icons/half.svg"),
            "solar/low" => include_bytes!("../assets/icons/low.svg"),
            "stickwalker.png" => include_bytes!("../assets/icon.png"),
            _ => return gpui_kit::assets::Assets.load(path),
        };
        Ok(Some(Cow::Borrowed(bytes)))
    }
    fn list(&self, path: &str) -> anyhow::Result<Vec<SharedString>> {
        gpui_kit::assets::Assets.list(path)
    }
}
pub fn icon(name: &str) -> Icon {
    Icon::default().path(format!("solar/{name}"))
}
pub fn battery(percent: u32, external: bool) -> Icon {
    icon(if external && percent < 100 {
        "charge"
    } else if percent < 20 {
        "low"
    } else if percent < 75 {
        "half"
    } else {
        "full"
    })
}
