use crate::{
    protocol::{self, Session},
    release::{ImageKind, Package},
    save,
    usb::{DeviceMode, Info, Port, mac_id, ports},
};
use anyhow::{Context, Result, ensure};
use espflash::{
    connection::{Connection, ResetAfterOperation, ResetBeforeOperation},
    flasher::Flasher,
    target::{Chip, ProgressCallbacks},
};
use serialport::UsbPortInfo;
use std::{
    fs, thread,
    time::{Duration, Instant},
};

pub fn connect_rom(port: &Port) -> Result<Flasher> {
    let until = Instant::now() + Duration::from_secs(2);
    let serial = loop {
        match serialport::new(&port.path, 115200)
            .timeout(Duration::from_secs(3))
            .open_native()
        {
            Ok(port) => break port,
            Err(_) if Instant::now() < until => thread::sleep(Duration::from_millis(150)),
            Err(error) => return Err(error.into()),
        }
    };
    let info = UsbPortInfo {
        vid: 0x303a,
        pid: 0x1001,
        serial_number: Some(port.serial.clone()),
        manufacturer: None,
        product: None,
    };
    let connection = Connection::new(
        serial,
        info,
        ResetAfterOperation::NoResetNoStub,
        ResetBeforeOperation::DefaultReset,
        115200,
    );
    Ok(Flasher::connect(
        connection,
        true,
        true,
        true,
        Some(Chip::Esp32s3),
        Some(921600),
    )?)
}
struct Progress<'a> {
    report: &'a mut dyn FnMut(&str, Option<u8>),
    units: usize,
    region: usize,
    previous: usize,
    total: usize,
    label: &'static str,
}
impl ProgressCallbacks for Progress<'_> {
    fn init(&mut self, _addr: u32, total: usize) {
        self.units = total;
        self.update(0);
    }
    fn update(&mut self, current: usize) {
        let done =
            self.previous + self.region.saturating_mul(current.min(self.units)) / self.units.max(1);
        (self.report)(self.label, Some((done * 100 / self.total.max(1)) as u8));
    }
    fn verifying(&mut self) {}
    fn finish(&mut self, _skipped: bool) {
        self.units = 1;
        self.update(1);
    }
}
fn read_region(flasher: &mut Flasher, offset: usize, size: usize) -> Result<Vec<u8>> {
    let temp = tempfile::NamedTempFile::new()?.into_temp_path();
    flasher.read_flash(offset as u32, size as u32, 4096, 64, temp.to_path_buf())?;
    let bytes = fs::read(&temp)?;
    ensure!(bytes.len() == size, "Incomplete device read");
    Ok(bytes)
}

// USB descriptors can be missing after a reset. Only STATUS from the chip
// that was flashed authorizes post-install settings on the returned session.
fn find_restarted<T>(
    candidates: Vec<Port>,
    chip_id: &str,
    mut status: impl FnMut(&str) -> Option<(T, Info)>,
) -> Option<(Port, T, Info)> {
    for port in candidates {
        if mac_id(&port.serial).is_some_and(|id| id != chip_id) {
            continue;
        }
        if let Some((session, info)) = status(&port.path)
            && mac_id(&info.mac).as_deref() == Some(chip_id)
        {
            return Some((port, session, info));
        }
    }
    None
}

fn app_fits(table: &[u8], offset: usize, size: usize, settings: (usize, usize)) -> bool {
    crate::release::partition(table, "factory")
        .is_some_and(|(at, capacity)| at == offset && size <= capacity)
        && crate::release::partition(table, "nvs") == Some(settings)
}

fn write_intake(
    image: &save::EepromImage,
    staged: Option<&[u8]>,
    mut write: impl FnMut(usize, &[u8]) -> Result<()>,
) -> Result<()> {
    if staged == Some(image.bytes()) {
        return Ok(());
    }
    // The first sector contains the marker the firmware recognizes on boot.
    let (first, rest) = image.bytes().split_at(4096);
    write(save::WALKER + first.len(), rest)?;
    write(save::WALKER, first)
}

/// The partition table is written last. A retry can still find the legacy
/// source, or the completed EEPROM intake if legacy bank sectors were retired.
pub fn install(
    port: &Port,
    session: Option<Session>,
    package: &Package,
    fresh: bool,
    progress: &mut dyn FnMut(&str, Option<u8>),
) -> Result<(Port, Session, Info)> {
    let manifest = &package.manifest;
    let images = manifest
        .images
        .iter()
        .map(|image| Ok((image, package.image(image)?)))
        .collect::<Result<Vec<_>>>()?;
    let (app, app_bytes) = images
        .iter()
        .find(|(image, _)| image.kind == ImageKind::Application)
        .unwrap();
    let build: String = app_bytes[176..208]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let mut expected_mac = crate::usb::mac_id(&port.serial);
    let mut managed = false;
    if let Some(mut session) = session {
        let info = Info::read(&mut session)?;
        let connected_mac =
            crate::usb::mac_id(&info.mac).context("Could not identify this Stick")?;
        ensure!(
            expected_mac.as_ref().is_none_or(|id| id == &connected_mac),
            "The selected Stick changed. Select it again."
        );
        expected_mac = Some(connected_mac);
        ensure!(
            info.mode != DeviceMode::Linking,
            "Finish the IR link on your Stick first."
        );
        if !fresh && info.mode == DeviceMode::Ready {
            session.request(protocol::PREPARE, &[])?;
            managed = true;
        }
        drop(session);
        thread::sleep(Duration::from_millis(200));
    }
    progress("Preparing Stickwalker", None);
    let mut flasher = connect_rom(port).context(
        "Hold the Stick’s Power button until its green light blinks, release it, then try again.",
    )?;
    let actual = flasher.device_info()?;
    ensure!(
        actual.flash_size.size() == save::FLASH as u32,
        "This device isn’t a supported M5Stack StickS3."
    );
    let chip_id = actual
        .mac_address
        .as_deref()
        .and_then(mac_id)
        .context("Cannot identify this Stick")?;
    ensure!(
        expected_mac.is_none_or(|id| id == chip_id),
        "The connected Stick changed. Select it again before installing."
    );
    let table = read_region(&mut flasher, 0x8000, 4096)?;
    let current = crate::release::partition(&table, "walker2") == Some((save::WALKER, 0x1f0000));
    let public = crate::release::partition(&table, "spiffs") == Some((0x670000, 0x180000));
    let (native, raw_intake) = if fresh || current {
        (false, false)
    } else {
        let a = read_region(&mut flasher, save::WALKER + 0x10000, 72)?;
        let b = read_region(&mut flasher, save::WALKER + 0xf0000, 72)?;
        let native = a[68..72] == *b"SWK6" || b[68..72] == *b"SWK6";
        let raw_intake =
            !native && read_region(&mut flasher, save::WALKER, 8)?.starts_with(b"nintendo");
        (native, raw_intake)
    };
    let image_size: usize = images.iter().map(|(_, b)| b.len()).sum();
    let mut saved = 0;
    let mut writes = Vec::new();
    let mut migrated = None;
    let check_storage = || -> Result<()> {
        ensure!(
            manifest.storage == "SWK6"
                && manifest.state.offset == save::WALKER
                && manifest.state.size == 0x1f0000
                && app.offset == 0x10000
                && manifest.settings.offset == 0x9000
                && manifest.settings.size == 0x6000,
            "This firmware package cannot use the installed storage."
        );
        Ok(())
    };
    if fresh {
        flasher.erase_region(
            manifest.settings.offset as u32,
            manifest.settings.size as u32,
        )?;
        flasher.erase_region(manifest.state.offset as u32, manifest.state.size as u32)?;
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    } else if current || native {
        check_storage()?;
        // The table may already be complete after an interrupted legacy install.
        if !managed {
            let nvs = read_region(
                &mut flasher,
                manifest.settings.offset,
                manifest.settings.size,
            )?;
            migrated = crate::legacy::migrated_preferences(&nvs);
        }
        // A save fault is repaired live after boot; reinstalling never clears it.
        if current
            && managed
            && app_fits(
                &table,
                app.offset,
                app_bytes.len(),
                (manifest.settings.offset, manifest.settings.size),
            )
        {
            writes.push((*app, app_bytes));
        } else {
            writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
        }
    } else {
        // A torn table may lack the spiffs entry while the old save is intact.
        progress("Reading the existing save", None);
        let raw = read_region(&mut flasher, 0x670000, 0x180000)?;
        let eeprom = match save::classify_public_partition(&raw) {
            save::PublicPartition::StickwalkerSave(image) => Some(image),
            // The completed intake survives retirement of the old filesystem.
            _ if raw_intake => None,
            save::PublicPartition::EmptyOrForeign => {
                flasher.erase_region(
                    manifest.settings.offset as u32,
                    manifest.settings.size as u32,
                )?;
                flasher.erase_region(manifest.state.offset as u32, manifest.state.size as u32)?;
                None
            }
            save::PublicPartition::DamagedSave(error) => {
                return Err(error.context("The older save is damaged. Nothing was erased. Use Device → Erase and Install for a fresh walker."));
            }
        };
        if eeprom.is_some() || raw_intake {
            check_storage()?;
            let nvs = read_region(&mut flasher, 0x9000, 0x5000)?;
            migrated = if public || eeprom.is_some() {
                Some(crate::legacy::preferences(&nvs))
            } else {
                crate::legacy::migrated_preferences(&nvs)
            };
            // A cut during the final sector can expose the marker early. The
            // legacy source is still intact then, even if its table entry is gone.
            if let Some(image) = eeprom {
                let staged = if raw_intake {
                    Some(read_region(&mut flasher, save::WALKER, save::EEPROM)?)
                } else {
                    None
                };
                write_intake(&image, staged.as_deref(), |offset, bytes| {
                    flasher.write_bin_to_flash(
                        offset as u32,
                        bytes,
                        &mut Progress {
                            report: progress,
                            units: 0,
                            region: bytes.len(),
                            previous: saved,
                            total: image_size + save::EEPROM,
                            label: "Transferring save",
                        },
                    )?;
                    saved += bytes.len();
                    Ok(())
                })?;
            }
            // Only these markers block the firmware's raw intake. Usually both
            // public checkpoint copies can stay intact until the first boot.
            for header in [0x620000, 0x621000, 0x700000, 0x701000] {
                let magic = read_region(&mut flasher, header + 68, 4)?;
                if magic.starts_with(b"SWK") && (b'0'..=b'6').contains(&magic[3]) {
                    flasher.erase_region(header as u32, 4096)?;
                }
            }
            // Keep the original NVS pages readable until the new firmware confirms them.
            flasher.erase_region(0xe000, 4096)?;
        }
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    }
    writes.sort_by_key(|(image, _)| image.kind == ImageKind::Partitions);
    let total = saved + writes.iter().map(|(_, b)| b.len()).sum::<usize>();
    for (image, bytes) in writes {
        flasher.write_bin_to_flash(
            image.offset as u32,
            bytes,
            &mut Progress {
                report: progress,
                units: 0,
                previous: saved,
                total,
                region: bytes.len(),
                label: "Writing firmware",
            },
        )?;
        saved += bytes.len();
    }
    progress("Restarting Stickwalker", None);
    let reset = Chip::Esp32s3.rtc_wdt_reset(flasher.connection());
    drop(flasher);
    // The reset itself may disconnect USB before the ROM acknowledges the last write.
    let until = Instant::now() + Duration::from_secs(25);
    while Instant::now() < until {
        thread::sleep(Duration::from_millis(300));
        let Ok(candidates) = ports() else { continue };
        if let Some((port, mut session, mut info)) = find_restarted(candidates, &chip_id, |path| {
            let mut session = Session::open(path).ok()?;
            let info = Info::read(&mut session).ok()?;
            Some((session, info))
        }) {
            ensure!(
                info.version == manifest.version.to_string() && info.build == build,
                "The expected firmware did not start"
            );
            if let Some(settings) = migrated {
                session.request(protocol::SETTINGS, &settings.to_bytes())?;
                if info.mode != DeviceMode::SaveFault {
                    // PREPARE commits preferences and the mounted save together.
                    session.request(protocol::PREPARE, &[])?;
                }
                info = Info::read(&mut session)?;
            }
            return Ok((port, session, info));
        }
    }
    reset.context(
        "Firmware was written, but the Stick could not restart. Reconnect it and try again.",
    )?;
    anyhow::bail!("Firmware was written. Reconnect the Stick to check its version.")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn interrupted_intake_publishes_its_header_after_the_body() {
        let mut source = vec![0x56; save::EEPROM];
        source[..8].copy_from_slice(b"nintendo");
        let image = save::EepromImage::parse(source).unwrap();
        let tail = save::EEPROM - 4096;
        for cut in [1, 4096, tail, tail + 8, save::EEPROM - 1, save::EEPROM] {
            let mut flash = vec![0xff; save::EEPROM];
            let mut remaining = cut;
            let result = write_intake(&image, None, |offset, data| {
                let at = offset - save::WALKER;
                let wrote = data.len().min(remaining);
                flash[at..at + wrote].copy_from_slice(&data[..wrote]);
                remaining -= wrote;
                ensure!(wrote == data.len(), "Power cut");
                Ok(())
            });
            assert_eq!(result.is_ok(), cut == save::EEPROM);
            let marked = flash.starts_with(b"nintendo");
            assert_eq!(marked, cut >= tail + 8);
            let staged = marked.then(|| flash.clone());
            write_intake(&image, staged.as_deref(), |offset, data| {
                assert!(cut < save::EEPROM, "complete intake was rewritten");
                let at = offset - save::WALKER;
                flash[at..at + data.len()].copy_from_slice(data);
                Ok(())
            })
            .unwrap();
            assert_eq!(flash, image.bytes());
        }
    }

    #[test]
    fn app_only_updates_require_matching_nvs_and_enough_application_space() {
        fn entry(label: &[u8], offset: u32, size: u32) -> [u8; 32] {
            let mut e = [0; 32];
            e[..2].copy_from_slice(&[0xaa, 0x50]);
            e[4..8].copy_from_slice(&offset.to_le_bytes());
            e[8..12].copy_from_slice(&size.to_le_bytes());
            e[12..12 + label.len()].copy_from_slice(label);
            e
        }
        let table = [
            entry(b"nvs", 0x9000, 0x6000),
            entry(b"factory", 0x10000, 0x60000),
        ]
        .concat();
        assert!(app_fits(&table, 0x10000, 0x60000, (0x9000, 0x6000)));
        assert!(!app_fits(&table, 0x10000, 0x60001, (0x9000, 0x6000)));
        assert!(!app_fits(&table, 0x10000, 0x60000, (0x9000, 0x5000)));
    }

    #[test]
    fn post_flash_settings_follow_the_chip_when_a_port_is_reused() {
        let target = "70:04:1d:dc:1a:20";
        let spare = "70:04:1d:dc:8b:04";
        for (old, new) in [("COM4", "COM12"), ("/dev/ttyACM0", "/dev/ttyACM1")] {
            let candidate = |path: &str, serial: &str| Port {
                path: path.into(),
                serial: serial.into(),
            };
            let lost = candidate(old, "6");
            let moved = candidate(new, "6");
            assert!(!lost.same_device(&moved));

            let ports = vec![candidate("known-spare", spare), lost, moved.clone()];
            let mut probed = Vec::new();
            let (port, _, _) = find_restarted(ports, "70041DDC1A20", |path| {
                probed.push(path.to_owned());
                let mac = if path == new { target } else { spare };
                Some((
                    (),
                    Info {
                        mac: mac.into(),
                        mode: DeviceMode::Ready,
                        version: "0.2.1".into(),
                        build: String::new(),
                        power: 0,
                        millivolts: 4000,
                        settings: Default::default(),
                    },
                ))
            })
            .expect("the flashed Stick reappeared");
            assert_eq!(probed, [old, new]);
            assert_eq!(port, moved);
        }
    }
}
