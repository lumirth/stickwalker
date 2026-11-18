use crate::{
    protocol::{self, Session},
    release::Package,
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
    let serial = serialport::new(&port.path, 115200)
        .timeout(Duration::from_secs(3))
        .open_native()?;
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
    let temp = tempfile::NamedTempFile::new()?;
    flasher.read_flash(offset as u32, size as u32, 4096, 64, temp.path().into())?;
    let bytes = fs::read(temp.path())?;
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

/// The partition table is written last. A retry can still find the legacy
/// source, or the completed EEPROM intake if legacy bank sectors were retired.
pub fn install(
    port: &Port,
    session: Option<Session>,
    package: &Package,
    fresh: bool,
    progress: &mut dyn FnMut(&str, Option<u8>),
) -> Result<(Port, Info)> {
    let manifest = &package.manifest;
    let images = manifest
        .images
        .iter()
        .map(|image| Ok((image, package.image(image)?)))
        .collect::<Result<Vec<_>>>()?;
    let (app, app_bytes) = images
        .iter()
        .find(|(image, _)| image.kind == "application")
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
        if info.mode != DeviceMode::SaveFault {
            session.request(protocol::PREPARE, &[])?;
            managed = true;
        }
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
    if fresh {
        flasher.erase_region(
            manifest.settings.offset as u32,
            manifest.settings.size as u32,
        )?;
        flasher.erase_region(manifest.state.offset as u32, manifest.state.size as u32)?;
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    } else if current || native {
        ensure!(
            manifest.storage == "SWK6"
                && manifest.state.offset == save::WALKER
                && manifest.state.size == 0x1f0000
                && app.offset == 0x10000
                && manifest.settings.offset == 0x9000
                && manifest.settings.size == 0x6000,
            "This firmware package cannot use the installed storage."
        );
        // A save fault is repaired live after boot; reinstalling never clears it.
        if current && managed {
            writes.push((*app, app_bytes));
        } else {
            writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
        }
    } else if public {
        progress("Reading the existing save", None);
        let raw = read_region(&mut flasher, 0x670000, 0x180000)?;
        let nvs = read_region(&mut flasher, 0x9000, 0x5000)?;
        let eeprom = match save::classify_public_partition(&raw) {
            save::PublicPartition::StickwalkerSave(image) => image,
            // Retiring bank headers can overlap LittleFS data. A verified intake
            // was written first; prefer the original source whenever it still reads.
            _ if raw_intake => read_region(&mut flasher, save::WALKER, save::EEPROM)?,
            save::PublicPartition::EmptyOrForeign => {
                flasher.erase_region(
                    manifest.settings.offset as u32,
                    manifest.settings.size as u32,
                )?;
                flasher.erase_region(manifest.state.offset as u32, manifest.state.size as u32)?;
                Vec::new()
            }
            save::PublicPartition::DamagedSave(error) => {
                return Err(error.context("The older save is damaged. Nothing was erased. Use Device → Erase and Install for a fresh walker."));
            }
        };
        if !eeprom.is_empty() {
            migrated = Some(crate::legacy::preferences(&nvs));
            // Finish the intake before erasing any overlapping legacy sectors.
            flasher.write_bin_to_flash(
                save::WALKER as u32,
                &eeprom,
                &mut Progress {
                    report: progress,
                    units: 0,
                    region: eeprom.len(),
                    previous: 0,
                    total: image_size + eeprom.len(),
                    label: "Transferring save",
                },
            )?;
            saved = eeprom.len();
            for header in [0x620000, 0x621000, 0x700000, 0x701000] {
                flasher.erase_region(header, 4096)?;
            }
            // This is the newly added NVS page (formerly OTA metadata).
            // Existing Arduino NVS pages remain readable by ESP-IDF.
            flasher.erase_region(0xe000, 4096)?;
        }
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    } else if raw_intake {
        // A completed intake can outlive an interrupted partition-table write.
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    } else {
        flasher.erase_region(
            manifest.settings.offset as u32,
            manifest.settings.size as u32,
        )?;
        flasher.erase_region(manifest.state.offset as u32, manifest.state.size as u32)?;
        writes.extend(images.iter().map(|(image, bytes)| (*image, bytes)));
    }
    writes.sort_by_key(|(image, _)| image.kind == "partitions");
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
    let _ = Chip::Esp32s3.rtc_wdt_reset(flasher.connection());
    drop(flasher);
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
                info.version == manifest.version && info.build == build,
                "The expected firmware did not start"
            );
            if let Some(settings) = migrated {
                session.request(protocol::SETTINGS, &settings.to_bytes())?;
                // PREPARE flushes preferences; the host controls any reset.
                session.request(protocol::PREPARE, &[])?;
                info = Info::read(&mut session)?;
            }
            return Ok((port, info));
        }
    }
    anyhow::bail!("Firmware was written. Reconnect the Stick to check its version.")
}

#[cfg(test)]
mod tests {
    use super::*;

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
