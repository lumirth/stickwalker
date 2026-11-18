use crate::{Rig, SAVE};
use esp_soc::SocBus;

pub fn repair(rig: &mut Rig, images: &[(usize, &[u8])]) -> Result<(), String> {
    sync(rig)?;
    let save = rig.machine.bus.flash[SAVE..].to_vec();
    command(rig, 0x0d, &[0; 8], 0, 20)?; // SPI_ATTACH
    for &(offset, firmware) in images {
        write_image(rig, offset, firmware)?;
        if rig.machine.bus.flash[offset..offset + firmware.len()] != *firmware {
            return Err("USB reflash wrote the wrong firmware bytes".into());
        }
    }
    if rig.machine.bus.flash[SAVE..] != save {
        return Err("USB reflash touched the walker's save".into());
    }
    restart(rig)
}

pub fn restart(rig: &mut Rig) -> Result<(), String> {
    // Use the ROM register commands behind esptool --after watchdog_reset.
    // A full reset re-samples GPIO0 after the Power button has been released.
    for (address, value) in [
        (0x6000_80b0, 0x50d8_3aa1), // Write protection
        (0x6000_809c, 2000),        // Slow-clock ticks before reset
        (0x6000_8098, 0xd000_0102),
    ] {
        command(rig, 0x09, &words(&[address, value, u32::MAX, 0]), 0, 10)?;
    }
    let resets = rig.machine.reboots;
    rig.run(1200)?;
    if rig.machine.reboots == resets
        || rig.frame().iter().all(|&pixel| pixel == 0)
        || !rig.pm.borrow().rail()
    {
        return Err("Leaving the downloader did not boot to the walker".into());
    }
    Ok(())
}

fn write_image(rig: &mut Rig, offset: usize, firmware: &[u8]) -> Result<(), String> {
    let block_size = 1024;
    let count = firmware.len().div_ceil(block_size);
    let begin = [
        (firmware.len().div_ceil(4096) * 4096) as u32,
        count as u32,
        block_size as u32,
        offset as u32,
        0,
    ];
    // esptool's ROM FLASH_BEGIN timeout: 40 seconds/MB, at least 3 seconds.
    let erase_timeout = (40_000 * firmware.len() as u64)
        .div_ceil(1_000_000)
        .max(3000);
    command(rig, 0x02, &words(&begin), 0, erase_timeout)?;
    for (sequence, block) in firmware.chunks(block_size).enumerate() {
        let mut data = words(&[block_size as u32, sequence as u32, 0, 0]);
        data.extend(block);
        data.resize(16 + block_size, 0xff);
        let checksum = data[16..].iter().fold(0xef, |sum, byte| sum ^ byte);
        command(rig, 0x03, &data, u32::from(checksum), 80)?;
    }
    Ok(())
}

pub fn sync(rig: &mut Rig) -> Result<(), String> {
    let mut sync = vec![7, 7, 0x12, 0x20];
    sync.extend([0x55; 32]);
    command(rig, 0x08, &sync, 0, 180)
}

fn words(values: &[u32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn command(rig: &mut Rig, opcode: u8, body: &[u8], checksum: u32, ms: u64) -> Result<(), String> {
    let mut request = vec![0, opcode];
    request.extend((body.len() as u16).to_le_bytes());
    request.extend(checksum.to_le_bytes());
    request.extend(body);
    let mut packet = vec![0xc0];
    for byte in request {
        match byte {
            0xc0 => packet.extend([0xdb, 0xdc]),
            0xdb => packet.extend([0xdb, 0xdd]),
            _ => packet.push(byte),
        }
    }
    packet.push(0xc0);
    let start = rig.machine.console.usb.len();
    rig.machine.bus.serial_input(&packet);
    let mut read = start;
    for elapsed in (0..ms).step_by(5) {
        rig.run(5.min(ms - elapsed))?;
        let available = &rig.machine.console.usb;
        let Some(last_end) = available[read..].iter().rposition(|&byte| byte == 0xc0) else {
            continue;
        };
        let end = read + last_end;
        for response in available[read..=end].split(|&byte| byte == 0xc0) {
            let mut decoded = Vec::new();
            let mut bytes = response.iter().copied();
            while let Some(byte) = bytes.next() {
                decoded.push(if byte == 0xdb {
                    match bytes.next() {
                        Some(0xdc) => 0xc0,
                        Some(0xdd) => 0xdb,
                        _ => break,
                    }
                } else {
                    byte
                });
            }
            if decoded.len() >= 12 && decoded[..2] == [1, opcode] {
                let length = u16::from_le_bytes([decoded[2], decoded[3]]) as usize;
                if decoded.len() == 8 + length && decoded[decoded.len() - 4] == 0 {
                    return Ok(());
                }
                return Err(format!("ROM command {opcode:02x} failed: {decoded:02x?}"));
            }
        }
        read = end + 1;
    }
    Err(format!(
        "No USB ROM response to {opcode:02x} at {:.3}s, flash address {:08x}, cores {:x?}",
        rig.machine.seconds(),
        rig.machine.bus.periph.spi1.regs.read(4),
        rig.machine.cores.iter().map(|c| c.pc).collect::<Vec<_>>()
    ))
}
