mod clock;
mod energy;
mod gameplay;
mod management;
mod optics;
mod peer;
mod recovery;
mod speaker;
mod walkers;

use emu_core::bus::Bus;
use speaker::Speaker;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use esp_periph::i2c::I2cDevice;
use esp_soc::board::{BoardEdge, BoardModel, SpiPins};
use esp_soc::devices::dcs_panel::DcsPanel;
use esp_soc::{SocBus, Stop};
use optics::{Optics, PathModel, TornWrite};

const CYCLES_PER_MS: u64 = 240_000;
const FLASH_BYTES: usize = 8 << 20;
const APP: usize = 0x10000;
const SAVE: usize = 0x610000;
const PM_IRQ: u8 = 13;

struct Pm {
    registers: [u8; 256],
    battery_mv: u16,
    battery_sample_mv: u16,
    next_battery_sample: u64,
    usb: bool,
    external_5v: bool,
    boost_conflict: bool,
    boost_started: Option<u64>,
    power: bool,
    pressed: bool,
    press_started: u64,
    last_release: Option<u64>,
    reset_requested: bool,
    power_cut: bool,
    codec_cold: bool,
    codec_reset: bool,
    l3b_ready_at: u64,
    codec_unpowered_accesses: u32,
    led_flashes: u32,
    key_reads: u64,
    i2c_transactions: u64,
    last_key_read: u64,
    longest_key_gap: u64,
    now: u64,
    watchdog_at: Option<u64>,
    download_at: Option<u64>,
    boot_low_until: u64,
    download_mode: bool,
    resets: u32,
    front: bool,
    side: bool,
    speaker: Rc<RefCell<Speaker>>,
}

impl Pm {
    fn new(battery_mv: u16, usb: bool, locked: bool, speaker: Rc<RefCell<Speaker>>) -> Self {
        let mut registers = [0; 256];
        registers[0..4].copy_from_slice(&[0x50, 0x20, 5, 6]);
        registers[6] = 0x17;
        // 0x40 is PM1's documented 2.50 V default cutoff. Firmware keeps
        // its own reserve above the hardware threshold.
        registers[8] = 0x40;
        registers[0x49] = 0x2a | (u8::from(locked) << 7);
        Self {
            registers,
            battery_mv,
            battery_sample_mv: battery_mv,
            next_battery_sample: 0,
            usb,
            external_5v: false,
            boost_conflict: false,
            boost_started: None,
            power: false,
            pressed: false,
            press_started: 0,
            last_release: None,
            reset_requested: false,
            power_cut: false,
            codec_cold: true,
            codec_reset: false,
            l3b_ready_at: 0,
            codec_unpowered_accesses: 0,
            led_flashes: 0,
            key_reads: 0,
            i2c_transactions: 0,
            last_key_read: 0,
            longest_key_gap: 0,
            now: 0,
            watchdog_at: None,
            download_at: None,
            boot_low_until: 0,
            download_mode: false,
            resets: 0,
            front: false,
            side: false,
            speaker,
        }
    }

    fn read(&mut self, register: u8) -> u8 {
        match register {
            4 => 4 | u8::from(self.usb) | (u8::from(self.external_5v && !self.boost_on()) << 1),
            0x22..=0x23 => self.battery_sample_mv.to_le_bytes()[usize::from(register - 0x22)],
            0x24..=0x25 => {
                if self.usb {
                    [0x88, 0x13][usize::from(register - 0x24)]
                } else {
                    0
                }
            }
            0x48 => {
                self.key_reads += 1;
                self.longest_key_gap = self.longest_key_gap.max(self.now - self.last_key_read);
                self.last_key_read = self.now;
                u8::from(self.power) | (u8::from(std::mem::take(&mut self.pressed)) << 7)
            }
            0x26 | 0x27 => {
                let mv = self.rail_mv();
                if register == 0x26 {
                    mv as u8
                } else {
                    (mv >> 8) as u8
                }
            }
            _ => self.registers[usize::from(register)],
        }
    }

    fn write(&mut self, register: u8, mut value: u8) {
        if register == 0x49 {
            value |= self.registers[0x49] & 0x80;
        }
        let rail_was_on = self.rail();
        let cell = &mut self.registers[usize::from(register)];
        if matches!(register, 0x40..=0x42) {
            *cell &= value; // M5PM1 interrupt status is write-zero-to-clear.
        } else {
            *cell = value;
        }
        if register == 0x06 {
            if self.boost_on() {
                self.boost_started.get_or_insert(self.now);
            } else {
                self.boost_started = None;
            }
        }
        if register == 0x06 && self.external_5v && self.boost_on() {
            self.boost_conflict = true;
        }
        if !rail_was_on && self.rail() {
            self.l3b_ready_at = self.now + 10 * CYCLES_PER_MS;
        }
        if rail_was_on && !self.rail() {
            self.speaker.borrow_mut().codec.fill(0);
            self.codec_cold = true;
            self.codec_reset = false;
        }
        if matches!(register, 0x10 | 0x11 | 0x16) {
            let mut speaker = self.speaker.borrow_mut();
            if self.amp_on() {
                speaker.enabled_at.get_or_insert(self.now);
            } else {
                speaker.enabled_at = None;
            }
        }
        if register == 0x0a || register == 0x0b && value == 0xa5 {
            assert!(
                register != 0x0a || value != 0,
                "Firmware disabled the PM1 watchdog"
            );
            let seconds = u64::from(self.registers[0x0a]);
            self.watchdog_at = (seconds != 0).then_some(self.now + seconds * 1000 * CYCLES_PER_MS);
        }
        if register == 0x0c {
            match value {
                0xa1 => {
                    self.power_cut = true;
                    self.registers[0x06] &= !0x1e;
                    self.registers[0x10..=0x16].fill(0);
                    self.speaker.borrow_mut().enabled_at = None;
                }
                0xa2 => self.reset_requested = true,
                // HW5/SW6 dispatches SYS_CMD=3 to 0x080031d4: DL_LOCK
                // guards this entry too, not just the physical hold gesture.
                0xa3 if self.registers[0x49] & 0x80 == 0 => {
                    self.download_at = Some(self.now);
                }
                _ => {}
            }
        }
        if self.registers[0x16] & 0x55 == 0 && self.registers[0x17] & 1 == 0 {
            self.registers[0x40..=0x42].fill(0);
        }
    }

    fn irq(&self) -> bool {
        self.registers[0x16] & 0x0c == 4
            && (0..3).any(|bank| self.registers[0x40 + bank] & !self.registers[0x43 + bank] != 0)
    }

    fn reset_due(&mut self) -> bool {
        // Stop the CPU at the same board boundary as a physical power reset;
        // Rig keeps it off until a PM1 power-on event occurs.
        if self.power_cut { return true; }
        let download = self
            .download_at
            .is_some_and(|deadline| self.now >= deadline);
        let button_reset = std::mem::take(&mut self.reset_requested);
        if !download
            && !button_reset
            && !self
                .watchdog_at
                .is_some_and(|deadline| self.now >= deadline)
        {
            return false;
        }
        if download {
            self.download_at = None;
            self.boot_low_until = self.now + 500 * CYCLES_PER_MS;
        }
        self.download_mode = download;
        self.resets += 1;
        self.watchdog_at = None;
        self.registers[0x06] = 0x17;
        self.registers[0x0a..=0x0b].fill(0);
        self.registers[0x10..=0x16].fill(0);
        self.registers[0x40..=0x42].fill(0);
        if button_reset {
            self.registers[0x05] = 1 << 3;
        } else {
            self.registers[0x05] = 0;
        }
        let mut speaker = self.speaker.borrow_mut();
        speaker.enabled_at = None;
        speaker.codec.fill(0);
        self.codec_cold = true;
        self.codec_reset = false;
        true
    }

    fn set_power_key(&mut self, down: bool) {
        self.power = down;
        if down && self.power_cut {
            self.power_cut = false;
            self.reset_requested = true;
            return;
        }
        let keys = self.registers[0x49];
        if down {
            self.press_started = self.now;
            self.pressed = true;
            if keys & 0x80 == 0 && !self.download_mode {
                let seconds = 1 + u64::from((keys >> 3) & 3);
                self.download_at = Some(self.now + seconds * 1000 * CYCLES_PER_MS);
            }
            return;
        }
        self.download_at = None;
        let single_window = (125 * CYCLES_PER_MS) << ((keys >> 1) & 3);
        let double_window = (125 * CYCLES_PER_MS) << ((keys >> 5) & 3);
        if self.now - self.press_started > single_window {
            self.last_release = None;
            return;
        }
        // M5PM1-O (identity 50 20 05 4f) resets on a qualified click in download mode,
        // even if the same click is routed to the application in normal mode.
        if keys & 1 == 0 || self.download_mode {
            self.reset_requested = true;
        } else {
            self.registers[0x42] |= 1;
            if self.registers[0x16] & 0x0c == 4 {
                self.led_flashes += 1;
            }
        }
        if self
            .last_release
            .is_some_and(|last| self.now - last < double_window)
        {
            if self.registers[0x4a] & 1 == 0 {
                self.power_cut = true;
            } else {
                self.registers[0x42] |= 4;
                // PM1 SW6 selects its feedback mode from GPIO IRQ enable,
                // independently of the per-event interrupt masks.
                if self.registers[0x16] & 0x0c == 4 {
                    self.led_flashes += 1;
                }
            }
        }
        self.last_release = Some(self.now);
    }

    fn amp_on(&self) -> bool {
        self.registers[0x10] & self.registers[0x11] & 8 != 0 && self.registers[0x16] & 0xc0 == 0
    }

    fn rail_mv(&self) -> u16 {
        if self.external_5v {
            return 5000;
        }
        self.boost_started.map_or(0, |start| {
            ((self.now - start) * 5000 / (100 * CYCLES_PER_MS)).min(5000) as u16
        })
    }

    fn boost_on(&self) -> bool {
        self.registers[0x06] & 8 != 0
    }

    fn rail(&self) -> bool {
        self.registers[0x10] & self.registers[0x11] & 4 != 0 && self.registers[0x16] & 0x30 == 0
    }
}

enum Chip {
    Power(Rc<RefCell<Pm>>),
    Motion(Rc<RefCell<Motion>>, Rc<RefCell<Pm>>),
    Codec(Rc<RefCell<Pm>>),
}

struct Motion {
    sample: [i16; 3],
    config_bytes: usize,
    status: u8,
    rate: u16,
    accelerometer: bool,
    available_at: u64,
    advanced_power_save: bool,
    access_after: u64,
    timing_violation: bool,
    fresh: bool,
    output: [i16; 3],
    fifo: VecDeque<u8>,
    fifo_enabled: bool,
    fifo_stop: bool,
    fifo_downs: u8,
    fifo_phase: u32,
    fifo_overflows: u64,
    clock_ppm: i32,
    clock_latch: u32,
    length_latch: usize,
    transactions: u64,
    bytes: u64,
}

impl Motion {
    fn sensor_time(&self, now: u64) -> u32 {
        ((now as u128 * 25_600 * (1_000_000 + self.clock_ppm) as u128
            / (240_000_000u128 * 1_000_000))
            & 0xffffff) as u32
    }

    fn period(&self) -> u64 {
        240_000_000u64 * 1_000_000 / (u64::from(self.rate) * (1_000_000 + self.clock_ppm) as u64)
    }

    fn advance(&mut self, now: u64) {
        if !self.accelerometer || self.rate == 0 {
            return;
        }
        while self.available_at <= now {
            self.output = self.sample;
            self.fresh = true;
            if self.fifo_enabled && self.fifo_phase % (1 << self.fifo_downs) == 0 {
                if self.fifo.len() + 6 > 2048 {
                    self.fifo_overflows += 1;
                    if !self.fifo_stop {
                        for _ in 0..6 {
                            self.fifo.pop_front();
                        }
                    }
                }
                if self.fifo.len() + 6 <= 2048 {
                    for axis in self.output {
                        self.fifo.extend(axis.to_le_bytes());
                    }
                }
            }
            self.fifo_phase += 1;
            self.available_at += self.period();
        }
    }
}

struct Device {
    chip: Chip,
    register: u8,
    addressing: bool,
    write_guard_us: u64,
}

impl Device {
    fn new(chip: Chip) -> Self {
        Self {
            chip,
            register: 0,
            addressing: true,
            write_guard_us: 0,
        }
    }
}

impl I2cDevice for Device {
    fn pins(&self) -> Option<(u8, u8)> {
        Some((47, 48))
    }

    fn start(&mut self, read: bool) -> bool {
        self.addressing = !read;
        if let Chip::Power(pm) = &self.chip {
            pm.borrow_mut().i2c_transactions += 1;
        }
        match &self.chip {
            Chip::Codec(pm) => pm.borrow().rail(),
            Chip::Motion(imu, pm) => {
                let mut imu = imu.borrow_mut();
                imu.advance(pm.borrow().now);
                imu.transactions += 1;
                let ready = pm.borrow().now >= imu.access_after;
                imu.timing_violation |= !ready;
                ready
            }
            _ => true,
        }
    }

    fn write(&mut self, value: u8) -> bool {
        if let Chip::Motion(imu, _) = &self.chip {
            imu.borrow_mut().bytes += 1;
        }
        if self.addressing {
            self.register = value;
            self.addressing = false;
            return true;
        }
        match &mut self.chip {
            Chip::Power(pm) => pm.borrow_mut().write(self.register, value),
            Chip::Motion(imu, pm) => {
                let mut imu = imu.borrow_mut();
                self.write_guard_us =
                    self.write_guard_us
                        .max(if imu.advanced_power_save { 450 } else { 2 });
                match self.register {
                    0x5e => {
                        imu.config_bytes += 1;
                        return true;
                    }
                    0x59 if value == 1 && imu.config_bytes == 8192 => imu.status = 1,
                    0x7e if value == 0xb6 => {
                        imu.advanced_power_save = true;
                        self.write_guard_us = 450;
                        imu.config_bytes = 0;
                        imu.status = 0;
                        imu.accelerometer = false;
                        imu.fresh = false;
                        imu.fifo.clear();
                        imu.fifo_enabled = false;
                    }
                    0x7e if value == 0xb0 => imu.fifo.clear(),
                    0x45 => imu.fifo_downs = (value >> 4) & 7,
                    0x48 => imu.fifo_stop = value & 1 != 0,
                    0x49 => {
                        imu.fifo_enabled = value & 0x40 != 0;
                        imu.fifo.clear();
                        imu.fifo_phase = 0;
                    }
                    0x7c => {
                        imu.advanced_power_save = value & 1 != 0;
                        if imu.advanced_power_save {
                            self.write_guard_us = 450;
                        }
                    }
                    0x40 => {
                        imu.rate = match value & 0x0f {
                            6 => 25,
                            7 => 50,
                            8 => 100,
                            9 => 200,
                            _ => 0,
                        };
                        imu.available_at = (pm.borrow().now / imu.period() + 1) * imu.period();
                    }
                    0x7d => {
                        imu.accelerometer = value & 4 != 0;
                        imu.available_at = ((pm.borrow().now + 2 * CYCLES_PER_MS) / imu.period()
                            + 1)
                            * imu.period();
                        imu.fifo.clear();
                        imu.fresh = false;
                    }
                    _ => {}
                }
            }
            Chip::Codec(pm) => {
                let mut pm = pm.borrow_mut();
                if !pm.rail() || pm.now < pm.l3b_ready_at {
                    pm.codec_unpowered_accesses += 1;
                    return false;
                }
                if std::mem::take(&mut pm.codec_cold) {
                    return false;
                }
                {
                    if self.register == 0 && value & 0x1f == 0x1f {
                        pm.codec_reset = true;
                    }
                    if pm.codec_reset {
                        pm.speaker.borrow_mut().codec[usize::from(self.register)] = value;
                    }
                }
            }
        }
        self.register = self.register.wrapping_add(1);
        true
    }

    fn stop(&mut self) {
        if let Chip::Motion(imu, pm) = &self.chip {
            if self.write_guard_us != 0 {
                imu.borrow_mut().access_after = pm.borrow().now + self.write_guard_us * 240;
                self.write_guard_us = 0;
            }
        }
    }

    fn read(&mut self) -> u8 {
        let register = self.register;
        self.register = self.register.wrapping_add(1);
        match &self.chip {
            Chip::Power(pm) => pm.borrow_mut().read(register),
            Chip::Motion(imu, pm) => {
                let mut imu = imu.borrow_mut();
                let now = pm.borrow().now;
                imu.advance(now);
                imu.bytes += 1;
                match register {
                    0 => 0x24,
                    3 => {
                        if imu.fresh {
                            0x80
                        } else {
                            0
                        }
                    }
                    0x21 => imu.status,
                    0x18..=0x1a => {
                        if register == 0x18 {
                            imu.clock_latch = imu.sensor_time(now);
                        }
                        (imu.clock_latch >> ((register - 0x18) * 8)) as u8
                    }
                    0x24 => {
                        imu.length_latch = imu.fifo.len();
                        imu.length_latch as u8
                    }
                    0x25 => (imu.length_latch >> 8) as u8,
                    0x26 => {
                        self.register = 0x26;
                        imu.timing_violation |= imu.advanced_power_save;
                        imu.fifo.pop_front().unwrap_or(0x80)
                    }
                    0x0c..=0x11 => {
                        let i = usize::from(register - 0x0c);
                        if i == 5 {
                            imu.fresh = false;
                        }
                        imu.output[i / 2].to_le_bytes()[i % 2]
                    }
                    _ => 0,
                }
            }
            Chip::Codec(pm) => pm.borrow().speaker.borrow().codec[usize::from(register)],
        }
    }
}

struct Panel {
    command: u8,
    lcmctrl: u8,
    lcd: DcsPanel,
    dc: bool,
    reset: bool,
    rail: bool,
    sleep_out: Option<u64>,
    premature: bool,
    shown_at: Option<u64>,
}

impl Panel {
    fn uninitialized() -> DcsPanel {
        let mut lcd = DcsPanel::st7789();
        lcd.gram.fill(0xdead);
        lcd
    }

    fn new() -> Self {
        Self {
            command: 0,
            lcmctrl: 0x2c,
            lcd: Self::uninitialized(),
            dc: false,
            reset: false,
            rail: false,
            sleep_out: None,
            premature: false,
            shown_at: None,
        }
    }

    fn synchronize(&mut self, powered: bool) {
        if self.rail && !powered {
            self.lcd = Self::uninitialized();
            self.lcmctrl = 0x2c;
            self.sleep_out = None;
            self.shown_at = None;
        }
        self.rail = powered;
    }

    fn send(&mut self, now: u64, bytes: &[u8]) {
        if !self.rail || !self.reset {
            return;
        }
        self.lcd.dc = self.dc;
        if self.dc {
            if self.command == 0xc0 {
                self.lcmctrl = bytes[0];
            }
            for &byte in bytes {
                self.lcd.byte(byte);
            }
        } else {
            for &cmd in bytes {
                self.command = cmd;
                if cmd == 0x11 {
                    self.sleep_out = Some(now);
                }
                if cmd == 0x10 || cmd == 0x28 {
                    self.shown_at = None;
                }
                if cmd == 0x29 {
                    if !self
                        .sleep_out
                        .is_some_and(|at| now >= at + 120 * CYCLES_PER_MS)
                    {
                        self.premature = true;
                        continue;
                    }
                    // M5GFX StickS3: 135x240 glass at (52,40) in 240x320 RAM.
                    for row in 40..280 {
                        if self.lcd.gram[row * 240 + 52..row * 240 + 187].contains(&0xdead) {
                            self.premature = true;
                        }
                    }
                    self.shown_at = Some(now);
                }
                self.lcd.byte(cmd);
            }
        }
    }

    fn image(&self, lit: bool) -> Vec<u16> {
        if !lit || !self.rail || self.shown_at.is_none() {
            return vec![0; 240 * 135];
        }
        let reverse = self.lcd.madctl & 0x80 != 0;
        (0..135)
            .flat_map(|y| {
                (0..240).map(move |x| {
                    let (column, row) = if reverse {
                        (52 + y, 279 - x)
                    } else {
                        (186 - y, 40 + x)
                    };
                    let pixel = self.lcd.gram[row * 240 + column];
                    // ST7789P3 LCMCTRL.XBGR XORs MADCTL.RGB. StickS3 glass
                    // uses BGR source order (M5GFX panel configuration).
                    if (self.lcd.madctl & 8 != 0) ^ (self.lcmctrl & 0x20 != 0) {
                        pixel
                    } else {
                        (pixel & 0x07e0) | ((pixel & 31) << 11) | ((pixel >> 11) & 31)
                    }
                })
            })
            .collect()
    }
}

struct Stick {
    pm: Rc<RefCell<Pm>>,
    panel: Rc<RefCell<Panel>>,
    imu: Rc<RefCell<Motion>>,
    optics: Rc<RefCell<Optics>>,
    now: u64,
    irq: bool,
    edges: Vec<BoardEdge>,
    energy: Rc<RefCell<energy::Components>>,
}

impl BoardModel for Stick {
    fn name(&self) -> &'static str {
        "StickS3"
    }
    fn uses_spi_pins(&self) -> bool {
        true
    }
    fn uses_gpio_waveform(&self) -> bool {
        self.optics.borrow().timed
    }

    fn gpio_output_at(&mut self, cycle: u64, _changes: &[(u8, bool)], enabled: u64, output: u64) {
        let mut panel = self.panel.borrow_mut();
        panel.dc = output & (1 << 45) != 0;
        panel.reset = output & (1 << 21) != 0;
        self.optics.borrow_mut().drive(
            cycle,
            (enabled & (1 << 5) != 0).then_some(output & (1 << 5) != 0),
        );
    }

    fn pad_access_cycles(&mut self, cycle: u64, pin: u8) -> u32 {
        if pin == 5 {
            self.optics.borrow_mut().stall(cycle)
        } else {
            0
        }
    }

    fn pad_pull_at(&mut self, cycle: u64, pin: u8, pull: bool) {
        if pin == 5 {
            self.optics.borrow_mut().set_pull(cycle, pull);
        }
    }

    fn pad_input_at(&mut self, cycle: u64, pin: u8, pull: bool) -> Option<bool> {
        (pin == 5).then(|| self.optics.borrow_mut().high(cycle, pull))
    }

    fn rmt_symbol_started(&mut self, at: u64, pin: u8, word: u32, tick: u32) {
        if pin == 46 && self.pm.borrow().rail_mv() >= 4500 {
            self.optics.borrow().forward_symbol(at, word, tick);
        }
    }

    fn rmt_symbols(&mut self, end: u64, pin: u8, words: &[u32], tick: u32) {
        let pm = self.pm.borrow();
        if pin == 46 && pm.rail_mv() >= 4500 {
            self.optics.borrow_mut().transmit(end, words, tick);
        }
    }

    fn spi_transfer_pins(&mut self, _host: u8, pins: SpiPins, tx: &[u8], rx_len: usize) -> Vec<u8> {
        let mut panel = self.panel.borrow_mut();
        panel.synchronize(self.pm.borrow().rail());
        if pins.cs & (1 << 41) != 0 && pins.sclk & (1 << 40) != 0 && pins.mosi & (1 << 39) != 0 {
            let mut optics = self.optics.borrow_mut();
            // SPI is delivered after its wire time. A GPIO sample inside that
            // interval means LCD traffic overlapped physical IR acquisition.
            let start = self.now.saturating_sub(tx.len() as u64 * 8 * 6);
            if optics.last_sample_cycle > start && optics.last_sample_cycle <= self.now {
                optics.lcd_overlap = true;
            }
            panel.send(self.now, tx);
        }
        vec![0xff; rx_len]
    }

    fn flash_power_cut(&mut self, offset: usize, data: &[u8]) -> Option<usize> {
        let mut optics = self.optics.borrow_mut();
        if offset >= SAVE
            && optics.replies.len() >= 20
            && optics
                .cut
                .as_ref()
                .is_some_and(|cut| cut.matches(optics.commit_received, data))
        {
            optics.cut = None;
            optics.power_cut = true;
            return Some(data.len() / 2);
        }
        None
    }

    fn take_power_reset(&mut self) -> bool {
        let mut optics = self.optics.borrow_mut();
        if matches!(optics.cut, Some(optics::TornWrite::Upload)) && optics.replies.len() >= 20 {
            optics.cut = None;
            optics.power_cut = true;
            return true;
        }
        self.pm.borrow_mut().reset_due()
    }

    fn next_deadline(&self) -> Option<u64> {
        let pm = self.pm.borrow();
        [pm.watchdog_at, pm.download_at].into_iter().flatten().min()
    }

    fn advance_to(&mut self, cycle: u64) {
        self.energy
            .borrow_mut()
            .advance(cycle - self.now, &self.pm.borrow(), &self.imu.borrow());
        self.now = cycle;
        self.imu.borrow_mut().advance(cycle);
        let mut pm = self.pm.borrow_mut();
        pm.now = cycle;
        if cycle >= pm.next_battery_sample {
            pm.battery_sample_mv = pm.battery_mv;
            pm.next_battery_sample = cycle + 1000 * CYCLES_PER_MS;
        }
        self.panel.borrow_mut().synchronize(pm.rail());
        let irq = pm.irq();
        drop(pm);
        if self.irq != irq {
            self.irq = irq;
            self.edges.push(BoardEdge {
                cycle,
                pin: PM_IRQ,
                level: !irq,
            });
        }
    }

    fn input_levels(&self) -> Vec<(u8, bool)> {
        let pm = self.pm.borrow();
        vec![
            (0, pm.now >= pm.boot_low_until),
            (11, !pm.front),
            (12, !pm.side),
            (13, !pm.irq()),
        ]
    }

    fn take_edges(&mut self) -> Vec<BoardEdge> {
        std::mem::take(&mut self.edges)
    }

    fn i2c_devices(&mut self) -> Vec<(u8, u8, Box<dyn I2cDevice>)> {
        vec![
            (1, 0x6e, Box::new(Device::new(Chip::Power(self.pm.clone())))),
            (
                1,
                0x68,
                Box::new(Device::new(Chip::Motion(self.imu.clone(), self.pm.clone()))),
            ),
            (1, 0x18, Box::new(Device::new(Chip::Codec(self.pm.clone())))),
        ]
    }
}

struct Rig {
    reference_wake: u64,
    speaker: Rc<RefCell<Speaker>>,
    machine: esp32s3::soc::Machine,
    pm: Rc<RefCell<Pm>>,
    panel: Rc<RefCell<Panel>>,
    imu: Rc<RefCell<Motion>>,
    optics: Rc<RefCell<Optics>>,
    energy: Rc<RefCell<energy::Components>>,
}

impl Rig {
    fn new(rom: &[u8], image: &[u8], usb: bool, locked: bool, mv: u16) -> Result<Self, String> {
        let optics = Rc::new(RefCell::new(Optics::new()));
        let speaker = Rc::new(RefCell::new(Speaker::new(55)));
        let pm = Rc::new(RefCell::new(Pm::new(mv, usb, locked, speaker.clone())));
        let panel = Rc::new(RefCell::new(Panel::new()));
        let imu = Rc::new(RefCell::new(Motion {
            sample: [0, 0, 16384],
            config_bytes: 0,
            status: 0,
            rate: 0,
            accelerometer: false,
            available_at: 0,
            advanced_power_save: true,
            access_after: 0,
            timing_violation: false,
            fresh: false,
            output: [0; 3],
            fifo: VecDeque::new(),
            fifo_enabled: false,
            fifo_stop: false,
            fifo_downs: 0,
            fifo_phase: 0,
            fifo_overflows: 0,
            clock_ppm: env::var("STICK_IMU_PPM")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(0),
            clock_latch: 0,
            length_latch: 0,
            transactions: 0,
            bytes: 0,
        }));
        let mut mac = [2, 5, 0, 0, 0, 1];
        if let Ok(value) = env::var("STICK_MAC") {
            let bytes: Vec<u8> = value
                .split(':')
                .map(|byte| u8::from_str_radix(byte, 16))
                .collect::<Result<_, _>>()
                .map_err(|e| e.to_string())?;
            mac = bytes
                .try_into()
                .map_err(|_| "STICK_MAC needs six hex bytes")?;
        }
        let mut machine = esp32s3::soc::machine(mac);
        machine.bus.set_psram_size(8 << 20)?;
        machine.dbg.stop_on_unimplemented = true;
        machine.load_rom(rom)?;
        machine.console.capture = true;
        machine.console.mask = 3;

        machine.write_flash(0, image)?;
        // GD25Q64E Rev 1.6, table 9.5 (-40..85 C), per command length.
        let maximum = env::var("STICK_TIMING").unwrap_or_default() == "maximum";
        machine.bus.flash_first_cycles = if maximum { 70 } else { 40 } * 240;
        machine.bus.flash_next_cycles = if maximum { 12 * 240 } else { 600 };
        machine.bus.flash_sector_cycles = if maximum { 300_000 } else { 45_000 } * 240;
        machine.bus.flash_block_cycles = if maximum { 1_600_000 } else { 250_000 } * 240;
        // Pico package field used by the SDK's SPI flash initialization.
        machine.bus.periph.efuse.ram.write(0x50, 1 << 21);
        machine.bus.set_reset_cause(1);
        machine.bus.periph.usb.connected = usb;
        machine.bus.set_strap(0x0f);
        machine.bus.gpio_set_input(0, true);
        let energy = Rc::new(RefCell::new(energy::Components::default()));
        machine.bus.board = Box::new(Stick {
            pm: pm.clone(),
            panel: panel.clone(),
            imu: imu.clone(),
            optics: optics.clone(),
            now: 0,
            irq: false,
            edges: Vec::new(),
            energy: energy.clone(),
        });
        machine.bus.attach_board_devices();
        machine.boot_rom();
        Ok(Self {
            reference_wake: 0,
            energy,
            speaker,
            machine,
            pm,
            panel,
            imu,
            optics,
        })
    }

    fn run(&mut self, ms: u64) -> Result<(), String> {
        if self.pm.borrow().power_cut {
            return Ok(());
        }
        // Differential reference: an early RTC timer wake services the same
        // FIFO at the game's 16 Hz cadence while the motion trace advances.
        if env::var_os("STICK_MOTION_REFERENCE").is_some() && self.machine.bus.sleeping
            && self.machine.bus.cycles() >= self.reference_wake
        {
            let period = 62_500 * 240;
            self.reference_wake = (self.machine.bus.cycles() / period + 1) * period;
            let rtc = &mut self.machine.bus.periph.rtc;
            rtc.ram.write(0x04, rtc.slow_ticks as u32);
            rtc.ram
                .write(0x08, (rtc.slow_ticks >> 32) as u32 | (1 << 16));
        }
        let end = self.machine.bus.cycles() + ms * CYCLES_PER_MS;
        let clocked = env::var_os("STICK_CONNECTION").is_some();
        self.machine.max_cycles = if clocked {
            end.min(self.machine.bus.cycles() + CYCLES_PER_MS)
        } else {
            end
        };
        loop {
            match self.machine.run(u64::MAX) {
                Stop::Halted if self.machine.bus.cycles() >= self.machine.max_cycles => {
                    if clocked {
                        self.optics
                            .borrow_mut()
                            .advance_peer(self.machine.bus.cycles())?;
                    }
                    if self.machine.bus.cycles() >= end {
                        break;
                    }
                    self.machine.max_cycles = end.min(self.machine.bus.cycles() + CYCLES_PER_MS);
                }
                Stop::SwReset => {
                    if self.pm.borrow().power_cut {
                        break;
                    }
                    if !self.pm.borrow().usb
                        && !self.pm.borrow().download_mode
                        && self
                            .machine
                            .bus
                            .read32(0x6000_812c)
                            .map_err(|e| format!("{e:?}"))?
                            & 1
                            != 0
                    {
                        return Err(
                            "Software forced the downloader on battery outside PM1 download mode"
                                .into(),
                        );
                    }
                    self.machine.reboot();
                }
                other => return Err(format!("{other:?} at {:.6}s", self.machine.seconds())),
            }
        }
        if self.machine.bus.short_flash_sleep {
            return Err("Wake reversed VDD_SPI before the 2 ms flash power-down interval".into());
        }
        if self.optics.borrow().lcd_overlap {
            return Err("LCD SPI traffic overlapped optical sampling".into());
        }
        if self.imu.borrow().timing_violation {
            return Err("BMI270 accessed before its post-write settling time".into());
        }
        if self.pm.borrow().led_flashes != 0 {
            return Err("PM1 flashed its button-feedback LED".into());
        }
        if self.machine.bus.periph.usb.connected && self.machine.bus.sleeping {
            return Err("Firmware disabled USB reachability by sleeping on USB power".into());
        }
        Ok(())
    }

    fn usb_power(&mut self, connected: bool, battery_mv: u16) {
        let mut pm = self.pm.borrow_mut();
        pm.usb = connected;
        pm.battery_mv = battery_mv;
        if connected && pm.power_cut {
            pm.power_cut = false;
            pm.reset_requested = true;
        }
        // PM1 IRQ status 0x41 bits 0/1 are USB VIN insertion/removal.
        pm.registers[0x41] |= if connected { 1 << 0 } else { 1 << 1 };
        self.machine.bus.periph.usb.connected = connected;
    }

    fn connect_downloader_over_usb(&mut self) -> Result<(), String> {
        // esptool's USBJTAGSerialReset, ESP32-S3 TRM Table 33.4-3.
        self.machine.bus.usb_control_lines(false, false);
        self.machine.bus.usb_control_lines(true, false);
        self.machine.bus.usb_control_lines(true, true);
        self.machine.bus.usb_control_lines(false, true);
        self.run(180)?;
        self.machine.bus.usb_control_lines(false, false);
        Ok(())
    }

    fn stall_until_watchdog(&mut self) -> Result<(), String> {
        let pm = self.pm.borrow();
        let deadline = pm.watchdog_at.ok_or("PM1 watchdog was not armed")?;
        let resets = pm.resets;
        drop(pm);
        while self.machine.bus.cycles() < deadline {
            let remaining = deadline - self.machine.bus.cycles();
            let cycles = self
                .machine
                .bus
                .cpu_cycles_for_bus(remaining.min(1000 * CYCLES_PER_MS));
            self.machine.bus.tick(cycles as u32);
        }
        self.machine.bus.flush_ticks();
        if self.pm.borrow().resets != resets + 1 || !self.machine.bus.sw_reset() {
            return Err("PM1 failed to reset the hung processor".into());
        }
        self.machine.reboot();
        self.run(1200)
    }

    fn run_speaker(&mut self, ms: u64) -> Result<(), String> {
        for _ in 0..ms {
            self.run(1)?;
            speaker::observe(self);
        }
        Ok(())
    }

    fn switch(&mut self, key: &str, down: bool) {
        match key {
            "power" => {
                self.pm.borrow_mut().set_power_key(down);
            }
            "front" | "side" => {
                let mut pm = self.pm.borrow_mut();
                let pin = if key == "front" {
                    pm.front = down;
                    11
                } else {
                    pm.side = down;
                    12
                };
                drop(pm);
                self.machine.bus.gpio_set_input(pin, !down);
            }
            _ => panic!("Unknown switch {key}"),
        }
    }

    fn press(&mut self, key: &str) -> Result<(), String> {
        self.switch(key, true);
        self.run(200)?;
        self.switch(key, false);
        self.run(180)
    }

    fn press_fast(&mut self, key: &str) -> Result<(), String> {
        self.switch(key, true);
        self.run(35)?;
        self.switch(key, false);
        self.run(45)
    }

    fn center(&mut self) -> Result<(), String> {
        self.press("front")
    }

    fn chord(&mut self, first: &str, second: &str) -> Result<(), String> {
        self.switch(first, true);
        self.run(60)?;
        self.switch(second, true);
        self.run(55)?;
        self.switch(first, false);
        self.run(20)?;
        self.switch(second, false);
        self.run(210)
    }

    fn open_settings(&mut self) -> Result<(), String> {
        self.switch("power", true);
        self.run(1650)?;
        self.switch("power", false);
        self.run(200)
    }

    fn walk(&mut self, seconds: u32) -> Result<(), String> {
        let varied = env::var("STICK_WALK_TRACE").is_ok_and(|value| value == "varied");
        let mut phase: f64 = 0.0;
        for sample in 0..seconds * 100 {
            let hz = if varied { [1.4, 2.0, 3.2][(sample as usize / 2400) % 3] } else { 1.86 };
            self.imu.borrow_mut().sample = [
                (2700.0 * phase.sin()) as i16,
                (5300.0 * phase.cos()) as i16,
                16384 + (3600.0 * phase.sin()) as i16,
            ];
            self.run(10)?;
            phase += std::f64::consts::TAU * hz / 100.0;
        }
        self.imu.borrow_mut().sample = [0, 0, 16384];
        Ok(())
    }

    fn frame(&self) -> Vec<u16> {
        self.panel.borrow().image(
            self.machine
                .bus
                .pwm_output(38)
                .is_some_and(|(duty, _)| duty > 0.0),
        )
    }

    fn snapshot(&self, dir: &Path, name: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut file = fs::File::create(dir.join(format!("{name}.ppm")))?;
        file.write_all(b"P6\n240 135\n255\n")?;
        for pixel in self.frame() {
            let r = ((pixel >> 11) * 255 / 31) as u8;
            let g = (((pixel >> 5) & 63) * 255 / 63) as u8;
            let b = ((pixel & 31) * 255 / 31) as u8;
            file.write_all(&[r, g, b])?;
        }
        Ok(())
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if !(6..=7).contains(&args.len()) {
        return Err(
            "usage: stickwalker-e2e ROM.elf firmware-directory seed-or-flash.bin journey output-directory"
                .into(),
        );
    }
    let firmware = PathBuf::from(&args[2]);
    let journey = args[4].as_str();
    if journey == "clock" {
        return clock::check(&fs::read(&args[1])?).map_err(Into::into);
    }
    let seed = fs::read(&args[3])?;
    let output = PathBuf::from(&args[5]);
    fs::create_dir_all(&output)?;
    let full_flash = seed.len() == FLASH_BYTES;
    let mut image = if full_flash {
        seed.clone()
    } else {
        vec![0xff; FLASH_BYTES]
    };
    for (offset, path) in [
        (0, "bootloader/bootloader.bin"),
        (0x8000, "partition_table/partition-table.bin"),
    ] {
        let data = fs::read(firmware.join(path))?;
        image[offset..offset + data.len()].copy_from_slice(&data);
    }
    let app = fs::read(firmware.join("stickwalker.bin"))?;
    image[APP..APP + app.len()].copy_from_slice(&app);
    if journey == "usb-recovery" {
        image[0] = 0;
    }
    if !full_flash {
        if seed.len() != 65_536 {
            return Err("Pass a native 64 KiB Pokéwalker EEPROM image".into());
        }
        image[SAVE..SAVE + seed.len()].copy_from_slice(&seed);
    }
    let battery_journey = matches!(journey, "pocket-walk" | "depleted" | "low-battery-audio" | "watchdog-loop");
    let (usb, external_5v) = match env::var("STICK_POWER").as_deref() {
        Ok("usb") => (true, false),
        Ok("battery") => (false, false),
        Ok("expansion") => (false, true),
        Err(env::VarError::NotPresent) => (!battery_journey, false),
        _ => return Err("Unknown STICK_POWER".into()),
    };
    let mut rig = Rig::new(
        &fs::read(&args[1])?,
        &image,
        usb,
        matches!(journey, "watchdog-loop" | "usb-recovery"),
        if journey == "depleted" || env::var_os("STICK_UNPLUG_ON_SEND").is_some() {
            3150
        } else if journey == "low-battery-audio" {
            3400
        } else {
            3910
        },
    )?;
    rig.pm.borrow_mut().external_5v = external_5v;
    let path = match env::var("STICK_IR_PATH").as_deref() {
        Ok("weak") => PathModel::Weak,
        Ok("weak-low-trip") => PathModel::WeakLowTrip,
        Ok("wide-pad") => PathModel::WidePad,
        Ok("slow-pad") => PathModel::SlowPad,
        Ok("bright") => PathModel::Bright,
        Ok("nominal") | Err(env::VarError::NotPresent) => PathModel::Nominal,
        _ => return Err("Unknown STICK_IR_PATH".into()),
    };
    rig.optics.borrow_mut().path = path;
    if env::var_os("STICK_CONNECTION").is_some() {
        rig.optics.borrow_mut().stress_seed =
            env::var("STICK_SEED").unwrap_or("1".into()).parse()?;
    }
    if matches!(journey, "send" | "return") {
        let program = args.get(6).ok_or("Pass the HGSS peer executable")?;
        let course = if journey == "send" {
            let data = fs::read(env::var("STICK_COURSE_EEPROM")?)?;
            if data.len() != 65536 {
                return Err("Course source must be a native EEPROM image".into());
            }
            Some(data)
        } else {
            None
        };
        let trainer = env::var("STICK_PEER_EEPROM")
            .ok()
            .map(fs::read)
            .transpose()?;
        if trainer.as_ref().is_some_and(|data| data.len() != 65536) {
            return Err("Peer status source must be a native EEPROM image".into());
        }
        let status = match trainer.as_deref() {
            Some(status) => status,
            None if !full_flash => &seed,
            None => return Err(
                "Pass the paired trainer's EEPROM in STICK_PEER_EEPROM when booting a flash image"
                    .into(),
            ),
        };
        let peer = peer::Peer::open(
            Path::new(program),
            if journey == "send" { "put" } else { "back" },
            status,
            course.as_ref().map(|image| &image[0x8f00..0x8f00 + 10430]),
        )?;
        let mut optics = rig.optics.borrow_mut();
        optics.peer = Some(peer);
        optics.cut = match env::var("STICK_TEAR_COMMIT").as_deref() {
            Ok("upload") => Some(TornWrite::Upload),
            Ok("publish") => Some(TornWrite::Publish),
            Err(env::VarError::NotPresent) => None,
            _ => return Err("Unknown STICK_TEAR_COMMIT".into()),
        };
        optics.lose_commit_reply = env::var_os("STICK_LOSE_COMMIT_REPLY").is_some();
        optics.drop_frame_pulse = env::var("STICK_DROP_PULSE_IN_REPLY")
            .ok()
            .map(|v| v.parse())
            .transpose()?;
    } else if let Some(trace) = args.get(6).filter(|_| journey == "pair") {
        rig.optics.borrow_mut().trace(Path::new(trace))?;
    }
    rig.machine
        .add_symbols(&fs::read(firmware.join("stickwalker.elf"))?)?;
    if journey == "walkers" {
        return walkers::run(
            rig,
            &fs::read(&args[1])?,
            &fs::read(firmware.join("stickwalker.elf"))?,
            &fs::read(args.get(6).ok_or("Pass the second walker flash image")?)?,
            &output,
        );
    }
    let result = (|| -> Result<(), Box<dyn std::error::Error>> {
        if journey == "usb-recovery" {
            rig.run(250)?;
            rig.connect_downloader_over_usb()?;
            let bootloader = fs::read(firmware.join("bootloader/bootloader.bin"))?;
            recovery::repair(&mut rig, &[(0, &bootloader)])?;
            rig.snapshot(&output, "recovered")?;
            return Ok(());
        }
        rig.run(2200)?;
        if journey == "management" { return management::check(&mut rig, &output); }
        if journey == "depleted" {
            if !rig.pm.borrow().power_cut || rig.pm.borrow().rail() {
                return Err("Depleted Stick did not request PM1 power-off".into());
            }
            let saved = rig.machine.bus.flash[SAVE..].to_vec();
            rig.run(10000)?;
            if rig.machine.bus.flash[SAVE..] != saved {
                return Err("Powered-off Stick continued writing its save".into());
            }
            // A charged cell can start through Power, without a USB cable.
            rig.pm.borrow_mut().battery_mv = 3910;
            rig.pm.borrow_mut().next_battery_sample = 0;
            rig.press("power")?;
            rig.run(1800)?;
            if rig.pm.borrow().power_cut || rig.frame().iter().all(|&p| p == 0) {
                return Err("Power did not restart a charged Stick after PM1 shutdown".into());
            }
            rig.usb_power(false, 3150);
            rig.run(3500)?;
            if !rig.pm.borrow().power_cut {
                return Err("Battery depletion during gameplay did not shut down".into());
            }
            rig.usb_power(true, 3150);
            rig.run(1800)?;
            rig.snapshot(&output, "plugged-in")?;
            if !rig.frame().iter().any(|&p| p != 0) || rig.machine.bus.sleeping {
                return Err("Plugging in did not return to the walker".into());
            }
            return Ok(());
        }
        for _ in 0..140 {
            if rig.frame().iter().any(|&pixel| pixel != 0) {
                break;
            }
            rig.run(200)?;
        }
        rig.snapshot(&output, "home")?;
        if !rig.frame().iter().any(|&color| color != 0) || rig.panel.borrow().premature {
            return Err(format!("No home at {:.3}s, PM={}, IMU={}, SPI pixels={}, backlight={:?}, i2c={:?}, app={:?}. {}", rig.machine.seconds(), rig.pm.borrow().rail(), rig.imu.borrow().config_bytes, rig.panel.borrow().lcd.pixels_written, rig.machine.bus.pwm_output(38), rig.machine.bus.periph.i2c[1].irq(), rig.machine.bus.reset_cause(), rig.machine.bus.report()).into());
        }
        if journey == "wake-audio" {
            rig.run(65000)?;
            if rig.pm.borrow().rail() { return Err("Screen did not sleep".into()); }
            rig.press("front")?;
            rig.run(500)?;
            if rig.pm.borrow().amp_on() { return Err("Silent wake kept the amplifier powered".into()); }
            if rig.pm.borrow().codec_unpowered_accesses != 0 {
                return Err("Codec accessed before L3B settled".into());
            }
            speaker::sequence(&mut rig, &output)?;
            return Ok(());
        }
        if journey == "muted-input" {
            for _ in 0..4 { rig.press("side")?; rig.press("power")?; }
            if rig.pm.borrow().amp_on() || !rig.machine.bus.periph.i2s0.pcm.is_empty() {
                return Err("Muted input powered the amplifier or started I2S".into());
            }
            return Ok(());
        }
        if journey == "sound-and-colors" {
            rig.run(200)?;
            speaker::sequence(&mut rig, &output)?;
            rig.open_settings()?;
            rig.press("side")?;
            rig.press("side")?;
            for _ in 0..5 {
                rig.center()?;
            }
            // The game artwork exercises all four shades on the physical LCD.
            let amber = [0xf715, 0xd612, 0xac8d, 0x7200];
            for orientation in 0..2 {
                if orientation == 1 {
                    rig.open_settings()?;
                    rig.press("side")?; // Orientation.
                    rig.center()?;
                    rig.press("side")?; // Palette.
                }
                rig.press("side")?; // Back to game.
                rig.center()?;
                let frame = rig.frame();
                if amber.iter().any(|color| !frame.contains(color))
                    || frame.iter().any(|color| !amber.contains(color))
                {
                    return Err(format!(
                        "Amber's physical RGB output is wrong in rotation {orientation}"
                    )
                    .into());
                }
                rig.snapshot(&output, &format!("amber-{orientation}"))?;
            }
            return Ok(());
        }
        if journey == "sleep-wake" {
            rig.usb_power(false, 3910);
            let asleep = *rig.machine.symbols.iter()
                .find(|(_, name)| name.as_str() == "screen_asleep")
                .ok_or("Missing screen state")?.0;
            for (key, delay_ms) in [("front", 0), ("side", 1), ("front", 4), ("side", 250)] {
                rig.run(65000)?;
                if rig.pm.borrow().rail() || rig.pm.borrow().watchdog_at.is_none() {
                    return Err("Screen sleep left the rail on or watchdog disabled".into());
                }
                // Interrupt a fresh sleep at several points, including before
                // the normal flash-ready delay has elapsed.
                let entries = rig.machine.bus.sleep_entries;
                for _ in 0..5000 {
                    rig.run(1)?;
                    if rig.machine.bus.sleep_entries > entries && rig.machine.bus.sleeping { break; }
                }
                if !rig.machine.bus.sleeping { return Err("No fresh sleep for button wake".into()); }
                rig.run(delay_ms)?;
                rig.switch(key, true);
                rig.run(30)?;
                rig.switch(key, false);
                rig.run(50)?;
                if rig.machine.bus.read8(asleep).map_err(|e| format!("{e:?}"))? != 0 {
                    return Err(format!("{key} did not wake the sleeping game").into());
                }
                rig.run(400)?; // Panel reset/sleep-out settling follows logical wake.
                if rig.frame().iter().all(|&pixel| pixel == 0) {
                    return Err(format!("{key} woke the game but not the display").into());
                }
            }
            rig.run(65000)?;
            rig.usb_power(true, 3910);
            // Cable power precedes enumeration. Idle source checks detect it
            // within 30 seconds; a face-button wake refreshes it immediately.
            rig.machine.bus.periph.usb.connected = false;
            rig.run(31000)?;
            if rig.machine.bus.sleeping {
                return Err("Cable power was not detected at the idle source deadline".into());
            }
            rig.machine.bus.periph.usb.connected = true;
            rig.run(65000)?;
            if rig.machine.reboots != 0 {
                return Err("Button or cable wake caused an unexpected restart".into());
            }
            rig.connect_downloader_over_usb()?;
            recovery::sync(&mut rig)?;
            recovery::restart(&mut rig)?;
            rig.snapshot(&output, "usb-recovered")?;
            return Ok(());
        }
        if journey == "connect-noise" {
            let link = *rig.machine.symbols.iter()
                .find(|(_, name)| name.as_str() == "in_link")
                .ok_or("Missing link state")?.0;
            rig.center()?;
            rig.run(1000)?;
            rig.center()?;
            let start = rig.machine.bus.cycles();
            let mut finished = false;
            for _ in 0..225 {
                let at = rig.machine.bus.cycles() + CYCLES_PER_MS;
                // Complete, non-protocol noise bytes less than one turn apart.
                rig.optics.borrow_mut().illuminate(&[0x00], at);
                rig.run(20)?;
                if rig.machine.bus.read8(link).map_err(|e| format!("{e:?}"))? == 0 {
                    if rig.machine.bus.cycles() - start < 500 * CYCLES_PER_MS {
                        return Err("A noisy burst killed discovery before its retry budget".into());
                    }
                    finished = true;
                    break;
                }
            }
            if !finished {
                return Err("Noise kept Connect alive beyond its discovery budget".into());
            }
            rig.press("side")?; // Dismiss the result.
            rig.center()?;
            rig.run(1000)?;
            rig.center()?;
            if rig.machine.bus.read8(link).map_err(|e| format!("{e:?}"))? == 0 {
                return Err("Connect could not reopen after a noisy attempt".into());
            }
            rig.run(4500)?;
            if rig.machine.bus.read8(link).map_err(|e| format!("{e:?}"))? != 0 {
                return Err("Quiet Connect did not finish after retrying in the same boot".into());
            }
            return Ok(());
        }
        if journey == "settings-focus" {
            let state = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, name)| name.as_str() == "g_state")
                .ok_or("Missing game state")?
                .0;
            // RuntimeState.view: the current Nintendo screen, not host overlay state.
            rig.press("power")?;
            rig.press("power")?;
            let before = rig
                .machine
                .bus
                .read8(state + 49)
                .map_err(|e| format!("{e:?}"))?;
            for phase in 0..8 {
                rig.open_settings()?;
                for _ in 0..3 {
                    rig.press("side")?;
                }
                rig.run(phase * 8)?;
                rig.center()?;
                let after = rig
                    .machine
                    .bus
                    .read8(state + 49)
                    .map_err(|e| format!("{e:?}"))?;
                if before != after {
                    return Err(format!(
                        "Closing Settings activated game screen {after} from {before}"
                    )
                    .into());
                }
            }
            return Ok(());
        }
        if journey == "stationary" {
            let state = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, name)| name.as_str() == "g_state")
                .ok_or("Missing game state")?
                .0;
            let jitter = [-15, 10, -5, 20, -20, 5, 15, -10];
            for rest in [
                [0, 0, 16384],
                [128, -128, 16384],
                [256, -256, 16380],
                [8192, -8192, 11585],
            ] {
                rig.imu.borrow_mut().sample = rest;
                rig.run(8000)?; // Settle the game's complete motion window after changing pose.
                let before = rig
                    .machine
                    .bus
                    .read32(state)
                    .map_err(|e| format!("{e:?}"))?;
                // Four complete 64-sample FFT windows at 16 Hz, with 100 Hz sensor noise.
                for sample in 0..1600 {
                    rig.imu.borrow_mut().sample = [
                        rest[0] + jitter[sample % 8],
                        rest[1] + jitter[(sample * 3 + 1) % 8],
                        rest[2] + jitter[(sample * 5 + 2) % 8],
                    ];
                    rig.run(10)?;
                }
                let after = rig
                    .machine
                    .bus
                    .read32(state)
                    .map_err(|e| format!("{e:?}"))?;
                if before != after {
                    return Err(format!(
                        "Stationary noise at {rest:?} credited {} steps",
                        after - before
                    )
                    .into());
                }
            }
            return Ok(());
        }
        if journey == "gameplay" {
            return gameplay::run(&mut rig, &output);
        }
        if journey == "inventory" {
            return gameplay::inventory(&mut rig, &output);
        }
        if journey == "first-boot" {
            return Ok(());
        }
        if journey == "low-battery-audio" {
            rig.press("power")?;
            rig.press("power")?;
            rig.run(3200)?; // Let the amplifier cool before measuring a cold note.
            speaker::observe(&mut rig);
            let peak = speaker::chirp(&mut rig, &output)?;
            if peak >= 1000.0 {
                return Err("The chirp exceeded its low-battery gain budget".into());
            }
            rig.run_speaker(600)?;
            if rig.pm.borrow().amp_on() {
                return Err("Speaker stayed powered after the chirp".into());
            }
            return Ok(());
        }
        if journey == "power-recovery" {
            let reads = rig.pm.borrow().key_reads;
            rig.run(1000)?;
            if rig.pm.borrow().key_reads - reads > 18 {
                return Err("Idle Power sampling exceeded the game tick rate".into());
            }
            let game = rig.frame();
            rig.switch("power", true);
            rig.run(1700)?;
            let release_prompt = rig.frame();
            if release_prompt == game || rig.machine.reboots != 0 {
                return Err("A Power hold did not reach Settings before recovery".into());
            }
            rig.run(2600)?;
            rig.switch("power", false);
            rig.run(600)?;
            recovery::sync(&mut rig)?;
            let resets = rig.machine.reboots;
            rig.press("power")?;
            rig.run(1200)?;
            if rig.machine.reboots != resets + 1 || rig.frame().iter().all(|&pixel| pixel == 0) {
                return Err("A short Power press did not leave download mode".into());
            }

            // Lose the application and startup image while Power is held.
            // A hardware recovery gesture must work even with both damaged.
            rig.switch("power", true);
            rig.run(1700)?;
            rig.machine.bus.flash[0] = 0;
            rig.machine.bus.flash[APP] = 0;
            rig.run(2600)?;
            rig.switch("power", false);
            rig.run(600)?;
            let bootloader = fs::read(firmware.join("bootloader/bootloader.bin"))?;
            recovery::repair(&mut rig, &[(0, &bootloader), (APP, &app)])?;
            rig.snapshot(&output, "recovered")?;
            return Ok(());
        }
        if journey == "watchdog-loop" {
            for _ in 0..3 {
                rig.stall_until_watchdog()?;
                if rig.frame().iter().all(|&pixel| pixel == 0)
                    || rig.pm.borrow().watchdog_at.is_none()
                {
                    return Err(
                        "Watchdog restart did not return to a protected, playable app".into(),
                    );
                }
            }
            rig.usb_power(true, 3910);
            rig.connect_downloader_over_usb()?;
            recovery::sync(&mut rig)?;
            recovery::restart(&mut rig)?;
            rig.snapshot(&output, "recovered")?;
            return Ok(());
        }
        if journey == "pocket-walk" {
            rig.run(61_000)?;
            let walk_budget = energy::Sample::take(&rig);
            let key_reads = rig.pm.borrow().key_reads;
            let pm_transactions = rig.pm.borrow().i2c_transactions;
            let state = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, n)| n.as_str() == "g_state")
                .ok_or("Missing state")?
                .0;
            let eeprom = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, n)| n.as_str() == "eeprom")
                .ok_or("Missing EEPROM")?
                .0;
            let before_steps = rig
                .machine
                .bus
                .read32(state)
                .map_err(|e| format!("{e:?}"))?;
            rig.walk(72)?;
            let walk_budget = walk_budget.finish(&rig, "walk")?;
            // Compare completed motion, not a live counter whose last FIFO
            // window and paced step credit may still be pending at second 72.
            rig.run(8000)?;
            let steps = rig
                .machine
                .bus
                .read32(state)
                .map_err(|e| format!("{e:?}"))?;
            let daily = rig
                .machine
                .bus
                .read32(state + 28)
                .map_err(|e| format!("{e:?}"))?;
            let history = rig
                .machine
                .bus
                .read32(eeprom + 0xcef0)
                .map_err(|e| format!("{e:?}"))?
                .swap_bytes();
            fs::write(
                output.join("motion.csv"),
                format!(
                    "steps,daily,yesterday\n{},{daily},{history}\n",
                    steps - before_steps
                ),
            )?;
            if env::var_os("STICK_WALK_TRACE").is_none() && !(120..=152).contains(&(steps - before_steps)) {
                return Err(format!(
                    "72 seconds of walking credited {} steps",
                    steps - before_steps
                )
                .into());
            }
            if env::var_os("STICK_MOTION_COMPARE").is_some() { return Ok(()); }
            if rig.pm.borrow().key_reads != key_reads {
                return Err("Sleeping walker polled the Power button".into());
            }
            rig.imu.borrow_mut().sample = [0, 0, 16384];
            let rest_budget = energy::Sample::take(&rig);
            rig.pm.borrow_mut().longest_key_gap = 0;
            rig.run(40_000)?;
            let steady_rest = energy::Sample::take(&rig);
            rig.run(60_000)?;
            if rig.pm.borrow().key_reads != key_reads {
                return Err("Resting walker polled the Power button".into());
            }
            if rig.machine.reboots != 0 {
                return Err("The walker rebooted while resting in a pocket".into());
            }
            let rest_budget = rest_budget.finish(&rig, "still-after-walk")?;
            let steady_rest = steady_rest.finish(&rig, "rest")?;
            fs::write(
                output.join("residency.csv"),
                format!("{}{}{}{}", energy::HEADER, walk_budget.row, rest_budget.row, steady_rest.row),
            )?;
            println!(
                "Pocket: {} wakes, {:.2}% awake, {:.2}..{:.2} mA components (board overhead excluded)",
                walk_budget.wakes,
                walk_budget.active_fraction * 100.0,
                walk_budget.low_ma,
                walk_budget.high_ma
            );
            if env::var_os("STICK_POWER_BASELINE").is_none()
                && (walk_budget.wakes > 25
                    || walk_budget.active_fraction > 0.013
                    || walk_budget.memory_off_fraction < 0.97
                    || steady_rest.memory_off_fraction < 0.97
                    || rest_budget.wakes > 90)
            {
                return Err(
                    "Pocket workload exceeded its wake or CPU budget; see residency.csv".into(),
                );
            }
            // 180 seconds permits six 30-second housekeeping rounds. Each
            // source/voltage read pair plus feed costs five STARTs; allow two extra rounds.
            if rig.pm.borrow().i2c_transactions - pm_transactions > 40 {
                return Err(
                    "Pocket walk/rest performed excessive total PM1 I2C transactions".into(),
                );
            }
            rig.snapshot(&output, "pocket")?;
            if rig.pm.borrow().rail()
                || !rig.frame().iter().all(|&p| p == 0)
                || rig.machine.bus.sleep_cycles == 0
            {
                return Err("Pocket walk did not use screen-off sleep".into());
            }
            rig.press("power")?;
            if !rig.frame().iter().all(|&p| p == 0) {
                return Err("Power unexpectedly woke the sleeping screen".into());
            }
            // Pick up the resting walker before walking. A pure 1.86 Hz sine
            // aliases below Nintendo's 1 Hz activity threshold at some phases.
            rig.imu.borrow_mut().sample = [8192, 0, 14189];
            rig.run(1500)?;
            rig.walk(16)?;
            // Inject an unserviced sensor backlog. Stalling the whole CPU here
            // would instead exercise the interrupt watchdog's earlier reset.
            if env::var_os("STICK_POWER_BASELINE").is_none() {
                let now = rig.machine.bus.cycles();
                {
                    let mut imu = rig.imu.borrow_mut();
                    imu.available_at = (now - 20000 * CYCLES_PER_MS) / imu.period() * imu.period();
                    imu.advance(now);
                }
                rig.run(4100)?;
                if rig.imu.borrow().fifo.len() >= 2046 || rig.machine.reboots != 0 {
                    return Err("Full sensor FIFO did not recover in the same boot".into());
                }
            }
            rig.walk(16)?;
            for _ in 0..400 {
                if rig.machine.bus.sleeping { break; }
                rig.run(1)?;
            }
            if !rig.machine.bus.sleeping { return Err("Walking CPU did not return to sleep".into()); }
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
            rig.switch("front", true);
            for _ in 0..400 {
                rig.run(1)?;
                if rig.frame().iter().any(|&p| p != 0) {
                    break;
                }
            }
            let shown = rig
                .machine
                .bus
                .read32(state)
                .map_err(|e| format!("{e:?}"))?;
            if shown < before + 3 || rig.frame().iter().all(|&p| p == 0) {
                return Err(format!(
                    "First wake frame omitted paced pocket steps ({before} -> {shown})"
                )
                .into());
            }
            rig.switch("front", false);
            rig.run(400)?;
            rig.snapshot(&output, "wake")?;
            if rig.frame().iter().all(|&p| p == 0) {
                return Err("Front did not wake the screen from the pocket walk".into());
            }
            let reads = rig.pm.borrow().key_reads;
            rig.pm.borrow_mut().longest_key_gap = 0;
            rig.run(1000)?;
            if !(15..=18).contains(&(rig.pm.borrow().key_reads - reads))
                || rig.pm.borrow().longest_key_gap > 80 * CYCLES_PER_MS
            {
                return Err("Awake Power sampling did not follow the game tick rate".into());
            }
            let game = rig.frame();
            rig.switch("power", true);
            rig.run(1650)?;
            rig.switch("power", false);
            rig.run(300)?;
            rig.snapshot(&output, "fresh-hold")?;
            if game == rig.frame() {
                return Err("Fresh Power hold did not open Settings".into());
            }
            let before_restart = rig.machine.bus.read32(state).map_err(|e| format!("{e:?}"))?;
            for _ in 0..3 { rig.press("side")?; }
            rig.center()?; // Return to the expedition.
            management::request(&mut rig, host::protocol::PREPARE, &[])?;
            rig.machine.reboot();
            rig.run(1800)?;
            let after_restart = rig.machine.bus.read32(state).map_err(|e| format!("{e:?}"))?;
            if rig.machine.reboots != 1 || after_restart < before_restart {
                return Err("Pocket steps did not survive the requested restart".into());
            }
            return Ok(());
        }
        if matches!(journey, "connect" | "pair" | "send" | "return") {
            let reboots_before_link = rig.machine.reboots;
            let story = env::var("STICK_CONNECTION").unwrap_or_default();
            rig.optics.borrow_mut().timed = story.is_empty();
            let link_address = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, n)| n.as_str() == "in_link")
                .ok_or("Missing link state")?
                .0;
            if story == "retry" || story == "glitch" {
                let path = rig.optics.borrow().path;
                let eeprom = *rig.machine.symbols.iter()
                    .find(|(_, name)| name.as_str() == "eeprom").ok_or("Missing EEPROM")?.0;
                let ownership = rig.machine.bus.read8(eeprom + 0x148)
                    .map_err(|e| format!("{e:?}"))? & 3;
                if story == "retry" {
                    // The DS is on the wrong screen or still out of view. Let
                    // discovery expire, then retry without rebooting the Stick.
                    rig.optics.borrow_mut().aligned_after = u64::MAX;
                } else {
                    rig.optics.borrow_mut().drop_frame_pulse = Some(30);
                }
                rig.center()?;
                rig.run(1000)?;
                rig.optics.borrow_mut().timed = true;
                rig.center()?;
                for _ in 0..180 {
                    rig.run(25)?;
                    if rig
                        .machine
                        .bus
                        .read8(link_address)
                        .map_err(|e| format!("{e:?}"))?
                        == 0
                    {
                        break;
                    }
                }
                if rig
                    .machine
                    .bus
                    .read8(link_address)
                    .map_err(|e| format!("{e:?}"))?
                    != 0
                    || rig.machine.reboots != reboots_before_link
                {
                    return Err("Failed attempt did not return without reboot".into());
                }
                rig.optics.borrow_mut().timed = false;
                if story == "retry" && rig.optics.borrow().missed_probes == 0 {
                    return Err("No-peer attempt never sent a discovery probe".into());
                }
                if story == "glitch" && rig.optics.borrow().replies.len() <= 30 {
                    return Err("Glitch attempt never reached the selected burst".into());
                }
                if rig.optics.borrow().peer.as_ref().unwrap().result == 15 ||
                    rig.machine.bus.read8(eeprom + 0x148).map_err(|e| format!("{e:?}"))? & 3 != ownership {
                    return Err("Failed attempt changed Pokemon ownership".into());
                }
                // A new DS attempt uses a fresh real protocol engine, while the Stick stays running.
                let status = fs::read(env::var("STICK_PEER_EEPROM")?)?;
                let course = env::var("STICK_COURSE_EEPROM")
                    .ok()
                    .map(fs::read)
                    .transpose()?;
                let peer = peer::Peer::open(
                    Path::new(args.get(6).unwrap()),
                    if journey == "send" { "put" } else { "back" },
                    &status,
                    course
                        .as_ref()
                        .filter(|_| journey == "send")
                        .map(|c| &c[0x8f00..0x8f00 + 10430]),
                )?;
                {
                    let mut o = rig.optics.borrow_mut();
                    o.peer = Some(peer);
                    o.path = path;
                    o.aligned_after = 0;
                    o.drop_frame_pulse = None;
                    o.missed = None;
                }
                rig.press("side")?;
            }
            if story == "delayed" {
                rig.optics.borrow_mut().lose_shutdown = true;
                rig.optics.borrow_mut().aligned_after =
                    rig.machine.bus.cycles() + 2300 * CYCLES_PER_MS;
            }
            if story == "hot" {
                // Unpaired discovery traffic while the independently-clocked HGSS engine waits.
                let mut o = rig.optics.borrow_mut();
                o.startup_interference = true;
                let start = rig.machine.bus.cycles() + 1400 * CYCLES_PER_MS;
                for i in 0..500 {
                    o.illuminate(&[0xfc ^ 0xaa], start + i * CYCLES_PER_MS);
                }
            }
            rig.center()?;
            rig.run(1000)?;
            rig.optics.borrow_mut().timed = true;
            rig.center()?;
            let mut unplug = env::var_os("STICK_UNPLUG_ON_SEND").is_some();
            let expect_unplug = unplug;
            let mut plug = env::var_os("STICK_PLUG_ON_SEND").is_some();
            let expect_link_loss = env::var_os("STICK_DROP_PULSE_IN_REPLY").is_some();
            let lose_reply = rig.optics.borrow().lose_commit_reply;
            let mut last_tx_count = 0;
            let mut quiet_slices = 0;
            for turn in 0..600 {
                rig.run(25)?;
                if !story.is_empty() && turn % 40 == 0 {
                    eprintln!(
                        "{story} {journey}: {:.3}s, {} frames, peer {}",
                        rig.machine.seconds(),
                        rig.optics.borrow().sent.len(),
                        rig.optics.borrow().peer.as_ref().unwrap().result
                    );
                }
                if !story.is_empty()
                    && rig
                        .machine
                        .bus
                        .read8(link_address)
                        .map_err(|e| format!("{e:?}"))?
                        == 0
                {
                    break;
                }
                let optics = rig.optics.borrow();
                if optics.power_cut
                    || optics.missed.is_some()
                    || journey == "pair" && optics.remaining() == 0
                    || optics
                        .peer
                        .as_ref()
                        .is_some_and(|peer| peer.result > 1 && !peer.connected)
                {
                    break;
                }
                if optics.sent.len() != last_tx_count {
                    last_tx_count = optics.sent.len();
                    quiet_slices = 0;
                } else {
                    quiet_slices += 1;
                }
                if plug && optics.replies.len() >= 20 {
                    drop(optics);
                    plug = false;
                    rig.usb_power(true, 3910);
                } else if unplug && optics.replies.len() >= 20 {
                    drop(optics);
                    unplug = false;
                    rig.usb_power(false, 3150);
                } else if expect_unplug && !unplug && rig.pm.borrow().power_cut {
                    break;
                } else if expect_link_loss
                    && optics.replies.len() > optics.drop_frame_pulse.unwrap()
                {
                    drop(optics);
                    rig.run(350)?;
                    break;
                } else if lose_reply && optics.lost_reply {
                    drop(optics);
                    rig.run(1800)?;
                    break;
                } else if quiet_slices >= 160 && last_tx_count > 0 && !expect_unplug {
                    break;
                }
            }
            rig.run(20)?;
            if rig.pm.borrow().boost_conflict {
                return Err("IR drove the boost output onto a supplied expansion rail".into());
            }
            let link = *rig
                .machine
                .symbols
                .iter()
                .find(|(_, name)| name.as_str() == "in_link")
                .ok_or("Missing firmware in_link symbol")?
                .0;
            for _ in 0..if story.is_empty() { 1200 } else { 200 } {
                if rig.optics.borrow().power_cut
                    || rig.pm.borrow().power_cut
                    || rig.machine.bus.read8(link).map_err(|e| format!("{e:?}"))? == 0
                {
                    break;
                }
                rig.run(25)?;
            }
            rig.optics.borrow_mut().timed = false;
            // Publication can cut power during the post-reply wait above.
            let power_cut = rig.optics.borrow().power_cut;
            if !power_cut && !rig.pm.borrow().power_cut
                && rig.machine.bus.read8(link).map_err(|e| format!("{e:?}"))? != 0
            {
                return Err("Communication did not finish within its completion deadline".into());
            }
            rig.run(if power_cut { 4500 } else { 600 })?;
            if !power_cut
                && !rig.pm.borrow().power_cut
                && rig.machine.reboots != reboots_before_link
            {
                return Err("An IR transfer rebooted instead of returning to the game".into());
            }
            if expect_unplug {
                if unplug
                    || !rig.pm.borrow().power_cut
                    || rig.pm.borrow().boost_on()
                    || !rig
                        .optics
                        .borrow()
                        .peer
                        .as_ref()
                        .is_some_and(|peer| peer.result == 15)
                {
                    return Err(
                        "USB removal did not preserve the accepted transfer before low-battery shutdown"
                            .into(),
                    );
                }
                rig.usb_power(true, 3150);
                rig.run(1800)?;
                if rig.machine.bus.sleeping || rig.frame().iter().all(|&pixel| pixel == 0) {
                    return Err("USB did not wake the walker after post-link shutdown".into());
                }
                rig.snapshot(&output, "result")?;
                return Ok(());
            }
            rig.snapshot(&output, "result")?;
            let optics = rig.optics.borrow();
            if !story.is_empty() && optics.pad_stalls == 0 {
                return Err("Connection stress did not delay peripheral writes".into());
            }
            if optics.sent.is_empty() {
                return Err("Connect did not emit a frame".into());
            }
            if optics.power_cut {
                if rig.machine.reboots == 0 || !rig.frame().iter().any(|&pixel| pixel != 0) {
                    return Err("The interrupted transfer did not reboot to the walker".into());
                }
                return Ok(());
            }
            if expect_link_loss {
                if optics.peer.as_ref().is_some_and(|peer| peer.result == 15)
                    || rig.pm.borrow().registers[6] & 8 != 0
                {
                    return Err("The corrupted transfer did not terminate safely".into());
                }
                return Ok(());
            }
            if lose_reply {
                if !optics.lost_reply || optics.peer.as_ref().is_some_and(|peer| peer.result == 15)
                {
                    return Err("HGSS acknowledged a send whose commit reply was lost".into());
                }
                return Ok(());
            }
            if optics.cut.is_some() {
                return Err("The requested power cut was never reached".into());
            }
            if let Some(peer) = &optics.peer {
                if peer.result != 15 || optics.missed.is_some() {
                    return Err(format!(
                        "HGSS {journey} ended with result {} after {} frames. {:?}",
                        peer.result,
                        optics.sent.len(),
                        optics.missed
                    )
                    .into());
                }
            }
            if story == "delayed" && (optics.missed_probes == 0 || !optics.lost_shutdown) {
                return Err("Delayed alignment or lost final shutdown was not exercised".into());
            }
            if story == "hot" && optics.sent.iter().filter(|wire| wire.as_slice() == [0xfc ^ 0xaa]).count() != 1 {
                return Err("Hot startup did not accept the first discovery reply".into());
            }
            if story == "hot" && optics.startup_light_samples == 0 {
                return Err(
                    "Hot-start traffic never reached the running sampler before its first probe"
                        .into(),
                );
            }
            if journey == "pair" && (optics.remaining() != 0 || optics.missed.is_some()) {
                return Err(format!(
                    "Only {} captured turns completed; {} remain. {:?}",
                    optics.exchanges,
                    optics.remaining(),
                    optics.missed
                )
                .into());
            }
            drop(optics);
            if !story.is_empty() {
                let ui = *rig
                    .machine
                    .symbols
                    .iter()
                    .find(|(_, n)| n.as_str() == "ui")
                    .ok_or("Missing host UI")?
                    .0;
                rig.open_settings()?;
                if rig.machine.bus.read32(ui).map_err(|e| format!("{e:?}"))? != 1 {
                    return Err("The completed connection left controls unresponsive".into());
                }
                for _ in 0..3 {
                    rig.press("side")?;
                }
                rig.center()?;
                if rig.machine.bus.read32(ui).map_err(|e| format!("{e:?}"))? != 0 {
                    return Err("Could not return to the game after the connection".into());
                }
            }
            return Ok(());
        }
        if args[4] != "controls-and-settings" {
            return Err("Unknown journey".into());
        }
        // Move through the native screens before opening the Stick overlay.
        let first = rig.frame();
        rig.press_fast("power")?;
        rig.press_fast("power")?;
        rig.run(160)?;
        let third = rig.frame();
        if first == third || rig.machine.reboots != 0 {
            return Err("Rapid Power presses did not page through the game".into());
        }
        if rig.pm.borrow().led_flashes != 0 {
            return Err("Power double-click triggered the PM1 status LED".into());
        }
        rig.open_settings()?;
        rig.snapshot(&output, "controls-and-settings")?;
        let settings = rig.frame();
        if settings.iter().all(|&pixel| pixel == 0) || settings == third {
            return Err("Power hold did not open Settings".into());
        }
        rig.walk(14)?;
        rig.center()?;
        rig.snapshot(&output, "controls")?;
        let controls = rig.frame();
        if controls == settings {
            return Err("Center did not open Controls".into());
        }
        rig.center()?; // Front + Side.
        for _ in 0..4 {
            rig.press("side")?;
        }
        rig.center()?; // Apply Front + Side using the old mapping.
        rig.open_settings()?;
        rig.press("front")?; // Center in Front + Side.
        rig.press("front")?; // Select Power + Side.
        for _ in 0..2 {
            rig.chord("front", "side")?; // Left in Front + Side.
        }
        rig.press("front")?; // Apply Power + Side.
        rig.open_settings()?;
        rig.chord("power", "side")?; // Center in Power + Side.
        rig.chord("power", "side")?; // Power + Front.
        rig.chord("power", "side")?; // All three.
        rig.press("side")?; // Remap the physical Power switch.
        rig.chord("power", "side")?;
        for _ in 0..3 {
            rig.press("side")?;
        }
        rig.chord("power", "side")?; // Apply and close.
        rig.open_settings()?;
        rig.press("power")?; // The remapped Center opens Controls.
        let remapped = rig.frame();
        if remapped == settings || remapped == controls {
            return Err("Controls did not show the new button assignments".into());
        }
        rig.press("front")?; // Left wraps from Buttons to Back.
        rig.press("power")?;
        rig.press("side")?; // Right in the new layout.
        rig.press("power")?; // Center toggles Orientation.
        rig.press("side")?;
        rig.press("power")?; // Palette.
        let game_shade = rig.machine.bus.pwm_output(38);
        rig.press("side")?; // Back to game.
        rig.press("power")?;
        rig.machine.reboot();
        rig.run(1200)?;
        if rig.machine.reboots == 0 {
            return Err("Simulated reset did not reboot".into());
        }
        let game_after_restart = rig.frame();
        rig.open_settings()?;
        if rig.frame() == settings {
            return Err("Settings did not survive Restart".into());
        }
        rig.snapshot(&output, "settings-after-restart")?;
        rig.usb_power(false, 3910);
        rig.run(91_000)?;
        let asleep = rig.frame().iter().all(|&pixel| pixel == 0);
        if !asleep || rig.machine.bus.sleep_cycles == 0 {
            return Err("Settings timeout did not enter screen-off light sleep".into());
        }
        rig.press("front")?;
        rig.run(350)?;
        rig.snapshot(&output, "wake")?;
        let awake = rig.frame();
        if awake == settings || awake.iter().all(|&pixel| pixel == 0) {
            return Err("Front did not wake into the game after Settings timed out".into());
        }
        if awake.iter().enumerate().any(|(i, pixel)| {
            let (x, y) = (i % 240, i / 240);
            let margin = !(24..216).contains(&x) || !(3..131).contains(&y);
            margin && *pixel != game_after_restart[i]
        }) {
            return Err("Settings pixels remained around the game after timeout and wake".into());
        }
        if rig.machine.bus.pwm_output(38) != game_shade {
            return Err("Settings changed game-controlled brightness".into());
        }
        speaker::observe(&mut rig);
        rig.open_settings()?;
        for _ in 0..3 { rig.press("side")?; } // Back to game.
        rig.press("power")?;
        rig.run(65000)?;
        rig.press("front")?;
        rig.run(400)?;
        if rig.frame().iter().all(|&pixel| pixel == 0) {
            return Err("Front did not wake from ordinary screen sleep".into());
        }
        if rig.machine.bus.pwm_output(38) != game_shade {
            return Err("Game-controlled Shade did not survive screen sleep".into());
        }
        Ok(())
    })();
    if result.is_ok()
        && matches!(
            journey,
            "first-boot" | "send" | "return" | "pair" | "gameplay" | "inventory"
        )
    {
        let address = *rig
            .machine
            .symbols
            .iter()
            .find(|(_, name)| name.as_str() == "eeprom")
            .ok_or("Missing firmware EEPROM symbol")?
            .0;
        let image: Vec<u8> = (0..65536)
            .map(|offset| rig.machine.bus.read8(address + offset))
            .collect::<Result<_, _>>()
            .map_err(|e| format!("{e:?}"))?;
        fs::write(output.join("live-eeprom.bin"), image)?;
    }
    let optics = rig.optics.borrow();
    if result.is_ok() && env::var_os("STICK_FAST_REPLY").is_some() {
        let probes = optics.sent.iter().take_while(|frame| frame.len() == 1).count();
        if probes != 1 { return Err(format!("Fast peer needed {probes} discovery probes").into()); }
    }
    fs::write(
        output.join("reply.txt"),
        optics
            .replies
            .iter()
            .map(|(at, bytes)| {
                format!(
                    "{at} {}\n",
                    bytes
                        .iter()
                        .map(|byte| format!("{byte:02x}"))
                        .collect::<String>()
                )
            })
            .collect::<String>(),
    )?;
    fs::write(
        output.join("walker.txt"),
        optics
            .sent
            .iter()
            .map(|frame| peer::hex(frame) + "\n")
            .collect::<String>(),
    )?;
    fs::write(
        output.join("optical.txt"),
        format!(
            "turns={} remaining={} frames={} power_cut={} failure={:?}\n",
            optics.exchanges,
            optics.remaining(),
            optics.sent.len(),
            optics.power_cut,
            optics.missed
        ),
    )?;
    fs::write(output.join("console.txt"), &rig.machine.console.all)?;
    if env::var_os("STICK_KEEP_FLASH").is_some() {
        fs::write(output.join("flash.bin"), &rig.machine.bus.flash)?;
    } else {
        fs::write(output.join("walker2.bin"), &rig.machine.bus.flash[SAVE..])?;
    }
    result
}
