//! Two compiled StickWalker instances exchange their actual RMT light pulses.
use crate::{Rig, CYCLES_PER_MS};
use emu_core::Bus;
use esp_soc::{SocBus, Stop};
use std::{error::Error, fs, path::Path, rc::Rc};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn address(rig: &Rig, name: &str) -> Result<u32> {
    rig.machine
        .symbols
        .iter()
        .find(|(_, value)| value.as_str() == name)
        .map(|(&address, _)| address)
        .ok_or_else(|| format!("Missing {name}").into())
}

fn bytes(rig: &mut Rig, at: u32, length: u32) -> Result<Vec<u8>> {
    (at..at + length)
        .map(|i| {
            rig.machine
                .bus
                .read8(i)
                .map_err(|e| format!("{e:?}").into())
        })
        .collect()
}

struct Walkers([Rig; 2]);

impl Walkers {
    fn run(&mut self, ms: u64) -> Result<()> {
        let end = self.0.iter().map(|r| r.machine.bus.cycles()).max().unwrap() + ms * CYCLES_PER_MS;
        while self.0.iter().any(|r| r.machine.bus.cycles() < end) {
            let index =
                usize::from(self.0[0].machine.bus.cycles() > self.0[1].machine.bus.cycles());
            let until = (self.0[1 - index].machine.bus.cycles() + 128).min(end);
            let rig = &mut self.0[index];
            rig.machine.max_cycles = until;
            match rig.machine.run(u64::MAX) {
                Stop::Halted => {}
                other => return Err(format!("Walker {index}: {other:?}").into()),
            }
            rig.machine.bus.flush_ticks();
        }
        Ok(())
    }

    fn center(&mut self) -> Result<()> {
        for rig in &mut self.0 {
            rig.switch("front", true);
        }
        self.run(200)?;
        for rig in &mut self.0 {
            rig.switch("front", false);
        }
        self.run(180)
    }

    fn connect(&mut self) -> Result<()> {
        for rig in &mut self.0 {
            rig.center()?;
            rig.run(1000)?;
            let state = address(rig, "g_state")?;
            if rig
                .machine
                .bus
                .read8(state + 49)
                .map_err(|e| format!("{e:?}"))?
                != 1
            {
                return Err("Peer Connect was not selected from the game menu".into());
            }
        }
        self.center()?;
        let link = [
            address(&self.0[0], "eeprom_owner")?,
            address(&self.0[1], "eeprom_owner")?,
        ];
        for turn in 0..400 {
            self.run(25)?;
            if turn % 40 == 0 {
                println!(
                    "Peer link {:.2}s: {} / {} transmitted frames",
                    self.0[0].machine.seconds(),
                    self.0[0].optics.borrow().sent.len(),
                    self.0[1].optics.borrow().sent.len()
                );
            }
            if self
                .0
                .iter_mut()
                .zip(link)
                .all(|(rig, at)| rig.machine.bus.read8(at).is_ok_and(|active| active == 0))
            {
                return Ok(());
            }
        }
        Err("Two walkers did not finish their exchange".into())
    }
}

pub fn run(
    mut first: Rig,
    rom: &[u8],
    elf: &[u8],
    second_flash: &[u8],
    output: &Path,
) -> Result<()> {
    let mut second_image = second_flash.to_vec();
    second_image[..crate::SAVE].copy_from_slice(&first.machine.bus.flash[..crate::SAVE]);
    let mut second = Rig::new(rom, &second_image, true, false, 3910)?;
    second.machine.add_symbols(elf)?;
    first.run(2200)?;
    second.run(2200)?;
    first.optics.borrow_mut().timed = true;
    second.optics.borrow_mut().timed = true;
    first.machine.bus.periph.rmt.stream_symbols = true;
    second.machine.bus.periph.rmt.stream_symbols = true;
    first.optics.borrow_mut().remote = Some(Rc::downgrade(&second.optics));
    second.optics.borrow_mut().remote = Some(Rc::downgrade(&first.optics));
    let mut pair = Walkers([first, second]);
    let eeprom = [
        address(&pair.0[0], "eeprom")?,
        address(&pair.0[1], "eeprom")?,
    ];
    let identities = [
        bytes(&mut pair.0[0], eeprom[0] + 0x83, 40)?,
        bytes(&mut pair.0[1], eeprom[1] + 0x83, 40)?,
    ];
    if identities[0] == identities[1] {
        return Err("Peer fixtures have the same device identity".into());
    }
    pair.connect()?;
    let mut gifts = Vec::new();
    for index in 0..2 {
        let rig = &mut pair.0[index];
        let history = bytes(rig, eeprom[index] + 0xde24 + 8, 40)?;
        if history != identities[1 - index] {
            let state = address(rig, "g_state")?;
            rig.snapshot(output, &format!("failed-peer-{index}"))?;
            fs::write(
                output.join(format!("failed-peer-{index}-console.txt")),
                &rig.machine.console.all,
            )?;
            return Err(format!(
                "Walker {index} did not record its peer: result {}",
                rig.machine
                    .bus
                    .read8(state + 45)
                    .map_err(|e| format!("{e:?}"))?
            )
            .into());
        }
        // (4000 + 3000) * 10 wraps at 16 bits before adding daily totals.
        // 4464 + 4500 + 3500 selects item 2 for A and item 3 for B.
        let expected = bytes(
            rig,
            eeprom[index] + 0x8f00 + 140 + 2 * (2 + index as u32),
            2,
        )?;
        let gift = bytes(rig, eeprom[index] + 0xcec8, 40)?;
        if gift[..2] != expected {
            return Err("Peer reward lost H8 16-bit arithmetic".into());
        }
        gifts.push(gift);
        rig.snapshot(output, &format!("peer-{index}"))?;
        rig.run(16000)?;
    }
    let first_frames: Vec<_> = pair
        .0
        .iter()
        .map(|rig| rig.optics.borrow().sent.len())
        .collect();
    pair.connect()?;
    for (index, rig) in pair.0.iter_mut().enumerate() {
        let state = address(rig, "g_state")?;
        if rig
            .machine
            .bus
            .read8(state + 45)
            .map_err(|e| format!("{e:?}"))?
            != 5
            || bytes(rig, eeprom[index] + 0xcec8, 40)? != gifts[index]
        {
            return Err("The same peer awarded a second gift on the same day".into());
        }
        rig.center()?;
        // The fixture starts one minute before midnight; both initial
        // encounters finish first, then this wait crosses the day boundary.
        rig.run(45000)?;
        if bytes(rig, eeprom[index] + 0xde24 + 8, 40)? != [0xff; 40] {
            return Err("Midnight did not clear the peer identity history".into());
        }
        let archived = bytes(rig, eeprom[index] + 0xcef0, 4)?;
        let expected: u32 = if index == 0 { 4500 } else { 3500 };
        if archived != expected.to_be_bytes() {
            return Err("Midnight did not archive the previous day's steps".into());
        }
        let asleep = address(rig, "screen_asleep")?;
        if bytes(rig, asleep, 1)?[0] != 0 {
            rig.press("front")?;
        }
        rig.run(1000)?;
    }
    pair.connect()?;
    for (index, rig) in pair.0.iter_mut().enumerate() {
        if bytes(rig, eeprom[index] + 0xde24 + 8, 40)? != identities[1 - index]
            || bytes(rig, eeprom[index] + 0xcecc, 2)? == [0, 0]
        {
            return Err("A peer could not earn a new gift after midnight".into());
        }
        fs::write(
            output.join(format!("peer-{index}-flash.bin")),
            &rig.machine.bus.flash,
        )?;
        fs::write(
            output.join(format!("peer-{index}-wire.txt")),
            rig.optics
                .borrow()
                .sent
                .iter()
                .map(|wire| crate::peer::hex(wire) + "\n")
                .collect::<String>(),
        )?;
        fs::write(
            output.join(format!("peer-{index}-console.txt")),
            &rig.machine.console.all,
        )?;
    }
    fs::write(output.join("peers.txt"), format!("first_exchange_frames={first_frames:?}\nmutual_history=true\ngift_wrap=true\nsame_day_suppressed=true\nmidnight_reset=true\n"))?;
    Ok(())
}
