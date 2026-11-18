pub mod flash;
pub mod legacy;
pub mod palette;
pub mod protocol;
pub mod release;
pub mod save;
pub mod settings;
pub mod usb;
pub mod worker;
pub use settings::{DeviceSettings, PaletteName, PaletteSlot, SLOT_COUNT};

pub const PALETTE_NAMES: [&str; 6] = [
    "Walker LCD",
    "Greyscale 2",
    "Greyscale",
    "Poketch",
    "DMG",
    "Amber",
];
pub const DEFAULT_COLORS: [[u32; 4]; 6] = [
    [0xb7b8b0, 0x808173, 0x666559, 0x1f1a17],
    [0xeaeaea, 0xa4a4a4, 0x5d5d5d, 0x171717],
    [0xffffff, 0xaaaaaa, 0x555555, 0],
    [0x70b070, 0x508050, 0x385030, 0x102818],
    [0xc2c263, 0x5c6b18, 0x17320b, 0x021900],
    [0xf7e2af, 0xd7c092, 0xaf936a, 0x764200],
];
pub fn rgb565(rgb: u32) -> u16 {
    (((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x7e0) | ((rgb >> 3) & 31)) as u16
}
pub fn rgb888(c: u16) -> u32 {
    let c = u32::from(c);
    (((c >> 11) * 255 / 31) << 16) | ((((c >> 5) & 63) * 255 / 63) << 8) | ((c & 31) * 255 / 31)
}
pub fn data_dir() -> std::path::PathBuf {
    directories::ProjectDirs::from("me", "kukas", "Stickwalker")
        .expect("user data directory")
        .data_dir()
        .into()
}
