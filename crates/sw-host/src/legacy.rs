//! Readers for the five public candidate releases. No private r1–r6 formats.
use crate::save::{EEPROM, EepromImage, PublicPartition, apply_spans, validate_eeprom};
use anyhow::{Context, Result, ensure};
mod filesystem;
const CHECKPOINT_MAGIC: u32 = 0x50574531;
const JOURNAL_MAGIC: u32 = 0x50574a31;

fn word(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
pub(super) fn classify_littlefs(data: &[u8]) -> PublicPartition {
    match scan_littlefs(data) {
        Ok(outcome) => outcome,
        Err(error) => PublicPartition::DamagedSave(error),
    }
}

fn scan_littlefs(data: &[u8]) -> Result<PublicPartition> {
    ensure!(data.len() == 0x180000, "Incomplete legacy filesystem");
    if let Some(files) = filesystem::mounted(data, scan_files) {
        if let Some(image) = files? {
            return Ok(PublicPartition::StickwalkerSave(image));
        }
    } else {
        let metadata: &[&[u8]] = &[b"littlefs", b"pw-a.bin", b"pw-b.bin", b"pw-journal.bin"];
        ensure!(
            !metadata
                .iter()
                .any(|m| data.windows(m.len()).any(|w| w == *m)),
            "The older filesystem could not be opened"
        );
    }
    // LittleFS can mount an older, empty directory revision after a metadata tear.
    ensure!(
        !data
            .windows(4)
            .any(|bytes| bytes == CHECKPOINT_MAGIC.to_le_bytes()
                || bytes == JOURNAL_MAGIC.to_le_bytes()),
        "The older filesystem contains an unreadable save"
    );
    Ok(PublicPartition::EmptyOrForeign)
}

fn scan_files(fs: &mut filesystem::Volume<'_>) -> Result<Option<EepromImage>> {
    let mut newest = None;
    let mut damaged_sequence = None;
    let mut found = false;
    for name in [c"/pw-a.bin", c"/pw-b.bin"] {
        let Some(bytes) = fs.read(name, 0x180000)? else {
            continue;
        };
        found = true;
        // A torn header has no sequence. Use the intact partner in that case.
        if bytes.len() < 8 {
            continue;
        }
        ensure!(
            word(&bytes, 0) == CHECKPOINT_MAGIC,
            "The older save has a damaged checkpoint header"
        );
        let sequence = word(&bytes, 4);
        if bytes.len() == EEPROM + 12
            && word(&bytes, 8) == crc32fast::hash(&bytes[12..])
            && validate_eeprom(&bytes[12..]).is_ok()
        {
            if newest.as_ref().is_none_or(|(n, _)| sequence > *n) {
                newest = Some((sequence, bytes[12..].to_vec()));
            }
        } else {
            damaged_sequence = damaged_sequence.max(Some(sequence));
        }
    }
    let journal = fs.read(c"/pw-journal.bin", 0x180000)?;
    if !found && journal.is_none() {
        return Ok(None);
    }
    let (sequence, mut image) = newest.context("No intact checkpoint in the older filesystem")?;
    ensure!(
        damaged_sequence.is_none_or(|n| n <= sequence),
        "The newer legacy checkpoint is damaged"
    );
    if let Some(b) = journal {
        let mut at = 0;
        while at + 12 <= b.len() {
            let size = word(&b, at + 4) as usize;
            ensure!(
                word(&b, at) == JOURNAL_MAGIC && size <= 4096,
                "Invalid legacy save journal"
            );
            if at + 12 + size > b.len() {
                break;
            }
            let body = &b[at + 12..at + 12 + size];
            ensure!(
                word(&b, at + 8) == crc32fast::hash(body),
                "Damaged legacy save journal"
            );
            apply_spans(&mut image, body, false)?;
            at += 12 + size;
        }
    }
    Ok(Some(EepromImage::parse(image)?))
}

// Only the U8 preferences used by public firmware are interpreted.
fn entries(nvs: &[u8]) -> Vec<(u8, String, u8)> {
    let mut pages: Vec<_> = nvs
        .as_chunks::<4096>()
        .0
        .iter()
        .map(|p| &p[..])
        .filter(|p| matches!(word(p, 0), 0xfffffffe | 0xfffffffc))
        .collect();
    pages.sort_by_key(|p| word(p, 4));
    let mut entries = Vec::new();
    for page in pages {
        for ix in 0..126 {
            if (page[32 + ix / 4] >> ((ix % 4) * 2)) & 3 != 2 {
                continue;
            }
            let e = &page[64 + ix * 32..96 + ix * 32];
            if e[1] != 1 || e[2] != 1 {
                continue;
            }
            let mut h = crc32fast::Hasher::new_with_initial(0xffffffff);
            h.update(&e[..4]);
            h.update(&e[8..]);
            if h.finalize() != word(e, 4) {
                continue;
            }
            let name = String::from_utf8_lossy(&e[8..24])
                .trim_end_matches('\0')
                .to_owned();
            entries.push((e[0], name, e[24]));
        }
    }
    entries
}
pub fn preferences(nvs: &[u8]) -> crate::DeviceSettings {
    translate(&entries(nvs))
}

pub(super) fn migrated_preferences(nvs: &[u8]) -> Option<crate::DeviceSettings> {
    let entries = entries(nvs);
    let has_namespace = |name: &str| entries.iter().any(|(ns, key, _)| *ns == 0 && key == name);
    (!has_namespace("stick") && (has_namespace("pw-controls") || has_namespace("pw-display")))
        .then(|| translate(&entries))
}

fn translate(entries: &[(u8, String, u8)]) -> crate::DeviceSettings {
    let value = |namespace: &str, key: &str| -> Option<u8> {
        let id = entries
            .iter()
            .rev()
            .find(|(ns, name, _)| *ns == 0 && name == namespace)?
            .2;
        entries
            .iter()
            .rev()
            .find(|(ns, name, _)| *ns == id && name == key)
            .map(|e| e.2)
    };
    let control = value("pw-controls", "layout-v2").unwrap_or(6);
    let layout = control & 3;
    let reverse = 1 - ((control >> 2) & 1);
    let mut prefs = crate::DeviceSettings {
        active_palette: crate::PaletteIndex::GREYSCALE,
        reverse: reverse != 0,
        layout: if layout < 2 { 2 } else { 1 },
        mappings: [
            1,
            if layout == 3 { 5 } else { 0 },
            if layout == 1 { 3 } else { 1 },
            1,
        ],
        ..Default::default()
    };
    let dark = value("pw-display", "dark").unwrap_or(1) != 0;
    for (i, gray) in [0u16, 10, 21, 31].into_iter().enumerate() {
        let color = (gray << 11) | (((gray * 63 + 15) / 31) << 5) | gray;
        let index = if dark { i } else { 3 - i };
        prefs.slots[2].colors[index] = color;
    }
    prefs
}
