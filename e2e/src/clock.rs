use emu_core::{bus::Bus, Core};
use esp32s3::soc::Machine;

// Invoke the actual windowed ROM routines with a call4 frame.
fn call(machine: &mut Machine, entry: u32, argument: u32) -> Result<(), String> {
    let cpu = &mut machine.cores[0];
    cpu.pc = entry;
    cpu.ps = (1 << 18) | (1 << 16) | (1 << 5);
    cpu.windowbase = 0;
    cpu.windowstart = 1;
    cpu.set_ar(1, 0x3fcf0000);
    cpu.set_ar(4, 0x40001000);
    cpu.set_ar(6, argument);
    let start = machine.bus.cycles;
    while machine.cores[0].pc != 0x40001000 {
        if machine.bus.cycles - start > 240_000_000 {
            return Err("ROM delay did not return within one second".into());
        }
        let before = machine.cores[0].ccount;
        machine.cores[0]
            .step(&mut machine.bus)
            .result()
            .map_err(|e| format!("{e:?}"))?;
        let elapsed = machine.cores[0].ccount.wrapping_sub(before);
        machine.bus.tick(elapsed);
    }
    Ok(())
}

pub fn check(rom: &[u8]) -> Result<(), String> {
    let mut machine = esp32s3::soc::machine([0; 6]);
    machine.load_rom(rom)?;
    for (source, selection, mhz) in [(1 << 10, 0, 80), (1 << 10, 1, 160), (1 << 10, 2, 240), (0, 0, 40), (3, 0, 10)] {
        machine
            .bus
            .write32(0x600c0060, source)
            .map_err(|e| format!("{e:?}"))?;
        machine
            .bus
            .write32(0x600c0010, selection)
            .map_err(|e| format!("{e:?}"))?;
        call(&mut machine, 0x40001a4c, mhz)?; // esp_rom_set_cpu_ticks_per_us
        let start = machine.bus.cycles;
        call(&mut machine, 0x40000600, 100_000)?; // esp_rom_delay_us
        let elapsed_us = (machine.bus.cycles - start) / 240;
        if !(100_000..=100_005).contains(&elapsed_us) {
            return Err(format!(
                "100 ms ROM delay at {mhz} MHz took {elapsed_us} us"
            ));
        }
        println!("ROM delay at {mhz} MHz: {elapsed_us} us");
    }
    Ok(())
}
