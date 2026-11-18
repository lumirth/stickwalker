//! Readers for the five public candidate releases. No private r1–r6 formats.
use crate::save::{EEPROM, apply_spans, validate_eeprom};
use anyhow::{Context, Result, ensure};
use littlefs2::{
    consts::{U16, U512},
    driver::Storage,
    fs::Filesystem,
    io::{Error, Result as FsResult},
    path::Path,
};
struct Flash<'a>(&'a [u8]);
impl Storage for Flash<'_> {
    type CACHE_SIZE = U512;
    type LOOKAHEAD_SIZE = U16;
    const READ_SIZE: usize = 16;
    const WRITE_SIZE: usize = 256;
    const BLOCK_SIZE: usize = 4096;
    const BLOCK_COUNT: usize = 384;
    fn read(&mut self, at: usize, out: &mut [u8]) -> FsResult<usize> {
        let bytes = self.0.get(at..at + out.len()).ok_or(Error::IO)?;
        out.copy_from_slice(bytes);
        Ok(out.len())
    }
    fn write(&mut self, _: usize, _: &[u8]) -> FsResult<usize> {
        Err(Error::IO)
    }
    fn erase(&mut self, _: usize, _: usize) -> FsResult<usize> {
        Err(Error::IO)
    }
}
fn word(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
pub fn littlefs(data: &[u8]) -> Result<Option<Vec<u8>>> {
    ensure!(data.len() >= 0x180000, "Incomplete legacy filesystem");
    let mut flash = Flash(&data[..0x180000]);
    let mut allocation = Filesystem::allocate();
    let Ok(fs) = Filesystem::mount(&mut allocation, &mut flash) else {
        return Ok(None);
    };
    if !["/pw-a.bin\0", "/pw-b.bin\0", "/pw-journal.bin\0"]
        .iter()
        .any(|name| fs.exists(Path::from_str_with_nul(name).unwrap()))
    {
        return Ok(None);
    }
    let read = |name: &str| -> Result<Vec<u8>> {
        let path = Path::from_str_with_nul(name)
            .map_err(|_| anyhow::anyhow!("Invalid legacy filename"))?;
        let length = fs
            .metadata(path)
            .map_err(|e| anyhow::anyhow!("{name}: {e:?}"))?
            .len();
        ensure!(length <= EEPROM + 4096, "Oversized legacy save file");
        let mut bytes = vec![0; length];
        fs.open_file_and_then(path, |f| {
            let mut at = 0;
            while at < bytes.len() {
                let n = f.read(&mut bytes[at..])?;
                if n == 0 {
                    return Err(Error::IO);
                }
                at += n;
            }
            Ok(())
        })
        .map_err(|e| anyhow::anyhow!("Cannot read legacy save: {e:?}"))?;
        Ok(bytes)
    };
    let mut slots = Vec::new();
    for name in ["/pw-a.bin\0", "/pw-b.bin\0"] {
        if fs.exists(Path::from_str_with_nul(name).unwrap()) {
            let b = read(name)?;
            ensure!(
                b.len() == EEPROM + 12
                    && word(&b, 0) == 0x50574531
                    && word(&b, 8) == crc32fast::hash(&b[12..]),
                "Damaged legacy checkpoint"
            );
            slots.push((word(&b, 4), b[12..].to_vec()));
        }
    }
    let (_, mut image) = slots
        .into_iter()
        .max_by_key(|v| v.0)
        .context("No committed save in the older filesystem")?;
    let journal_path = Path::from_str_with_nul("/pw-journal.bin\0").unwrap();
    if fs.exists(journal_path) {
        let b = read("/pw-journal.bin\0")?;
        let mut at = 0;
        while at + 12 <= b.len() {
            let size = word(&b, at + 4) as usize;
            ensure!(
                word(&b, at) == 0x50574a31 && size <= 4096,
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
    validate_eeprom(&image)?;
    Ok(Some(image))
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
    let entries = entries(nvs);
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
        active_palette: 2,
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
