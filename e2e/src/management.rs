//! The desktop's framing through the emulated USB controller into real firmware.
use crate::{Rig, SAVE};
use emu_core::Bus;
use esp_soc::SocBus;
use std::{error::Error, path::Path};
use host::{protocol, save};
type Result<T> = std::result::Result<T, Box<dyn Error>>;
pub(super) fn request(rig: &mut Rig, op: u8, payload: &[u8]) -> Result<Vec<u8>> {
    rig.machine.console.usb.clear();
    rig.machine
        .bus
        .serial_input(&protocol::frame(op, 13, payload));
    let mut decoder = protocol::Decoder::default();
    for _ in 0..400 {
        rig.run(5)?;
        for b in std::mem::take(&mut rig.machine.console.usb) {
            if let Some(raw) = decoder.push(b) {
                if raw[3] == op && raw[4..6] == 13u16.to_le_bytes() {
                    if raw[8] != 0 {
                        return Err(format!("Management {op} failed: {}", raw[8]).into());
                    }
                    return Ok(raw[9..raw.len() - 4].to_vec());
                }
            }
        }
    }
    Err(format!("No management response to {op}").into())
}
fn read(rig: &mut Rig, offset: usize, count: usize) -> Result<Vec<u8>> {
    let mut body = (offset as u32).to_le_bytes().to_vec();
    body.extend((count as u16).to_le_bytes());
    request(rig, protocol::READ, &body)
}
fn snapshot(rig: &mut Rig) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    while bytes.len() < protocol::SAVE_SIZE {
        bytes.extend(read(
            rig,
            bytes.len(),
            (protocol::SAVE_SIZE - bytes.len()).min(protocol::CHUNK),
        )?);
    }
    Ok(bytes)
}
pub fn check(rig: &mut Rig, output: &Path) -> Result<()> {
    rig.machine
        .bus
        .serial_input(b"boot noise\xc0SW\x02\x05\x00\xdb\x22\xc0");
    let info = request(rig, protocol::STATUS, &[])?;
    assert_eq!(info.len(), 225);
    request(rig, protocol::IDENTIFY, &[])?;
    let mut prefs = info[74..225].to_vec();
    prefs[0] = 5;
    prefs[47..49].copy_from_slice(&[0xc0, 0xdb]);
    prefs[55 + 5 * 16..55 + 6 * 16].fill(0);
    prefs[55 + 5 * 16..55 + 5 * 16 + 8].copy_from_slice(b"Pea Soup");
    let mut truncated = protocol::frame(protocol::SETTINGS, 12, &prefs);
    truncated.insert(truncated.len() - 1, 0xdb);
    rig.machine.bus.serial_input(&truncated);
    assert_eq!(
        &request(rig, protocol::STATUS, &[])?[74..225],
        &info[74..225]
    );
    request(rig, protocol::SETTINGS, &prefs)?;
    rig.run(400)?;
    assert_eq!(&request(rig, protocol::STATUS, &[])?[74..225], &prefs);
    // Desktop palette writes must preserve controls changed on the Stick.
    let mut palette_only = prefs.clone();
    palette_only[0] |= 0x80;
    palette_only[1..7].fill(0xff);
    request(rig, protocol::SETTINGS, &palette_only)?;
    assert_eq!(&request(rig, protocol::STATUS, &[])?[74..225], &prefs);
    let original = snapshot(rig)?;
    let stored = save::decode_current(&rig.machine.bus.flash[SAVE..])?;
    assert_eq!(&original[..save::EEPROM], &stored.0);

    // An abandoned export releases the borrowed buffer and catches game time up.
    read(rig, 0, 480)?;
    rig.run(2200)?;
    let later = snapshot(rig)?;
    let seconds =
        |bytes: &[u8]| u32::from_be_bytes(bytes[0x156 + 8..0x156 + 12].try_into().unwrap());
    assert!(
        seconds(&later) >= seconds(&original) + 2,
        "Abandoned export did not resume the game clock"
    );

    let mut incoming = later.clone();
    incoming[0x7000] ^= 0x5a;
    let reboots = rig.machine.reboots;
    let mut wrong = 4080u32.to_le_bytes().to_vec();
    wrong.extend(&incoming[4080..8160]);
    assert!(request(rig, protocol::WRITE, &wrong).is_err());
    let mut partial = 0u32.to_le_bytes().to_vec();
    partial.extend(&incoming[..4080]);
    partial[4 + 0x400] ^= 0x5a;
    request(rig, protocol::WRITE, &partial)?;
    for (op, payload) in [(protocol::IDENTIFY, &[][..]), (protocol::SETTINGS, prefs.as_slice()), (protocol::PREPARE, &[][..])] {
        assert_eq!(request(rig, op, payload).unwrap_err().to_string(),
                   format!("Management {op} failed: 1"));
    }
    rig.run(2200)?;
    let after_partial = snapshot(rig)?;
    assert_eq!(after_partial[0x400], later[0x400]);
    assert_eq!(
        after_partial[0x7000], later[0x7000],
        "Incomplete import changed the walk"
    );
    assert!(
        seconds(&after_partial) >= seconds(&later) + 2,
        "Abandoned import did not resume the game clock"
    );
    for (index, chunk) in incoming.chunks(protocol::CHUNK).enumerate() {
        let mut data = ((index * protocol::CHUNK) as u32).to_le_bytes().to_vec();
        data.extend(chunk);
        request(rig, protocol::WRITE, &data)?;
    }
    assert_eq!(
        rig.machine.reboots, reboots,
        "Save import rebooted the device"
    );
    let restored = snapshot(rig)?;
    assert_eq!(restored[0x7000], incoming[0x7000]);
    assert_eq!(&restored[0x8f00..0x8f52], &incoming[0x8f00..0x8f52]);
    request(rig, protocol::PREPARE, &[])?;
    rig.machine.reboot();
    rig.run(2200)?;
    assert_eq!(&request(rig, protocol::STATUS, &[])?[74..225], &prefs);
    // Plugging in mid-stride must export the partial FIFO batch, not the
    // last four-second checkpoint. No button press wakes the screen here.
    rig.usb_power(false, 3910);
    rig.run(61_000)?;
    rig.walk(16)?;
    for _ in 0..400 {
        if rig.machine.bus.sleeping {
            break;
        }
        rig.run(1)?;
    }
    assert!(rig.machine.bus.sleeping, "Pocket walker did not sleep");
    let state = *rig
        .machine
        .symbols
        .iter()
        .find(|(_, n)| n.as_str() == "g_state")
        .ok_or("Missing game state")?
        .0;
    let before = rig
        .machine
        .bus
        .read32(state)
        .map_err(|e| format!("{e:?}"))?;
    for sample in 0..250 {
        let phase = sample as f64 * std::f64::consts::TAU * 1.86 / 100.0;
        rig.imu.borrow_mut().sample = [
            (2700.0 * phase.sin()) as i16,
            (5300.0 * phase.cos()) as i16,
            16384 + (3600.0 * phase.sin()) as i16,
        ];
        rig.run(10)?;
    }
    rig.usb_power(true, 3910);
    // A sleeping USB controller cannot enumerate until the next wake.
    rig.machine.bus.periph.usb.connected = false;
    for _ in 0..4100 {
        rig.run(1)?;
        if !rig.machine.bus.sleeping {
            break;
        }
    }
    assert!(
        !rig.machine.bus.sleeping,
        "Cable insertion never reached a wake"
    );
    rig.machine.bus.periph.usb.connected = true;
    let snapshot = snapshot(rig)?;

    let after = rig
        .machine
        .bus
        .read32(state)
        .map_err(|e| format!("{e:?}"))?;
    assert!(
        after >= before + 3,
        "USB snapshot omitted paced pocket steps"
    );
    assert_eq!(
        snapshot,
        save::decode_current(&rig.machine.bus.flash[SAVE..])?.0
    );
    let asleep = *rig
        .machine
        .symbols
        .iter()
        .find(|(_, name)| name.as_str() == "screen_asleep")
        .ok_or("Missing screen sleep flag")?
        .0;
    assert_ne!(
        rig.machine
            .bus
            .read8(asleep)
            .map_err(|e| format!("{e:?}"))?,
        0
    );
    request(rig, protocol::SETTINGS, &prefs)?;
    assert_eq!(
        rig.machine
            .bus
            .read8(asleep)
            .map_err(|e| format!("{e:?}"))?,
        0,
        "Palette edit did not wake the screen"
    );
    // Corrupt both checkpoints, reboot to recovery, then import through USB.
    for base in [0x10000, 0xf0000] {
        rig.machine.bus.flash[SAVE + base + 64] ^= 0x80;
    }
    rig.machine.reboot();
    rig.run(2200)?;
    assert_eq!(request(rig, protocol::STATUS, &[])?[70], 2);
    for (index, chunk) in snapshot.chunks(protocol::CHUNK).enumerate() {
        let mut page = ((index * protocol::CHUNK) as u32).to_le_bytes().to_vec();
        page.extend(chunk);
        request(rig, protocol::WRITE, &page)?;
    }
    assert_eq!(request(rig, protocol::STATUS, &[])?[70], 0);
    rig.machine.reboot();
    rig.run(2200)?;
    assert_eq!(request(rig, protocol::STATUS, &[])?[70], 0);
    request(rig, protocol::WRITE, &[])?;
    assert_eq!(request(rig, protocol::STATUS, &[])?[70], 0);
    // A runtime fault can leave valid banks behind. Replacements must outrank
    // them after reboot, including when the newer bank is bank B.
    let ready = *rig
        .machine
        .symbols
        .iter()
        .find(|(_, name)| name.as_str() == "game_ready")
        .ok_or("Missing game readiness flag")?
        .0;
    for marker in [0x31, 0x32] {
        rig.machine
            .bus
            .write8(ready, 0)
            .map_err(|e| format!("{e:?}"))?;
        assert_eq!(request(rig, protocol::STATUS, &[])?[70], 2);
        let mut replacement = snapshot.clone();
        replacement[0x7000] = marker;
        for (index, chunk) in replacement.chunks(protocol::CHUNK).enumerate() {
            let mut page = ((index * protocol::CHUNK) as u32).to_le_bytes().to_vec();
            page.extend(chunk);
            request(rig, protocol::WRITE, &page)?;
        }
        rig.machine.reboot();
        rig.run(2200)?;
        assert_eq!(request(rig, protocol::STATUS, &[])?[70], 0);
        assert_eq!(
            save::decode_current(&rig.machine.bus.flash[SAVE..])?.0[0x7000],
            marker
        );
    }
    // A future-version NVS page must recover through ESP-IDF, then accept and
    // persist settings again. Start with an actual page written by the firmware.
    let page = (0x9000..0xf000)
        .step_by(4096)
        .find(|&at| rig.machine.bus.flash[at..at + 4] == [0xfe, 0xff, 0xff, 0xff])
        .ok_or("Missing active NVS page")?;
    let flash = &mut rig.machine.bus.flash;
    let mut checksum = crc32fast::Hasher::new_with_initial(0xffffffff);
    checksum.update(&flash[page + 4..page + 28]);
    assert_eq!(
        &flash[page + 28..page + 32],
        &checksum.finalize().to_le_bytes()
    );
    flash[page + 8] = 0xfd;
    let mut checksum = crc32fast::Hasher::new_with_initial(0xffffffff);
    checksum.update(&flash[page + 4..page + 28]);
    flash[page + 28..page + 32].copy_from_slice(&checksum.finalize().to_le_bytes());
    rig.machine.reboot();
    rig.run(2200)?;
    request(rig, protocol::SETTINGS, &prefs)?;
    request(rig, protocol::PREPARE, &[])?;
    rig.machine.reboot();
    rig.run(2200)?;
    assert_eq!(&request(rig, protocol::STATUS, &[])?[74..225], &prefs);
    rig.snapshot(output, "managed")?;
    // Render the longest supported name, including punctuation, in the actual
    // firmware menu. Keep this capture beside the game view for visual review.
    let name = b"A'(#&_!)/-1234Z";
    prefs[55 + 5 * 16..55 + 6 * 16].fill(0);
    prefs[55 + 5 * 16..55 + 5 * 16 + name.len()].copy_from_slice(name);
    request(rig, protocol::SETTINGS, &prefs)?;
    rig.open_settings()?;
    rig.snapshot(output, "palette-name")?;
    // The host cannot enumerate a stationary sleeping Stick until VBUS keeps it awake.
    rig.usb_power(false, 3910);
    rig.imu.borrow_mut().sample = [0, 0, 16384];
    // Let the settings menu time out and the game finish its last active FIFO batch.
    for _ in 0..240 {
        rig.run(1000)?;
        if !rig.pm.borrow().rail() && !rig.imu.borrow().fifo_enabled && rig.machine.bus.sleeping {
            break;
        }
    }
    assert!(!rig.pm.borrow().rail() && !rig.imu.borrow().fifo_enabled && rig.machine.bus.sleeping,
            "Walker did not reach stationary sleep");
    let previous = rig.pm.borrow().source_reads;
    for _ in 0..220 {
        rig.run(10)?;
        if rig.pm.borrow().source_reads > previous && rig.machine.bus.sleeping { break; }
    }
    assert!(rig.pm.borrow().source_reads > previous);
    rig.usb_power(true, 3910);
    rig.machine.bus.periph.usb.connected = false;
    rig.run(2100)?;
    assert!(!rig.machine.bus.sleeping, "Stationary walker missed cable insertion");
    rig.machine.bus.periph.usb.connected = true;
    assert_eq!(request(rig, protocol::STATUS, &[])?[73], 1);
    println!(
        "USB management: coherent EEPROM export, abandoned-transfer recovery, durable palettes, live import, save-fault repair, reset and pocket catch-up"
    );
    Ok(())
}
