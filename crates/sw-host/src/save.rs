use anyhow::{Context, Result, bail, ensure};
use std::{
    fs,
    io::{Read, Write},
    path::Path,
};

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

/// Bound both allocation and the actual read if the file grows after opening.
pub fn read_limited(path: &Path, limit: usize) -> Result<Vec<u8>> {
    ensure!(fs::metadata(path)?.is_file(), "Choose a file.");
    let file = fs::File::open(path)?;
    let meta = file.metadata()?;
    ensure!(
        meta.is_file() && meta.len() <= limit as u64,
        "The selected file is too large."
    );
    let mut bytes = Vec::with_capacity(meta.len() as usize);
    file.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= limit, "The selected file is too large.");
    Ok(bytes)
}

#[derive(Debug)]
pub struct EepromImage(Box<[u8; EEPROM]>);

impl EepromImage {
    pub fn load(path: &Path) -> Result<Self> {
        Self::parse(read_limited(path, EEPROM)?)
    }

    pub fn parse(bytes: Vec<u8>) -> Result<Self> {
        validate_eeprom(&bytes)?;
        Ok(Self(
            bytes
                .into_boxed_slice()
                .try_into()
                .expect("validated EEPROM size"),
        ))
    }

    pub fn bytes(&self) -> &[u8] {
        self.0.as_slice()
    }
}
pub fn validate_eeprom(bytes: &[u8]) -> Result<()> {
    ensure!(
        bytes.len() == EEPROM && &bytes[..8] == b"nintendo",
        "Choose a valid Pokéwalker save."
    );
    Ok(())
}
pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent)?;
    let mut temp = tempfile::NamedTempFile::new_in(parent)?;
    temp.write_all(bytes)?;
    temp.as_file().sync_all()?;
    #[cfg(windows)]
    for delay_ms in [25, 75] {
        match temp.persist(path) {
            Ok(_) => return Ok(()),
            Err(e) if e.error.kind() == std::io::ErrorKind::PermissionDenied => {
                temp = e.file;
                std::thread::sleep(std::time::Duration::from_millis(delay_ms));
            }
            Err(e) => return Err(e.into()),
        }
    }
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
    StickwalkerSave(EepromImage),
    EmptyOrForeign,
    DamagedSave(anyhow::Error),
}
pub fn classify_public_partition(data: &[u8]) -> PublicPartition {
    let raw = [0, 0x11000].into_iter().any(|at| {
        data.get(at..at + 4) == Some(&0x53574531u32.to_le_bytes())
            || data.get(at + 24..at + 28) == Some(&0x54494d43u32.to_le_bytes())
    }) || data.get(0x22000..0x22004) == Some(&0x53574a31u32.to_le_bytes());
    if !raw {
        return crate::legacy::classify_littlefs(data);
    }
    match decode_public(data).and_then(EepromImage::parse) {
        Ok(image) => PublicPartition::StickwalkerSave(image),
        Err(e) => PublicPartition::DamagedSave(e),
    }
}
