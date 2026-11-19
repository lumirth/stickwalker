use anyhow::{Result, bail, ensure};
use serialport::SerialPort;
use std::{
    io::{self, Read, Write},
    sync::atomic::{AtomicU16, Ordering},
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
        Ok(Self::from_port(port))
    }

    pub(crate) fn from_port(port: Box<dyn SerialPort>) -> Self {
        Self {
            port,
            decoder: Decoder::default(),
            identified: false,
        }
    }
    pub fn request(&mut self, op: u8, payload: &[u8]) -> Result<Vec<u8>> {
        let timeout = if !self.identified {
            Duration::from_millis(350)
        } else {
            match (op, payload) {
                // Preparing, resetting, and the first EEPROM page can flush or erase flash.
                (PREPARE, _) | (WRITE, []) | (READ | WRITE, [0, 0, 0, 0, ..]) => {
                    Duration::from_secs(10)
                }
                _ => Duration::from_secs(2),
            }
        };
        let result = self.exchange(op, payload, timeout);
        if result.is_ok() {
            self.identified = true;
        }
        result
    }
    fn exchange(&mut self, op: u8, payload: &[u8], timeout: Duration) -> Result<Vec<u8>> {
        ensure!(
            payload.len() <= 4096,
            "The request to your Stick is too large."
        );
        static NEXT_SEQUENCE: AtomicU16 = AtomicU16::new(1);
        let sequence = NEXT_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let until = Instant::now() + timeout;
        send_frame(&mut self.port, &frame(op, sequence, payload), until)?;
        receive(&mut self.port, &mut self.decoder, sequence, op, until)
    }
}

fn send_frame(port: &mut impl Write, mut bytes: &[u8], until: Instant) -> io::Result<()> {
    while !bytes.is_empty() {
        if Instant::now() >= until {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Timed out sending to your Stick.",
            ));
        }
        match port.write(bytes) {
            // Windows can complete a timed-out WriteFile with zero bytes.
            Ok(0) => std::thread::sleep(Duration::from_millis(1)),
            Ok(n) => bytes = &bytes[n..],
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn receive(
    port: &mut impl Read,
    decoder: &mut Decoder,
    sequence: u16,
    op: u8,
    until: Instant,
) -> Result<Vec<u8>> {
    let mut buf = [0; 256];
    while Instant::now() < until {
        match port.read(&mut buf) {
            Ok(0) => return Err(std::io::Error::from(std::io::ErrorKind::UnexpectedEof).into()),
            Ok(n) => {
                for &b in &buf[..n] {
                    if let Some(raw) = decoder.push(b) {
                        if raw[2] != 1 || raw[3] != op || raw[4..6] != sequence.to_le_bytes() {
                            continue;
                        }
                        ensure!(raw.len() >= 13, "Your Stick sent an incomplete reply.");
                        match raw[8] {
                            0 => return Ok(raw[9..raw.len() - 4].to_vec()),
                            1 => {
                                bail!(
                                    "Your Stick is busy. If it’s linking, finish or cancel the link and try again."
                                )
                            }
                            2 => {
                                bail!("This operation is not supported by the connected firmware.")
                            }
                            _ => bail!("Couldn’t save changes to your Stick."),
                        }
                    }
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(e.into()),
        }
    }
    Err(std::io::Error::new(
        std::io::ErrorKind::TimedOut,
        "Stickwalker did not reply. Reconnect it and try again.",
    )
    .into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{self, Cursor};

    #[test]
    fn a_stalled_write_resumes_at_the_first_unsent_byte() {
        struct SerialWriter {
            count: usize,
            sent: Vec<u8>,
        }
        impl Write for SerialWriter {
            fn write(&mut self, data: &[u8]) -> io::Result<usize> {
                self.count += 1;
                match self.count {
                    2 => return Err(io::ErrorKind::TimedOut.into()),
                    3 => return Err(io::ErrorKind::Interrupted.into()),
                    4 => return Ok(0),
                    _ => {}
                }
                let n = data.len().min(317);
                self.sent.extend_from_slice(&data[..n]);
                Ok(n)
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let wire = frame(WRITE, 5, &[0xc0; CHUNK]);
        let mut port = SerialWriter {
            count: 0,
            sent: Vec::new(),
        };
        send_frame(&mut port, &wire, Instant::now() + Duration::from_secs(1)).unwrap();
        assert_eq!(port.sent, wire);
        let mut stalled = Cursor::new(&mut [][..]);
        let err = send_frame(
            &mut stalled,
            &wire,
            Instant::now() + Duration::from_millis(3),
        )
        .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::TimedOut);
    }

    #[test]
    fn unplug_ends_the_exchange_and_interrupted_reads_resume() {
        let mut decoder = Decoder::default();
        let until = Instant::now() + Duration::from_secs(1);
        let error = receive(&mut io::empty(), &mut decoder, 1, STATUS, until).unwrap_err();
        assert_eq!(
            error.downcast_ref::<io::Error>().unwrap().kind(),
            io::ErrorKind::UnexpectedEof
        );

        struct Interrupted(bool, Cursor<Vec<u8>>);
        impl Read for Interrupted {
            fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
                if std::mem::take(&mut self.0) {
                    return Err(io::ErrorKind::Interrupted.into());
                }
                self.1.read(out)
            }
        }
        let mut stream = Interrupted(true, Cursor::new(frame(STATUS, 7, &[0, 42])));
        assert_eq!(
            receive(&mut stream, &mut decoder, 7, STATUS, until).unwrap(),
            [42]
        );
    }
}
