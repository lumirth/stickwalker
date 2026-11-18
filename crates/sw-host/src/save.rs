use anyhow::{Context, Result, bail, ensure};
use std::{fs, io::Write, path::Path};

pub const EEPROM: usize = 65536;
pub const WALKER: usize = 0x610000;
pub const FLASH: usize = 8 << 20;
const BANK: usize = 0xe0000;
fn u32le(p: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(p[i..i + 4].try_into().unwrap())
}
fn crc(a: &[u8], b: &[u8]) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(a);
    h.update(b);
    h.finalize()
}

pub fn read_eeprom(path: &Path) -> Result<Vec<u8>> {
    let bytes = fs::read(path)?;
    validate_eeprom(&bytes)?;
    Ok(bytes)
}
pub fn validate_eeprom(bytes: &[u8]) -> Result<()> {
    ensure!(
        bytes.len() == EEPROM && &bytes[..8] == b"nintendo",
        "Choose a valid Pokéwalker save (.eeprom)."
    );
    Ok(())
}
pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path.parent().context("File has no parent directory")?;
    fs::create_dir_all(parent)?;
    let mut temp = tempfile::NamedTempFile::new_in(parent)?;
    temp.write_all(bytes)?;
    temp.as_file().sync_all()?;
    temp.persist(path)?;
    Ok(())
}
/// Offline diagnostic reader used by the simulator and CLI; updates do not decode current storage.
pub fn decode_current(data: &[u8]) -> Result<(Vec<u8>, Vec<u8>)> {
    let mut banks = Vec::new();
    for base in [0x10000, 0xf0000] {
        if data.len() < base + 0x11000 {
            continue;
        }
        let h = &data[base..base + 72];
        let image = &data[base + 4096..base + 0x11000];
        if &h[68..72] == b"SWK6" && crc(&h[..64], image) == u32le(h, 64) {
            banks.push((u32le(h, 0), base, image.to_vec(), h[8..64].to_vec()));
        }
    }
    let Some((_, base, mut image, mut runtime)) = banks.into_iter().max_by_key(|b| b.0) else {
        let header = [0x10000, 0xf0000, 0x11000, 0xf1000].iter().any(|&b| {
            data.get(b + 68..b + 72)
                .is_some_and(|m| &m[..3] == b"SWK" && (b'0'..=b'6').contains(&m[3]))
        });
        if !header && data.len() >= EEPROM && &data[..8] == b"nintendo" {
            return Ok((data[..EEPROM].to_vec(), vec![0; 56]));
        }
        bail!("No readable committed Stickwalker save.");
    };
    let mut at = 0x11000;
    while at + 72 <= BANK && base + at + 72 <= data.len() {
        let h = &data[base + at..base + at + 72];
        let size = u32le(h, 4) as usize;
        if &h[..4] != b"SWR6"
            || size > EEPROM + 12
            || at + 72 + size > BANK
            || base + at + 72 + size > data.len()
        {
            break;
        }
        let body = &data[base + at + 72..base + at + 72 + size];
        if crc(&h[..64], body) != u32le(h, 64) {
            break;
        }
        let mut next = image.clone();
        if apply_spans(&mut next, body, true).is_err() {
            break;
        }
        image = next;
        runtime = h[8..64].to_vec();
        at += 72 + size;
    }
    Ok((image, runtime))
}
pub(crate) fn apply_spans(image: &mut [u8], mut body: &[u8], zero_runs: bool) -> Result<()> {
    while !body.is_empty() {
        ensure!(body.len() >= 4, "Truncated save journal");
        let addr = u16::from_le_bytes([body[0], body[1]]) as usize;
        let encoded = u16::from_le_bytes([body[2], body[3]]);
        body = &body[4..];
        let zero = zero_runs && encoded & 0x8000 != 0;
        let count = if zero_runs { encoded & 0x7fff } else { encoded } as usize;
        ensure!(
            count > 0 && addr + count <= EEPROM,
            "Invalid save journal address"
        );
        if zero {
            image[addr..addr + count].fill(0);
        } else {
            ensure!(body.len() >= count, "Truncated save journal span");
            image[addr..addr + count].copy_from_slice(&body[..count]);
            body = &body[count..];
        }
    }
    Ok(())
}
// Public lumirth/stickwalker SWE1 checkpoints and SWJ1 byte-span journal.
pub fn decode_public(data: &[u8]) -> Result<Vec<u8>> {
    fn header(data: &[u8], at: usize, magic: u32) -> Option<(u32, u32, usize)> {
        let h = data.get(at..at + 28)?;
        let size = u32le(h, 12) as usize;
        let body = data.get(at + 28..at + 28 + size)?;
        (u32le(h, 0) == magic
            && u32le(h, 4) > 0
            && u32le(h, 8) > 0
            && u32le(h, 24) == 0x54494d43
            && crc32fast::hash(&h[..20]) == u32le(h, 20)
            && crc32fast::hash(body) == u32le(h, 16))
        .then_some((u32le(h, 4), u32le(h, 8), size))
    }
    for base in [0, 0x11000] {
        if data.get(base + 24..base + 28) == Some(&0x54494d43u32.to_le_bytes()) {
            ensure!(
                header(data, base, 0x53574531).is_some_and(|h| h.2 == EEPROM),
                "Damaged committed public checkpoint"
            );
        }
    }
    let (generation, mut sequence, base) = [0, 0x11000]
        .into_iter()
        .filter_map(|base| {
            header(data, base, 0x53574531)
                .filter(|h| h.2 == EEPROM)
                .map(|(g, s, _)| (g, s, base))
        })
        .max()
        .context("No committed public Stickwalker save")?;
    let mut image = data[base + 28..base + 28 + EEPROM].to_vec();
    let mut at = 0x22000;
    while at + 28 <= data.len().min(0x62000) {
        if u32le(data, at + 24) != 0x54494d43 {
            break;
        }
        let (g, s, size) = header(data, at, 0x53574a31).context("Corrupt public save journal")?;
        ensure!(
            size > 0 && size <= 4096 && g <= generation,
            "Invalid public save journal"
        );
        if s > sequence {
            ensure!(
                g == generation && s == sequence + 1,
                "Missing public save journal record"
            );
            apply_spans(&mut image, &data[at + 28..at + 28 + size], false)?;
            sequence = s;
        }
        at += (28 + size + 255) & !255;
    }
    validate_eeprom(&image)?;
    Ok(image)
}

#[derive(Debug)]
pub enum PublicPartition {
    StickwalkerSave(Vec<u8>),
    EmptyOrForeign,
    DamagedSave(anyhow::Error),
}
pub fn classify_public_partition(data: &[u8]) -> PublicPartition {
    let raw = [0, 0x11000].into_iter().any(|at| {
        data.get(at..at + 4) == Some(&0x53574531u32.to_le_bytes())
            || data.get(at + 24..at + 28) == Some(&0x54494d43u32.to_le_bytes())
    }) || data.get(0x22000..0x22004) == Some(&0x53574a31u32.to_le_bytes());
    let result = if raw {
        decode_public(data).map(Some)
    } else {
        crate::legacy::littlefs(data)
    };
    match result {
        Ok(Some(image)) => PublicPartition::StickwalkerSave(image),
        Ok(None) => PublicPartition::EmptyOrForeign,
        Err(error) => PublicPartition::DamagedSave(error),
    }
}
