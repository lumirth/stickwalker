mod board;
mod peer;
use esp_soc::{SocBus, Stop};
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    assert!(
        args.len() == 5 || args.len() == 7,
        "usage: stickwalker-e2e ROM.elf MERGED.bin SCRIPT OUTPUT_DIR [FLASH_OFFSET FLASH_IMAGE]"
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
    if args.len() == 7 {
        machine
            .write_flash(args[5].parse().unwrap(), &std::fs::read(&args[6]).unwrap())
            .unwrap();
    }
    if std::env::var_os("STICK_E2E_OPCODE_TIMING").is_some() {
        machine.set_approximate_jit_timing(1, 1).unwrap();
        machine.set_approximate_jit_frontiers(true).unwrap();
        for core in &mut machine.cores {
            core.price_control = true;
        }
    }
    if let Ok(value) = std::env::var("STICK_E2E_FLASH_TIMING") {
        let (program, erase) = value
            .split_once(',')
            .expect("flash timing: program_us,sector_erase_us");
        machine.bus.periph.spi1.program_cycles = program.parse::<u64>().unwrap() * 240;
        machine.bus.periph.spi1.sector_erase_cycles = erase.parse::<u64>().unwrap() * 240;
    }
    machine.console.capture = true;
    machine.boot_rom();
    for line in std::fs::read_to_string(&args[3]).unwrap().lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.is_empty() || fields[0].starts_with('#') {
            continue;
        }
        match fields.as_slice() {
            ["run", ms] | ["run-until-timer-wake", ms] | ["run-until-peer", ms] => {
                let deadline = machine.bus.cycles + ms.parse::<u64>().unwrap() * 240_000;
                let timer_wake = fields[0] == "run-until-timer-wake";
                let peer_done = fields[0] == "run-until-peer";
                loop {
                    if peer_done
                        && inputs
                            .lock()
                            .unwrap()
                            .peer
                            .as_ref()
                            .is_some_and(|peer| peer.complete() || peer.error().is_some())
                    {
                        break;
                    }
                    if timer_wake && machine.bus.periph.rtc.read(0x130) & 8 != 0 {
                        break;
                    }
                    if machine.bus.cycles >= deadline {
                        assert!(!timer_wake, "RTC timer did not wake the CPU within {ms} ms");
                        break;
                    }
                    let target = if timer_wake || peer_done {
                        (machine.bus.cycles + 240).min(deadline)
                    } else {
                        deadline
                    };
                    machine.max_cycles = target;
                    let stop = machine.run(u64::MAX);
                    let console = String::from_utf8_lossy(&machine.console.all);
                    if !matches!(stop, Stop::Halted)
                        || machine.bus.cycles < target
                        || console.contains("PW_STICK_FATAL")
                        || console.contains("Guru Meditation")
                    {
                        std::fs::write(out.join("console.txt"), &machine.console.all).unwrap();
                        machine
                            .write_tft_png(out.join("failure.png").to_str().unwrap(), 1)
                            .unwrap();
                        std::fs::write(out.join("flash.bin"), &machine.bus.flash).unwrap();
                        let mut registers = format!(
                            "action={line}\ncycles={}\n{}",
                            machine.bus.cycles,
                            machine.dump_regs()
                        );
                        for offset in [0x0, 0x18, 0x1c, 0x44, 0x98, 0x9c, 0xa0, 0xa4, 0xa8] {
                            registers += &format!(
                                "RTC+{offset:03x}={:08x}\n",
                                machine.bus.periph.rtc.read(offset)
                            );
                        }
                        std::fs::write(out.join("failure-registers.txt"), registers).unwrap();
                        std::fs::write(out.join("failure-sram.bin"), &machine.bus.sram).unwrap();
                        panic!(
                            "firmware stopped: {stop:?}; reset cause={}\n{console}",
                            machine.bus.reset_cause()
                        );
                    }
                }
                std::fs::write(out.join("console.txt"), &machine.console.all).unwrap();
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
            ["adc", pin, volts] => machine.bus.analog_set(
                pin.parse().unwrap(),
                esp_periph::AnalogSource::Const(volts.parse().unwrap()),
            ),
            ["cap", pin, low, high] => {
                machine.bus.periph.rtc.touch_caps[pin.parse::<usize>().unwrap()] =
                    [low.parse().unwrap(), high.parse().unwrap()];
            }
            ["touch", "timeout", value] => {
                machine.bus.periph.rtc.touch_timeout = *value == "1";
            }
            ["touch", "hang", value] => {
                machine.bus.periph.rtc.touch_hang = *value == "1";
            }
            ["diode", "report", name] => {
                let state = inputs.lock().unwrap();
                let touch_scans = machine.bus.periph.rtc.touch_scans;
                let touch_timer = machine.bus.periph.rtc.read(0x10c) & (1 << 13) != 0;
                let touch_mask = (machine.bus.periph.rtc.read(0x110) >> 10) & 0x7fff;
                std::fs::write(
                    out.join(name),
                    format!(
                        "high_drives={}\nreboots={}\noutput_enabled={}\ntouch_scans={}\ntouch_timer={}\ntouch_mask={}\n",
                        state.diode_high_drives,
                        machine.reboots,
                        machine.bus.periph.gpio.enable & (1 << 5) != 0,
                        touch_scans,
                        touch_timer,
                        touch_mask,
                    ),
                )
                .unwrap();
            }
            ["serial", text] => machine.bus.serial_input(text.as_bytes()),
            ["vbus", millivolts] => {
                inputs.lock().unwrap().vbus_mv = Some(millivolts.parse().unwrap());
            }
            ["board", name] => {
                let rtc = &mut machine.bus.periph.rtc;
                let state = format!(
                    "{}\nrtc_wakeup={}\next1_mask={}\nmain_rtc_mux={}\nside_rtc_mux={}\n",
                    machine.bus.board.report(),
                    rtc.read(0x130),
                    rtc.read(0xe0) & 0x3fffff,
                    (rtc.read(0x484 + 11 * 4) >> 19) & 1,
                    (rtc.read(0x484 + 12 * 4) >> 19) & 1
                );
                std::fs::write(out.join(name), state).unwrap();
            }
            ["audio-clear"] => {
                machine.bus.periph.i2s0.pcm.clear();
                machine.bus.periph.i2s1.pcm.clear();
            }
            ["audio", name] => {
                let (samples, rate) = machine.bus.audio();
                let bytes: Vec<u8> = samples
                    .iter()
                    .flat_map(|sample| sample.to_le_bytes())
                    .collect();
                std::fs::write(out.join(name), bytes).unwrap();
                std::fs::write(out.join(format!("{name}.rate")), rate.to_string()).unwrap();
            }
            ["peer", "start"] => {
                inputs.lock().unwrap().peer = Some(peer::OpticalPeer::new());
            }
            ["peer", "echo", enabled] => {
                inputs.lock().unwrap().local_echo = *enabled == "on";
            }
            ["peer", "start", baud, turnaround] => {
                inputs.lock().unwrap().peer = Some(
                    peer::OpticalPeer::new()
                        .timing(baud.parse().unwrap(), turnaround.parse().unwrap()),
                );
            }
            ["peer", "put", path] => {
                inputs.lock().unwrap().peer =
                    Some(peer::OpticalPeer::with_put(std::fs::read(path).unwrap()));
            }
            ["peer", "put", path, baud, turnaround] => {
                inputs.lock().unwrap().peer = Some(
                    peer::OpticalPeer::with_put(std::fs::read(path).unwrap())
                        .timing(baud.parse().unwrap(), turnaround.parse().unwrap()),
                );
            }
            ["peer", "put-compressed", path, baud, turnaround] => {
                inputs.lock().unwrap().peer = Some(
                    peer::OpticalPeer::with_compressed_put(std::fs::read(path).unwrap())
                        .timing(baud.parse().unwrap(), turnaround.parse().unwrap()),
                );
            }
            ["peer", "walker", path] => {
                inputs.lock().unwrap().peer = Some(
                    peer::OpticalPeer::with_walker(std::fs::read(path).unwrap())
                        .timing(115760, 5000),
                );
            }
            ["peer", mode @ ("write-page" | "read-page"), address] => {
                let data = (0..128).map(|index| (index as u8) ^ 0x5a).collect();
                inputs.lock().unwrap().peer = Some(
                    peer::OpticalPeer::with_page(
                        address.parse().unwrap(),
                        data,
                        *mode == "write-page",
                    )
                    .unwrap(),
                );
            }
            ["peer", "stop"] => {
                inputs.lock().unwrap().peer = None;
                machine.bus.gpio_set_input(5, true);
            }
            ["peer", "report", name] => {
                let state = inputs.lock().unwrap();
                let peer = state.peer.as_ref().expect("peer has not been started");
                std::fs::write(
                    out.join(name),
                    format!(
                        "complete={} error={:?}\nflash_erases_during_reply_window={}\n{}",
                        peer.complete(),
                        peer.error(),
                        state.reply_window_erases,
                        peer.report()
                    ),
                )
                .unwrap();
            }
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
