use crate::save::{FLASH, atomic_write, read_limited};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, fs, path::Path, sync::LazyLock, time::Duration};

pub use semver::Version;
pub const FEED: &str =
    "https://github.com/lumirth/stickwalker/releases/download/firmware-feed/firmware.json";
const INVALID_PACKAGE: &str = "This firmware package is invalid or damaged. Download it again.";

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

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ImageKind {
    Application,
    Bootloader,
    Partitions,
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Image {
    pub kind: ImageKind,
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
    pub version: Version,
    pub storage: String,
    pub state: Region,
    pub settings: Region,
    pub images: Vec<Image>,
}
#[derive(Clone)]
pub struct Package {
    pub manifest: Manifest,
    images: BTreeMap<String, Vec<u8>>,
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
        let manifest: Self = serde_json::from_slice(bytes).context(INVALID_PACKAGE)?;
        ensure!(
            manifest.schema == 1 && manifest.product == "StickS3",
            "This firmware package was not built for Stickwalker."
        );
        ensure!(
            manifest.images.len() == 3
                && [
                    ImageKind::Application,
                    ImageKind::Bootloader,
                    ImageKind::Partitions
                ]
                .into_iter()
                .all(|kind| manifest.images.iter().any(|i| i.kind == kind)),
            INVALID_PACKAGE
        );
        for (index, image) in manifest.images.iter().enumerate() {
            let name = &image.file.0;
            ensure!(
                !name.eq_ignore_ascii_case("firmware.json")
                    && !manifest.images[..index]
                        .iter()
                        .any(|i| i.file.0.eq_ignore_ascii_case(name)),
                INVALID_PACKAGE
            );
        }
        Ok(manifest)
    }
}

impl Package {
    pub fn open(path: &Path) -> Result<Self> {
        let manifest = Manifest::parse(
            &read_limited(path, MANIFEST_LIMIT).context("Couldn’t open firmware.json.")?,
        )?;
        let root = path.parent().context("Choose a firmware.json file.")?;
        Self::load(manifest, |name| {
            read_limited(&root.join(name), FLASH)
                .with_context(|| format!("Couldn’t open {name}. Keep the firmware files together."))
        })
    }

    fn load(manifest: Manifest, mut read: impl FnMut(&str) -> Result<Vec<u8>>) -> Result<Self> {
        let images = manifest
            .images
            .iter()
            .map(|image| Ok((image.file.0.clone(), read(&image.file.0)?)))
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
                INVALID_PACKAGE
            );
            ranges.push((region.offset, region.offset + region.size));
        }
        for image in &m.images {
            let bytes = p.image(image)?;
            ensure!(
                hash(bytes).eq_ignore_ascii_case(&image.sha256),
                "Firmware file {} is incomplete or damaged.",
                image.file.0
            );
            let end = image
                .offset
                .checked_add(bytes.len().next_multiple_of(4096))
                .context(INVALID_PACKAGE)?;
            ensure!(
                !bytes.is_empty() && image.offset.is_multiple_of(4096) && end <= FLASH,
                INVALID_PACKAGE
            );
            ranges.push((image.offset, end));
            if image.kind == ImageKind::Application {
                ensure!(
                    bytes.len() >= 208
                        && bytes[0] == 0xe9
                        && bytes[32..36] == 0xabcd5432u32.to_le_bytes(),
                    INVALID_PACKAGE
                );
                ensure!(
                    String::from_utf8_lossy(&bytes[48..80]).trim_end_matches('\0')
                        == m.version.to_string(),
                    INVALID_PACKAGE
                );
            }
        }
        let table = m
            .images
            .iter()
            .find(|i| i.kind == ImageKind::Partitions)
            .context(INVALID_PACKAGE)?;
        let table = p.image(table)?;
        let app = m
            .images
            .iter()
            .find(|i| i.kind == ImageKind::Application)
            .context(INVALID_PACKAGE)?;
        let app_size = p.image(app)?.len();
        ensure!(
            partition(table, "nvs") == Some((m.settings.offset, m.settings.size))
                && partition(table, "walker2") == Some((m.state.offset, m.state.size))
                && partition(table, "factory")
                    .is_some_and(|(at, capacity)| at == app.offset && app_size <= capacity),
            INVALID_PACKAGE
        );
        ranges.sort_unstable();
        ensure!(ranges.windows(2).all(|w| w[0].1 <= w[1].0), INVALID_PACKAGE);
        Ok(p)
    }
    pub fn build(&self) -> String {
        let app = self
            .manifest
            .images
            .iter()
            .find(|i| i.kind == ImageKind::Application)
            .expect("validated package has an application");
        self.images[&app.file.0][176..208]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect()
    }
    pub fn image(&self, image: &Image) -> Result<&[u8]> {
        self.images
            .get(&image.file.0)
            .map(Vec::as_slice)
            .context(INVALID_PACKAGE)
    }
    /// A version becomes available offline once its verified manifest is in place.
    pub(super) fn store(&self, cache: &Path) -> Result<()> {
        let dir = cache.join(self.manifest.version.to_string());
        for (name, bytes) in &self.images {
            atomic_write(&dir.join(name), bytes)?;
        }
        atomic_write(
            &dir.join("firmware.json"),
            &serde_json::to_vec(&self.manifest)?,
        )
    }

    pub(super) fn cached(cache: &Path) -> Option<Self> {
        let mut versions = fs::read_dir(cache)
            .ok()?
            .filter_map(|entry| {
                let entry = entry.ok()?;
                let version = Version::parse(entry.file_name().to_str()?).ok()?;
                Some((version, entry.path()))
            })
            .collect::<Vec<_>>();
        versions.sort_unstable_by(|a, b| b.0.cmp(&a.0));
        versions.into_iter().find_map(|(version, dir)| {
            Self::open(&dir.join("firmware.json"))
                .ok()
                .filter(|p| p.manifest.version == version)
        })
    }

    pub(super) fn latest(current: Option<&Version>) -> Result<Option<Self>> {
        Self::fetch(current, get)
    }

    pub(super) fn fetch(
        current: Option<&Version>,
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<Option<Self>> {
        let manifest = Manifest::parse(&read(FEED, MANIFEST_LIMIT)?)?;
        if current.is_some_and(|version| !manifest.version.cmp_precedence(version).is_gt()) {
            return Ok(None);
        }

        let url = format!(
            "https://github.com/lumirth/stickwalker/releases/download/firmware-v{}",
            manifest.version
        );
        Self::load(manifest, |name| read(&format!("{url}/{name}"), FLASH)).map(Some)
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
