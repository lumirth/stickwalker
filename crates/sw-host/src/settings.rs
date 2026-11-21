use anyhow::{Result, ensure};

pub const SLOT_COUNT: usize = 6;
pub const SETTINGS_SIZE: usize = 151;
pub const PALETTE_ONLY: u8 = 0x80;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaletteName([u8; 16]);
impl PaletteName {
    pub fn new(name: &str) -> Result<Self> {
        let name = name.trim();
        ensure!(
            !name.is_empty()
                && name.len() <= 15
                && name.bytes().all(|b| (32..=126).contains(&b) && b != b'|'),
            "Use 1–15 standard letters, numbers, or symbols. Leave out |."
        );
        let mut bytes = [0; 16];
        bytes[..name.len()].copy_from_slice(name.as_bytes());
        Ok(Self(bytes))
    }
    pub fn from_user_input(name: &str) -> Option<Self> {
        let clean: String = name
            .trim()
            .chars()
            .filter(|c| (' '..='~').contains(c) && *c != '|')
            .take(15)
            .collect();
        Self::new(&clean).ok()
    }
    pub fn as_str(&self) -> &str {
        let end = self.0.iter().position(|b| *b == 0).unwrap_or(16);
        std::str::from_utf8(&self.0[..end]).expect("validated palette name")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaletteSlot {
    pub name: PaletteName,
    pub colors: [u16; 4],
}
impl PaletteSlot {
    pub fn default_at(slot: usize) -> Self {
        Self {
            name: PaletteName::new(crate::PALETTE_NAMES[slot]).unwrap(),
            colors: crate::DEFAULT_COLORS[slot].map(crate::rgb565),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PaletteIndex(u8);

impl PaletteIndex {
    pub fn new(index: usize) -> Result<Self> {
        ensure!(index < SLOT_COUNT, "Choose one of the six palettes.");
        Ok(Self(index as u8))
    }
    pub fn index(self) -> usize {
        usize::from(self.0)
    }
    pub const GREYSCALE: Self = Self(2);
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceSettings {
    pub active_palette: PaletteIndex,
    pub reverse: bool,
    pub layout: u8,
    pub mappings: [u8; 4],
    pub slots: [PaletteSlot; SLOT_COUNT],
}
impl Default for DeviceSettings {
    fn default() -> Self {
        Self {
            active_palette: PaletteIndex::default(),
            reverse: false,
            layout: 1,
            mappings: [1, 0, 4, 1],
            slots: std::array::from_fn(PaletteSlot::default_at),
        }
    }
}
impl DeviceSettings {
    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        // Orientation, layout and mappings belong to the Stick. Desktop carries
        // them through unchanged, so a newer firmware's values are not an error.
        ensure!(bytes.len() == SETTINGS_SIZE, "Unsupported device settings");
        let mut slots = Self::default().slots;
        for (index, slot) in slots.iter_mut().enumerate() {
            let name = &bytes[55 + index * 16..55 + (index + 1) * 16];
            let end = name
                .iter()
                .position(|b| *b == 0)
                .ok_or_else(|| anyhow::anyhow!("Invalid palette name"))?;
            slot.name = PaletteName::new(std::str::from_utf8(&name[..end])?)?;
            for (shade, color) in slot.colors.iter_mut().enumerate() {
                let at = 7 + index * 8 + shade * 2;
                *color = u16::from_le_bytes(bytes[at..at + 2].try_into().unwrap());
            }
        }
        Ok(Self {
            active_palette: PaletteIndex::new(usize::from(bytes[0]))?,
            reverse: bytes[1] != 0,
            layout: bytes[2],
            mappings: bytes[3..7].try_into().unwrap(),
            slots,
        })
    }
    pub fn to_bytes(self) -> [u8; SETTINGS_SIZE] {
        let mut bytes = [0; SETTINGS_SIZE];
        bytes[..3].copy_from_slice(&[self.active_palette.0, self.reverse as u8, self.layout]);
        bytes[3..7].copy_from_slice(&self.mappings);
        for (index, slot) in self.slots.iter().enumerate() {
            for (shade, color) in slot.colors.iter().enumerate() {
                let at = 7 + index * 8 + shade * 2;
                bytes[at..at + 2].copy_from_slice(&color.to_le_bytes());
            }
            bytes[55 + index * 16..55 + (index + 1) * 16].copy_from_slice(&slot.name.0);
        }
        bytes
    }
    pub fn palette_bytes(self) -> [u8; SETTINGS_SIZE] {
        let mut bytes = self.to_bytes();
        bytes[0] |= PALETTE_ONLY;
        bytes
    }
    pub fn colors(&self) -> [u16; 4] {
        self.slots[self.active_palette.index()].colors
    }
}
