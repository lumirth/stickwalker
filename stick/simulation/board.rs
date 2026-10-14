use esp_periph::i2c::I2cDevice;
use esp_soc::{
    board::{BoardEdge, BoardModel, SpiPins},
    devices::dcs_panel::DcsPanel,
};
use std::sync::{Arc, Mutex};

#[derive(Default)]
pub struct Inputs {
    pub left: bool,
    pub left_latched: bool,
    pub optical: Vec<Vec<(u64, bool)>>,
    pub cycle: u64,
    pub vbus_mv: Option<u16>,
    pub peer: Option<crate::peer::OpticalPeer>,
    pub local_echo: bool,
    pub reply_window_erases: u64,
}

enum Device {
    Pmic,
    Imu,
    Codec,
}
struct Registers {
    device: Device,
    regs: [u8; 256],
    pointer: u8,
    first: bool,
    inputs: Arc<Mutex<Inputs>>,
    config: Vec<u8>,
    consumed_sample: Option<u64>,
}
impl Registers {
    fn new(device: Device, inputs: Arc<Mutex<Inputs>>) -> Self {
        let mut value = Self {
            device,
            regs: [0; 256],
            pointer: 0,
            first: true,
            inputs,
            config: Vec::new(),
            consumed_sample: None,
        };
        value.reset();
        value
    }
    fn reset(&mut self) {
        self.regs.fill(0);
        match self.device {
            Device::Pmic => {
                self.regs[0..4].copy_from_slice(&[0x50, 0x20, 0, 0]);
                self.regs[4] = 1;
                self.regs[6] = 0x17;
                self.regs[0x22..0x24].copy_from_slice(&4000u16.to_le_bytes());
                self.regs[0x24..0x26].copy_from_slice(&5000u16.to_le_bytes());
            }
            Device::Imu => {
                self.regs[0] = 0x24;
                self.regs[0x7c] = 3;
                self.regs[0x40] = 0xa8;
                self.config.clear();
                self.consumed_sample = None;
            }
            Device::Codec => {
                self.regs[0xfd] = 0x83;
                self.regs[0xfe] = 0x11;
            }
        }
    }
}
impl I2cDevice for Registers {
    fn pins(&self) -> Option<(u8, u8)> {
        Some((47, 48))
    }
    fn start(&mut self, read: bool) -> bool {
        if !read {
            self.first = true;
        }
        true
    }
    fn write(&mut self, byte: u8) -> bool {
        if self.first {
            self.pointer = byte;
            self.first = false;
            return true;
        }
        let reg = self.pointer as usize;
        match self.device {
            Device::Imu if reg == 0x7e && byte == 0xb6 => self.reset(),
            Device::Imu if reg == 0x5e => {
                self.config.push(byte);
                return true;
            }
            Device::Imu if reg == 0x59 => {
                self.regs[reg] = byte;
                if byte == 1 && self.config.len() == 8192 {
                    self.regs[0x21] = 1;
                }
            }
            Device::Pmic if reg < 4 || (0x20..=0x27).contains(&reg) || reg == 0x48 => {}
            Device::Imu if reg <= 0x27 => {}
            _ => self.regs[reg] = byte,
        }
        self.pointer = self.pointer.wrapping_add(1);
        true
    }
    fn read(&mut self) -> u8 {
        let reg = self.pointer as usize;
        let byte = match self.device {
            Device::Pmic if reg == 4 => {
                if self.inputs.lock().unwrap().vbus_mv.unwrap_or(5000) >= 4000 {
                    1
                } else {
                    4
                }
            }
            Device::Pmic if reg == 0x24 || reg == 0x25 => self
                .inputs
                .lock()
                .unwrap()
                .vbus_mv
                .unwrap_or(5000)
                .to_le_bytes()[reg - 0x24],
            Device::Pmic if reg == 0x48 => {
                let mut state = self.inputs.lock().unwrap();
                let byte = u8::from(state.left) | if state.left_latched { 0x80 } else { 0 };
                state.left_latched = false;
                byte
            }
            Device::Imu if reg == 0x1d => {
                let odr = self.regs[0x40] & 15;
                if self.regs[0x7d] & 4 != 0 && self.regs[0x21] == 1 && (1..=12).contains(&odr) {
                    let period = 240_000_000u64 * 64 / (25u64 << odr);
                    let sample = self.inputs.lock().unwrap().cycle / period.max(1);
                    if self.consumed_sample != Some(sample) {
                        0x80
                    } else {
                        0
                    }
                } else {
                    0
                }
            }
            Device::Imu if reg == 0x10 || reg == 0x11 => {
                let gravity = (16384u16 >> (self.regs[0x41] & 3)).to_le_bytes();
                if reg == 0x11 {
                    let odr = self.regs[0x40] & 15;
                    let period = 240_000_000u64 * 64 / (25u64 << odr);
                    self.consumed_sample = Some(self.inputs.lock().unwrap().cycle / period.max(1));
                }
                gravity[reg - 0x10]
            }
            _ => self.regs[reg],
        };
        self.pointer = self.pointer.wrapping_add(1);
        byte
    }
}

struct BusDevice(Arc<Mutex<Registers>>);
impl I2cDevice for BusDevice {
    fn pins(&self) -> Option<(u8, u8)> {
        Some((47, 48))
    }
    fn start(&mut self, read: bool) -> bool {
        self.0.lock().unwrap().start(read)
    }
    fn write(&mut self, byte: u8) -> bool {
        self.0.lock().unwrap().write(byte)
    }
    fn read(&mut self) -> u8 {
        self.0.lock().unwrap().read()
    }
}

pub struct StickS3 {
    panel: DcsPanel,
    levels: u64,
    command: u8,
    pub inputs: Arc<Mutex<Inputs>>,
    devices: [(u8, Arc<Mutex<Registers>>); 3],
}
impl StickS3 {
    pub fn new(inputs: Arc<Mutex<Inputs>>) -> Self {
        Self {
            panel: DcsPanel::st7789(),
            levels: u64::MAX,
            command: 0,
            devices: [
                (
                    0x6e,
                    Arc::new(Mutex::new(Registers::new(Device::Pmic, inputs.clone()))),
                ),
                (
                    0x68,
                    Arc::new(Mutex::new(Registers::new(Device::Imu, inputs.clone()))),
                ),
                (
                    0x18,
                    Arc::new(Mutex::new(Registers::new(Device::Codec, inputs.clone()))),
                ),
            ],
            inputs,
        }
    }
}
impl BoardModel for StickS3 {
    fn name(&self) -> &'static str {
        "stick-s3"
    }
    fn gpio_changes(&mut self, changes: &[(u8, bool)]) {
        for &(pin, high) in changes {
            if pin == 21 && !high {
                self.panel.reset();
            }
            if high {
                self.levels |= 1u64 << pin;
            } else {
                self.levels &= !(1u64 << pin);
            }
        }
        self.panel.dc = self.levels & (1 << 45) != 0;
    }
    fn gpio_output_at(&mut self, cycle: u64, changes: &[(u8, bool)], enabled: u64, output: u64) {
        self.gpio_changes(changes);
        if let Some(peer) = self.inputs.lock().unwrap().peer.as_mut() {
            peer.gpio_output(cycle, enabled, output);
        }
    }
    fn i2c_devices(&mut self) -> Vec<(u8, u8, Box<dyn I2cDevice>)> {
        self.devices
            .iter()
            .map(|(address, device)| {
                (
                    1,
                    *address,
                    Box::new(BusDevice(device.clone())) as Box<dyn I2cDevice>,
                )
            })
            .collect()
    }
    fn uses_spi_pins(&self) -> bool {
        true
    }
    fn spi_transfer_pins(&mut self, host: u8, pins: SpiPins, tx: &[u8], rx_len: usize) -> Vec<u8> {
        if host != 3
            || pins.sclk & (1 << 40) == 0
            || (!tx.is_empty() && pins.mosi & (1 << 39) == 0)
            || pins.cs & (1 << 41) == 0
        {
            return vec![0xff; rx_len];
        }
        for &byte in tx {
            if !self.panel.dc {
                self.command = byte;
            }
            self.panel.byte(byte);
        }
        let value = match self.command {
            0x0a => {
                (if !self.panel.sleeping { 0x10 } else { 0 }) | (if self.panel.on { 4 } else { 0 })
            }
            0x0b => self.panel.madctl,
            0x0c => self.panel.colmod,
            _ => 0,
        };
        vec![value; rx_len]
    }
    fn display(&self) -> Option<(u32, u32, Vec<u16>, u64)> {
        let mut pixels = Vec::with_capacity(135 * 240);
        for y in 40..280 {
            for x in 52..187 {
                pixels.push(if self.panel.on && !self.panel.sleeping {
                    self.panel.gram[y * 240 + x]
                } else {
                    0
                });
            }
        }
        Some((135, 240, pixels, self.panel.pixels_written))
    }
    fn gram(&self) -> Option<(Vec<u16>, usize, usize)> {
        Some((self.panel.gram.clone(), 240, 320))
    }
    fn display_version(&self) -> u64 {
        self.panel.pixels_written
    }
    fn named_pin(&self, name: &str) -> Option<u8> {
        match name {
            "M" => Some(11),
            "R" => Some(12),
            _ => None,
        }
    }
    fn flash_erase_at(&mut self, _cycle: u64, _offset: usize, _length: usize) {
        let mut state = self.inputs.lock().unwrap();
        if state.peer.as_ref().is_some_and(|peer| peer.reply_pending()) {
            state.reply_window_erases += 1;
        }
    }
    fn rmt_symbol_at(&mut self, cycle: u64, pin: u8, pulses: &[(u64, bool)]) {
        if pin != 46 || self.devices[0].1.lock().unwrap().regs[6] & 8 == 0 {
            return;
        }
        let mut state = self.inputs.lock().unwrap();
        if state.local_echo {
            if let Some(peer) = state.peer.as_mut() {
                peer.local_light(cycle, pulses);
            }
        }
    }
    fn rmt_waveform(&mut self, pin: u8, pulses: &[(u64, bool)]) {
        if pin == 46 {
            // The onboard transmitter draws from the PMIC's external 5 V rail.
            let powered = self.devices[0].1.lock().unwrap().regs[6] & 8 != 0;
            let light: Vec<(u64, bool)> = pulses
                .iter()
                .map(|&(cycles, high)| (cycles, high && powered))
                .collect();
            let mut state = self.inputs.lock().unwrap();
            let cycle = state.cycle;
            if let Some(peer) = state.peer.as_mut() {
                peer.observe_tx(cycle, &light);
            }
            state.optical.push(light);
        }
    }
    fn next_deadline(&self) -> Option<u64> {
        self.inputs
            .lock()
            .unwrap()
            .peer
            .as_ref()
            .and_then(|peer| peer.next_deadline())
    }
    fn uses_gpio_waveform(&self) -> bool {
        self.inputs.lock().unwrap().peer.is_some()
    }
    fn advance_to(&mut self, cycle: u64) {
        let mut state = self.inputs.lock().unwrap();
        state.cycle = cycle;
        if let Some(peer) = state.peer.as_mut() {
            peer.advance_to(cycle);
        }
    }
    fn take_edges(&mut self) -> Vec<BoardEdge> {
        self.inputs
            .lock()
            .unwrap()
            .peer
            .as_mut()
            .map_or_else(Vec::new, |peer| peer.take_edges())
    }
    fn report(&self) -> String {
        format!(
            "ST7789: {} pixels; on={}, sleeping={}; peripheral_supply={}",
            self.panel.pixels_written,
            self.panel.on,
            self.panel.sleeping,
            self.devices[0].1.lock().unwrap().regs[0x11] & 4 != 0
        )
    }
}
