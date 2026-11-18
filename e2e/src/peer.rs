use std::io::{self, BufRead, BufReader, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

pub struct Peer {
    child: Child,
    input: ChildStdin,
    output: BufReader<ChildStdout>,
    pub result: i32,
    pub connected: bool,
    last_frame: Option<u64>,
    clocked: bool,
}

impl Peer {
    pub fn open(
        program: &Path,
        mode: &str,
        eeprom: &[u8],
        course: Option<&[u8]>,
    ) -> io::Result<Self> {
        let mut child = Command::new(program)
            .args([mode, "15ba196b", "1"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()?;
        let input = child.stdin.take().unwrap();
        let output = BufReader::new(child.stdout.take().unwrap());
        let mut peer = Self {
            child,
            input,
            output,
            result: 1,
            connected: false,
            last_frame: None,
            clocked: std::env::var_os("STICK_CONNECTION").is_some(),
        };
        // The original status and record carry the trainer's pairing, language and game.
        let mut status = eeprom[0xed..0xed + 104].to_vec();
        if mode == "put" {
            let (country, version) = (status[..4].to_vec(), status[8..10].to_vec());
            status[4..8].copy_from_slice(&country);
            status[10..12].copy_from_slice(&version);
        }
        peer.payload("status", &status)?;
        peer.payload("record", &eeprom[0xcc00..0xcc00 + 548])?;
        if let Some(course) = course {
            peer.payload("course", course)?;
        }
        peer.exchange("S")?;
        Ok(peer)
    }

    fn payload(&mut self, name: &str, bytes: &[u8]) -> io::Result<()> {
        self.exchange(&format!("P {name} {}", hex(bytes)))
            .map(|_| ())
    }

    pub fn receive(&mut self, at_us: u64, wire: &[u8]) -> io::Result<Vec<Vec<u8>>> {
        let mut replies = Vec::new();
        if self.clocked {
            replies.extend(self.advance(at_us)?);
            replies.extend(self.exchange(&format!("F {}", hex(wire)))?);
            return Ok(replies);
        }
        if let Some(previous) = self.last_frame {
            let gap = at_us.saturating_sub(previous);
            // The bench adapter advances its clock by 4 ms on every F command.
            if gap > 4000 {
                replies.extend(self.exchange(&format!("T {}", gap - 4000))?);
            }
        }
        self.last_frame = Some(at_us);
        replies.extend(self.exchange(&format!("F {}", hex(wire)))?);
        Ok(replies)
    }

    pub fn advance(&mut self, at_us: u64) -> io::Result<Vec<Vec<u8>>> {
        let previous = self.last_frame.replace(at_us).unwrap_or(at_us);
        if at_us < previous {
            return Err(io::Error::other("Peer clock moved backwards"));
        }
        self.exchange(&format!("T {}", at_us - previous))
    }

    fn exchange(&mut self, command: &str) -> io::Result<Vec<Vec<u8>>> {
        writeln!(self.input, "{command}")?;
        let mut replies = Vec::new();
        loop {
            let mut line = String::new();
            if self.output.read_line(&mut line)? == 0 {
                return Err(io::Error::other("HGSS peer stopped"));
            }
            let line = line.trim_end();
            if line == "END" {
                return Ok(replies);
            }
            if let Some(wire) = line.strip_prefix("TX ") {
                let frame = (0..wire.len())
                    .step_by(2)
                    .map(|i| u8::from_str_radix(&wire[i..i + 2], 16).map_err(io::Error::other))
                    .collect::<io::Result<Vec<_>>>()?;
                replies.push(frame);
            }
            if let Some(state) = line.strip_prefix("STATE ") {
                let fields: Vec<_> = state.split_whitespace().collect();
                if fields.len() != 6 {
                    return Err(io::Error::other("Malformed HGSS peer state"));
                }
                self.result = fields[4].parse().map_err(io::Error::other)?;
                self.connected = fields[5] == "1";
            }
        }
    }
}

impl Drop for Peer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

pub fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}
