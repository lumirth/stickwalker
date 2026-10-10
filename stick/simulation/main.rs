mod board;
use esp_soc::{SocBus, Stop};
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    assert!(
        args.len() == 5,
        "usage: stickwalker-e2e ROM.elf MERGED.bin SCRIPT OUTPUT_DIR"
    );
    let out = PathBuf::from(&args[4]);
    std::fs::create_dir_all(&out).unwrap();
    let mut machine = esp32s3::machine([0x02, 0x53, 0x03, 0x00, 0x00, 0x01]);
    let inputs = Arc::new(Mutex::new(board::Inputs::default()));
    machine.bus.board = Box::new(board::StickS3::new(inputs.clone()));
    machine.bus.set_psram_size(8 << 20).unwrap();
    machine.bus.attach_board_devices();
    machine.load_rom(&std::fs::read(&args[1]).unwrap()).unwrap();
    machine
        .write_flash(0, &std::fs::read(&args[2]).unwrap())
        .unwrap();
    machine.console.capture = true;
    machine.boot_rom();
    for line in std::fs::read_to_string(&args[3]).unwrap().lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.is_empty() || fields[0].starts_with('#') {
            continue;
        }
        match fields.as_slice() {
            ["run", ms] => {
                let target = machine.bus.cycles + ms.parse::<u64>().unwrap() * 240_000;
                machine.max_cycles = target;
                let stop = machine.run(u64::MAX);
                std::fs::write(out.join("console.txt"), &machine.console.all).unwrap();
                let console = String::from_utf8_lossy(&machine.console.all);
                if !matches!(stop, Stop::Halted)
                    || machine.bus.cycles < target
                    || console.contains("PW_STICK_FATAL")
                    || console.contains("Guru Meditation")
                {
                    machine
                        .write_tft_png(out.join("failure.png").to_str().unwrap(), 1)
                        .unwrap();
                    std::fs::write(out.join("flash.bin"), &machine.bus.flash).unwrap();
                    panic!(
                        "firmware stopped: {stop:?}; reset cause={}\n{console}",
                        machine.bus.reset_cause()
                    );
                }
            }
            ["press", key] | ["release", key] => {
                let down = fields[0] == "press";
                match *key {
                    "M" => machine.bus.gpio_set_input(11, !down),
                    "R" => machine.bus.gpio_set_input(12, !down),
                    "L" => {
                        let mut state = inputs.lock().unwrap();
                        state.left_latched |= down && !state.left;
                        state.left = down;
                    }
                    _ => panic!("unknown physical button {key}"),
                }
            }
            ["optical", name] => {
                let state = inputs.lock().unwrap();
                let mut trace = String::new();
                for (index, frame) in state.optical.iter().enumerate() {
                    trace += &format!("frame {index}\n");
                    for (cycles, high) in frame {
                        trace += &format!("{cycles} {}\n", u8::from(*high));
                    }
                }
                std::fs::write(out.join(name), trace).unwrap();
            }
            ["gpio", pin, level] => machine
                .bus
                .gpio_set_input(pin.parse().unwrap(), *level == "1"),
            ["screen", name] => machine
                .write_tft_png(out.join(name).to_str().unwrap(), 1)
                .unwrap(),
            ["flash", name] => std::fs::write(out.join(name), &machine.bus.flash).unwrap(),
            ["reset"] => {
                machine.bus.request_reset(1);
                machine.reboot();
            }
            _ => panic!("unknown action: {line}"),
        }
    }
    std::fs::write(out.join("console.txt"), &machine.console.all).unwrap();
    std::fs::write(out.join("flash.bin"), &machine.bus.flash).unwrap();
    eprintln!(
        "cycles={} reboots={}\n{}",
        machine.bus.cycles,
        machine.reboots,
        machine.bus.report()
    );
}
