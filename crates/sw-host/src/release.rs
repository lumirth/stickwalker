use crate::save::FLASH;
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{borrow::Cow, collections::BTreeMap, fs, path::Path};

pub use semver::Version;
pub const FEED: &str =
    "https://github.com/lumirth/stickwalker/releases/download/firmware-feed/firmware.json";
#[derive(Clone, Serialize, Deserialize)]
pub struct Image {
    pub kind: String,
    pub offset: usize,
    pub file: String,
    pub sha256: String,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct Region {
    pub offset: usize,
    pub size: usize,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct Manifest {
    pub schema: u32,
    pub product: String,
    pub version: String,
    pub storage: String,
    pub state: Region,
    pub settings: Region,
    pub images: Vec<Image>,
}
#[derive(Clone)]
pub struct Package {
    pub manifest: Manifest,
    images: BTreeMap<String, Cow<'static, [u8]>>,
}
pub fn hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
pub fn get(url: &str) -> Result<Vec<u8>> {
    ensure!(
        url.starts_with("https://"),
        "Release downloads require HTTPS"
    );
    Ok(ureq::get(url)
        .header("User-Agent", "Stickwalker Desktop")
        .call()?
        .body_mut()
        .with_config()
        .limit(32 << 20)
        .read_to_vec()?)
}
impl Package {
    pub fn open(path: &Path) -> Result<Self> {
        let manifest: Manifest = serde_json::from_slice(
            &fs::read(path).context("The firmware manifest couldn’t be opened.")?,
        )
        .context("Choose the firmware.json file from a Stickwalker release.")?;
        let root = path.parent().context("Package directory missing")?;
        let images = manifest
            .images
            .iter()
            .map(|image| {
                valid_filename(&image.file)?;
                Ok((
                    image.file.clone(),
                    Cow::Owned(fs::read(root.join(&image.file))?),
                ))
            })
            .collect::<Result<_>>()?;
        Self { manifest, images }.validate()
    }
    fn validate(self) -> Result<Self> {
        let p = self;
        ensure!(
            p.manifest.schema == 1 && p.manifest.product == "StickS3",
            "This firmware package was not built for Stickwalker."
        );
        semver::Version::parse(&p.manifest.version)?;
        let m = &p.manifest;
        let mut ranges = Vec::new();
        for region in [&m.state, &m.settings] {
            ensure!(
                region.size > 0
                    && region.offset.is_multiple_of(4096)
                    && region.size.is_multiple_of(4096)
                    && region
                        .offset
                        .checked_add(region.size)
                        .is_some_and(|end| end <= FLASH),
                "Invalid storage region"
            );
            ranges.push((region.offset, region.offset + region.size));
        }
        for image in &m.images {
            valid_filename(&image.file)?;
            let bytes = p.image(image)?;
            ensure!(
                hash(bytes) == image.sha256,
                "Firmware download is incomplete: {}",
                image.file
            );
            let end = image
                .offset
                .checked_add(bytes.len().next_multiple_of(4096))
                .context("Invalid image size")?;
            ensure!(
                !bytes.is_empty() && image.offset.is_multiple_of(4096) && end <= FLASH,
                "Invalid firmware image size"
            );
            ranges.push((image.offset, end));
            if image.kind == "application" {
                ensure!(
                    bytes.len() >= 208
                        && bytes[0] == 0xe9
                        && bytes[32..36] == 0xabcd5432u32.to_le_bytes(),
                    "Invalid ESP32 application image"
                );
                ensure!(
                    String::from_utf8_lossy(&bytes[48..80]).trim_end_matches('\0') == m.version,
                    "Firmware version does not match its package"
                );
            }
        }
        ensure!(
            m.images.iter().filter(|i| i.kind == "application").count() == 1,
            "Package needs one application image"
        );
        ranges.sort_unstable();
        ensure!(
            ranges.windows(2).all(|w| w[0].1 <= w[1].0),
            "Firmware package regions overlap"
        );
        Ok(p)
    }
    pub fn image(&self, image: &Image) -> Result<&[u8]> {
        self.images
            .get(&image.file)
            .map(|bytes| bytes.as_ref())
            .context("Firmware image missing")
    }
    pub fn bundled() -> Result<Self> {
        let files: &[(&str, &'static [u8])] = include!(concat!(env!("OUT_DIR"), "/firmware.rs"));
        let manifest = files
            .iter()
            .find(|(name, _)| *name == "firmware.json")
            .context("No bundled firmware in this development build")?
            .1;
        let images = files
            .iter()
            .filter(|(name, _)| *name != "firmware.json")
            .map(|(name, bytes)| (name.to_string(), Cow::Borrowed(*bytes)))
            .collect();
        Self {
            manifest: serde_json::from_slice(manifest)?,
            images,
        }
        .validate()
    }
    pub fn latest() -> Result<Self> {
        let bytes = get(FEED).context(
            "Could not check firmware updates. The bundled firmware is available offline.",
        )?;
        let manifest: Manifest = serde_json::from_slice(&bytes)?;
        Version::parse(&manifest.version)?;
        let images = manifest
            .images
            .iter()
            .map(|image| {
                valid_filename(&image.file)?;
                let url = format!(
                    "https://github.com/lumirth/stickwalker/releases/download/firmware-v{}/{}",
                    manifest.version, image.file
                );
                Ok((image.file.clone(), Cow::Owned(get(&url)?)))
            })
            .collect::<Result<_>>()?;
        Self { manifest, images }.validate()
    }
}

fn valid_filename(name: &str) -> Result<()> {
    ensure!(
        !name.is_empty() && !name.contains(['/', '\\']) && name != "..",
        "Invalid package filename"
    );
    Ok(())
}

pub fn partition(table: &[u8], name: &str) -> Option<(usize, usize)> {
    table
        .as_chunks::<32>()
        .0
        .iter()
        .take_while(|e| e[..2] == [0xaa, 0x50])
        .find_map(|e| {
            let label = &e[12..28];
            let length = label.iter().position(|b| *b == 0).unwrap_or(16);
            (label[..length] == *name.as_bytes()).then(|| {
                (
                    u32::from_le_bytes(e[4..8].try_into().unwrap()) as usize,
                    u32::from_le_bytes(e[8..12].try_into().unwrap()) as usize,
                )
            })
        })
}
