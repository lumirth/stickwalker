use crate::save::FLASH;
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{borrow::Cow, collections::BTreeMap, path::Path, sync::LazyLock, time::Duration};

pub use semver::Version;
pub const FEED: &str =
    "https://github.com/lumirth/stickwalker/releases/download/firmware-feed/firmware.json";
#[derive(Clone, Serialize, Deserialize)]
#[serde(try_from = "String")]
pub struct AssetName(String);

impl TryFrom<String> for AssetName {
    type Error = &'static str;
    fn try_from(name: String) -> std::result::Result<Self, Self::Error> {
        let stem = name
            .split('.')
            .next()
            .unwrap_or_default()
            .to_ascii_uppercase();
        let reserved = matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
            || ((stem.starts_with("COM") || stem.starts_with("LPT"))
                && stem.len() == 4
                && stem.as_bytes()[3].is_ascii_digit());
        if name.is_empty()
            || name.ends_with('.')
            || reserved
            || !name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
        {
            return Err("Invalid firmware asset name");
        }
        Ok(Self(name))
    }
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Image {
    pub kind: String,
    pub offset: usize,
    pub file: AssetName,
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
const MANIFEST_LIMIT: usize = 64 * 1024;
static HTTP: LazyLock<ureq::Agent> = LazyLock::new(|| {
    ureq::Agent::config_builder()
        .https_only(true)
        .timeout_connect(Some(Duration::from_secs(10)))
        .timeout_global(Some(Duration::from_secs(60)))
        .build()
        .new_agent()
});

fn get(url: &str, limit: usize) -> Result<Vec<u8>> {
    Ok(HTTP
        .get(url)
        .header("User-Agent", "Stickwalker Desktop")
        .call()?
        .body_mut()
        .with_config()
        .limit(limit as u64)
        .read_to_vec()?)
}

impl Manifest {
    fn parse(bytes: &[u8]) -> Result<Self> {
        let manifest: Self = serde_json::from_slice(bytes)
            .context("Choose the firmware.json file from a Stickwalker release.")?;
        ensure!(
            manifest.schema == 1 && manifest.product == "StickS3",
            "This firmware package was not built for Stickwalker."
        );
        Version::parse(&manifest.version)?;
        ensure!(
            manifest.images.len() == 3
                && ["application", "bootloader", "partitions"]
                    .iter()
                    .all(|kind| manifest.images.iter().any(|i| i.kind == *kind)),
            "Missing firmware components"
        );
        Ok(manifest)
    }
}
impl Package {
    pub fn open(path: &Path) -> Result<Self> {
        let manifest = Manifest::parse(
            &crate::save::read_limited(path, MANIFEST_LIMIT)
                .context("The firmware manifest couldn’t be opened.")?,
        )?;
        let root = path.parent().context("Package directory missing")?;
        let images = manifest
            .images
            .iter()
            .map(|image| {
                Ok((
                    image.file.0.clone(),
                    Cow::Owned(crate::save::read_limited(&root.join(&image.file.0), FLASH)?),
                ))
            })
            .collect::<Result<_>>()?;
        Self { manifest, images }.validate()
    }
    fn validate(self) -> Result<Self> {
        let p = self;
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
            let bytes = p.image(image)?;
            ensure!(
                hash(bytes).eq_ignore_ascii_case(&image.sha256),
                "Firmware download is incomplete: {}",
                image.file.0
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
        let table = m.images.iter().find(|i| i.kind == "partitions").unwrap();
        let table = p.image(table)?;
        let app = m.images.iter().find(|i| i.kind == "application").unwrap();
        let app_size = p.image(app)?.len();
        ensure!(
            partition(table, "nvs") == Some((m.settings.offset, m.settings.size))
                && partition(table, "walker2") == Some((m.state.offset, m.state.size))
                && partition(table, "factory")
                    .is_some_and(|(at, capacity)| at == app.offset && app_size <= capacity),
            "Firmware images do not match the packaged partition table"
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
            .get(&image.file.0)
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
            manifest: Manifest::parse(manifest)?,
            images,
        }
        .validate()
    }
    pub fn latest() -> Result<Self> {
        let bytes = get(FEED, MANIFEST_LIMIT).context(
            "Could not check firmware updates. The bundled firmware is available offline.",
        )?;
        let manifest = Manifest::parse(&bytes)?;
        let images = manifest
            .images
            .iter()
            .map(|image| {
                let url = format!(
                    "https://github.com/lumirth/stickwalker/releases/download/firmware-v{}/{}",
                    manifest.version, image.file.0
                );
                Ok((image.file.0.clone(), Cow::Owned(get(&url, FLASH)?)))
            })
            .collect::<Result<_>>()?;
        Self { manifest, images }.validate()
    }
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

#[cfg(test)]
mod tests {
    use super::AssetName;
    #[test]
    fn asset_names_are_single_portable_url_segments() {
        for name in [
            "C:foo",
            "../bin",
            "sub\\x",
            "x?file",
            "x#fragment",
            "CON.bin",
            ".",
            "..",
            "image.",
            "%2f",
        ] {
            assert!(AssetName::try_from(name.to_owned()).is_err(), "{name}");
        }
        assert!(AssetName::try_from("partition-table_1.bin".to_owned()).is_ok());
    }
}
