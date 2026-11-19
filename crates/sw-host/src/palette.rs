use crate::{PaletteName, PaletteSlot, SLOT_COUNT, rgb565, rgb888};
use anyhow::{Context, Result, ensure};

pub struct ParsedPalette {
    pub name: Option<PaletteName>,
    pub colors: [u16; 4],
    pub two_color: bool,
}

/// Hachistep blends endpoints in linear sRGB before quantizing for the display.
pub fn blend_endpoints(panel: u32, ink: u32) -> [u32; 4] {
    fn linear(value: u32) -> f32 {
        let x = value as f32 / 255.;
        if x <= 0.04045 {
            x / 12.92
        } else {
            ((x + 0.055) / 1.055).powf(2.4)
        }
    }
    fn srgb(x: f32) -> u32 {
        let s = if x <= 0.0031308 {
            x * 12.92
        } else {
            1.055 * x.powf(1. / 2.4) - 0.055
        };
        (s.clamp(0., 1.) * 255.).round() as u32
    }
    let blend = |t| {
        [16, 8, 0].into_iter().fold(0, |rgb, shift| {
            let a = linear((panel >> shift) & 255);
            let b = linear((ink >> shift) & 255);
            rgb | (srgb(a + (b - a) * t) << shift)
        })
    };
    [panel, blend(1. / 3.), blend(2. / 3.), ink]
}
/// Canonical ramp for the hardware: quantize endpoints before blending.
pub fn ramp(panel: u16, ink: u16) -> [u16; 4] {
    blend_endpoints(rgb888(panel), rgb888(ink)).map(rgb565)
}
pub fn is_ramp(colors: [u16; 4]) -> bool {
    colors == ramp(colors[0], colors[3])
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb24(u32);

impl Rgb24 {
    pub fn rgb565(self) -> u16 {
        rgb565(self.0)
    }
}

pub fn parse_hex(value: &str) -> Result<Rgb24> {
    let value = value.strip_prefix('#').unwrap_or(value);
    ensure!(
        value.len() == 6 && value.bytes().all(|b| b.is_ascii_hexdigit()),
        "Use six hexadecimal digits for each color."
    );
    Ok(Rgb24(u32::from_str_radix(value, 16)?))
}
pub fn parse(text: &str) -> Result<ParsedPalette> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let fields: Vec<_> = if text.contains('|') {
        text.trim().split('|').map(str::trim).collect()
    } else {
        text.split_whitespace().collect()
    };
    let (name, colors) = match fields.as_slice() {
        [name, _, _] | [name, _, _, _, _] if text.contains('|') => {
            (PaletteName::from_user_input(name), &fields[1..])
        }
        [_, _, _, _] => (None, fields.as_slice()),
        _ => anyhow::bail!("Not a valid palette format."),
    };
    let values = colors
        .iter()
        .map(|s| parse_hex(s))
        .collect::<Result<Vec<_>>>()?;
    let two_color = values.len() == 2;
    let colors = if two_color {
        ramp(values[0].rgb565(), values[1].rgb565())
    } else {
        values
            .into_iter()
            .map(Rgb24::rgb565)
            .collect::<Vec<_>>()
            .try_into()
            .unwrap()
    };
    Ok(ParsedPalette {
        name,
        colors,
        two_color,
    })
}
pub fn parse_many(text: &str) -> Result<Vec<ParsedPalette>> {
    let mut result = Vec::new();
    for (line, text) in text.trim_start_matches('\u{feff}').lines().enumerate() {
        let text = text.trim();
        let comment = text
            .strip_prefix('#')
            .is_some_and(|s| s.is_empty() || s.starts_with(char::is_whitespace));
        if text.is_empty() || (comment && !text.contains('|')) {
            continue;
        }
        match parse(text) {
            Ok(palette) => result.push(palette),
            Err(_) if comment => continue,
            Err(error) => return Err(error).with_context(|| format!("Line {}", line + 1)),
        }
        ensure!(
            result.len() <= SLOT_COUNT,
            "A palette set can contain at most six palettes."
        );
    }
    ensure!(!result.is_empty(), "No palettes found.");
    Ok(result)
}
pub fn format(slot: &PaletteSlot) -> String {
    format!(
        "{}|{}",
        slot.name.as_str(),
        if is_ramp(slot.colors) {
            vec![slot.colors[0], slot.colors[3]]
        } else {
            slot.colors.to_vec()
        }
        .into_iter()
        .map(|c| format!("{:06X}", rgb888(c)))
        .collect::<Vec<_>>()
        .join("|")
    )
}
