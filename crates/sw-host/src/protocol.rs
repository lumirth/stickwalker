use anyhow::{Result, bail, ensure};
use serialport::SerialPort;
use std::{
    io::{Read, Write},
    time::{Duration, Instant},
};

pub const STATUS: u8 = 1;
pub const SETTINGS: u8 = 2;
pub const READ: u8 = 3;
pub const WRITE: u8 = 4;
pub const PREPARE: u8 = 5;
pub const IDENTIFY: u8 = 6;
pub const SAVE_SIZE: usize = 65536;
pub const CHUNK: usize = 4080;

pub fn frame(op: u8, sequence: u16, payload: &[u8]) -> Vec<u8> {
    let mut raw = vec![b'S', b'W', 1, op];
    raw.extend(sequence.to_le_bytes());
    raw.extend((payload.len() as u16).to_le_bytes());
    raw.extend(payload);
    raw.extend(crc32fast::hash(&raw).to_le_bytes());
    let mut wire = vec![0xc0];
    for b in raw {
        match b {
            0xc0 => wire.extend([0xdb, 0xdc]),
            0xdb => wire.extend([0xdb, 0xdd]),
            _ => wire.push(b),
        }
    }
    wire.push(0xc0);
    wire
}

#[derive(Default)]
pub struct Decoder {
    bytes: Vec<u8>,
    escape: bool,
    discard: bool,
}
impl Decoder {
    pub fn push(&mut self, b: u8) -> Option<Vec<u8>> {
        if b == 0xc0 {
            let raw = std::mem::take(&mut self.bytes);
            let valid = !self.discard
                && !self.escape
                && raw.len() >= 12
                && raw[..2] == *b"SW"
                && raw[2] == 1
                && raw.len() == 12 + u16::from_le_bytes([raw[6], raw[7]]) as usize
                && crc32fast::hash(&raw[..raw.len() - 4])
                    == u32::from_le_bytes(raw[raw.len() - 4..].try_into().unwrap());
            self.escape = false;
            self.discard = false;
            return valid.then_some(raw);
        }
        if self.discard {
            return None;
        }
        let b = if self.escape {
            self.escape = false;
            match b {
                0xdc => 0xc0,
                0xdd => 0xdb,
                _ => {
                    self.discard = true;
                    return None;
                }
            }
        } else if b == 0xdb {
            self.escape = true;
            return None;
        } else {
            b
        };
        if self.bytes.len() >= 4108 {
            self.discard = true;
        } else {
            self.bytes.push(b);
        }
        None
    }
}

pub struct Session {
    port: Box<dyn SerialPort>,
    sequence: u16,
    decoder: Decoder,
    identified: bool,
}
impl Session {
    pub fn open(path: &str) -> Result<Self> {
        // Clearing DTR alone while RTS is asserted resets USB Serial/JTAG.
        // Passive discovery leaves both control lines as the driver opened them.
        let port = serialport::new(path, 115200)
            .timeout(Duration::from_millis(30))
            .preserve_dtr_on_open()
            .open()?;
        Ok(Self {
            port,
            sequence: 0,
            decoder: Decoder::default(),
            identified: false,
        })
    }
    pub fn request(&mut self, op: u8, payload: &[u8]) -> Result<Vec<u8>> {
        let timeout = if self.identified {
            Duration::from_secs(2)
        } else {
            Duration::from_millis(150)
        };
        let result = self.exchange(op, payload, timeout);
        if result.is_ok() {
            self.identified = true;
        }
        result
    }
    fn exchange(&mut self, op: u8, payload: &[u8], timeout: Duration) -> Result<Vec<u8>> {
        ensure!(payload.len() <= 4096, "Management message is too large");
        self.sequence = self.sequence.wrapping_add(1);
        self.port.write_all(&frame(op, self.sequence, payload))?;
        let until = Instant::now() + timeout;
        let mut buf = [0; 256];
        while Instant::now() < until {
            match self.port.read(&mut buf) {
                Ok(n) => {
                    for &b in &buf[..n] {
                        if let Some(raw) = self.decoder.push(b) {
                            if raw[2] != 1
                                || raw[3] != op
                                || raw[4..6] != self.sequence.to_le_bytes()
                            {
                                continue;
                            }
                            ensure!(raw.len() >= 13, "Empty management reply");
                            match raw[8] {
                                0 => return Ok(raw[9..raw.len() - 4].to_vec()),
                                1 => {
                                    bail!("Stickwalker is busy. Finish connecting, then try again.")
                                }
                                2 => bail!(
                                    "This operation is not supported by the connected firmware."
                                ),
                                _ => bail!(
                                    "Stickwalker could not save its state. Import an EEPROM image or reset the walker."
                                ),
                            }
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(e) => return Err(e.into()),
            }
        }
        bail!("Stickwalker did not reply. Reconnect it and try again.")
    }
}
