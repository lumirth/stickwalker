use esp_soc::board::BoardEdge;
use std::collections::VecDeque;

const CPU_HZ: u64 = 240_000_000;
const BAUD: u64 = 115_200;
const TURNAROUND: u64 = CPU_HZ / 1000;
const TIMEOUT: u64 = CPU_HZ / 10;
const TOKEN: [u8; 4] = [0x13, 0x57, 0x9b, 0xdf];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Phase {
    Probe,
    Acknowledgment,
    Status,
    Watts,
    PageWritten,
    PageRead,
    Shutdown,
    Complete,
    Failed,
}

#[derive(Clone, Copy)]
struct LightPulse {
    start: u64,
    end: u64,
}

struct Page {
    address: u16,
    bytes: Vec<u8>,
    write: bool,
}

pub struct DsPeer {
    phase: Phase,
    now: u64,
    session: [u8; 4],
    timeout: Option<u64>,
    transmit_end: u64,
    pulses: VecDeque<LightPulse>,
    output_enabled: bool,
    output_high: bool,
    charged: bool,
    edges: Vec<BoardEdge>,
    failure: Option<String>,
    trace: String,
    watts: Option<[u8; 2]>,
    page: Option<Page>,
    page_verified: bool,
    received_frames: usize,
    transmitted_frames: usize,
    charge_count: u64,
    discharge_count: u64,
}

impl DsPeer {
    pub fn new() -> Self {
        Self {
            phase: Phase::Probe,
            now: 0,
            session: [0; 4],
            timeout: None,
            transmit_end: 0,
            pulses: VecDeque::new(),
            output_enabled: false,
            output_high: false,
            charged: false,
            edges: Vec::new(),
            failure: None,
            trace: String::new(),
            watts: None,
            page: None,
            page_verified: false,
            received_frames: 0,
            transmitted_frames: 0,
            charge_count: 0,
            discharge_count: 0,
        }
    }

    pub fn with_page(address: u16, bytes: Vec<u8>, write: bool) -> Result<Self, String> {
        if address & 0x7f != 0 || bytes.len() != 128 {
            return Err("peer page requires an aligned address and exactly 128 bytes".into());
        }
        let mut peer = Self::new();
        peer.page = Some(Page {
            address,
            bytes,
            write,
        });
        Ok(peer)
    }

    fn fail(&mut self, reason: String) {
        if self.failure.is_none() {
            self.failure = Some(format!(
                "cycle={} phase={:?}: {reason}",
                self.now, self.phase
            ));
        }
        self.phase = Phase::Failed;
        self.timeout = None;
        self.pulses.clear();
    }

    fn edge(&mut self, cycle: u64, level: bool) {
        if self.charged != level {
            self.charged = level;
            self.edges.push(BoardEdge {
                cycle,
                pin: 5,
                level,
            });
            if !level {
                self.discharge_count += 1;
            }
        }
    }

    fn discharge_deadline(&self) -> Option<u64> {
        if self.output_enabled || !self.charged {
            return None;
        }
        self.pulses.front().map(|pulse| pulse.start.max(self.now))
    }

    pub fn gpio_output(&mut self, cycle: u64, enabled: u64, output: u64) {
        self.advance_to(cycle);
        let enabled = enabled & (1 << 5) != 0;
        let high = output & (1 << 5) != 0;
        if enabled && high && (!self.output_enabled || !self.output_high) {
            self.charge_count += 1;
        }
        self.output_enabled = enabled;
        self.output_high = high;
        if enabled {
            self.edge(cycle, high);
        } else if self.charged
            && self
                .pulses
                .front()
                .is_some_and(|pulse| pulse.start <= cycle)
        {
            self.edge(cycle, false);
        }
    }

    pub fn advance_to(&mut self, cycle: u64) {
        if cycle < self.now {
            self.fail("board clock reset without resetting peer session".into());
            return;
        }
        if let Some(discharge) = self.discharge_deadline() {
            if discharge <= cycle {
                self.edge(discharge, false);
            }
        }
        self.now = cycle;
        while self.pulses.front().is_some_and(|pulse| pulse.end <= cycle) {
            self.pulses.pop_front();
        }
        if self.phase == Phase::Shutdown && cycle >= self.transmit_end {
            self.phase = Phase::Complete;
            self.timeout = None;
        }
        if self.timeout.is_some_and(|deadline| cycle >= deadline) {
            self.fail("DS-side 100 ms response timeout".into());
        }
    }

    pub fn next_deadline(&self) -> Option<u64> {
        [
            self.discharge_deadline(),
            self.timeout,
            (self.phase == Phase::Shutdown).then_some(self.transmit_end),
        ]
        .into_iter()
        .flatten()
        .filter(|deadline| *deadline > self.now)
        .min()
    }

    pub fn take_edges(&mut self) -> Vec<BoardEdge> {
        std::mem::take(&mut self.edges)
    }

    fn send(&mut self, command: u8, argument: u8, token: [u8; 4], payload: &[u8]) {
        if self.now < self.transmit_end {
            self.fail("peer reply overlaps an unfinished transmission".into());
            return;
        }
        let mut packet = vec![command, argument, 0, 0];
        packet.extend_from_slice(&token);
        packet.extend_from_slice(payload);
        let checksum = checksum(&packet).to_le_bytes();
        packet[2..4].copy_from_slice(&checksum);
        self.trace += &format!("{} DS {}\n", self.now, hex(&packet));
        let start = self.now + TURNAROUND;
        self.pulses.clear();
        for (index, byte) in packet.iter().enumerate() {
            let wire = byte ^ 0xaa;
            for bit in 0..10 {
                let zero = bit == 0 || (bit < 9 && wire & (1 << (bit - 1)) == 0);
                if zero {
                    let start = start + (index as u64 * 10 + bit) * CPU_HZ / BAUD;
                    self.pulses.push_back(LightPulse {
                        start,
                        end: start + CPU_HZ * 3 / (BAUD * 16),
                    });
                }
            }
        }
        self.transmit_end = start + packet.len() as u64 * 10 * CPU_HZ / BAUD;
        self.timeout = Some(self.now + TIMEOUT);
        self.transmitted_frames += 1;
    }

    fn read_page(&mut self) {
        let address = self.page.as_ref().unwrap().address.to_be_bytes();
        self.phase = Phase::PageRead;
        self.send(0x0c, 2, self.session, &[address[0], address[1], 128]);
    }

    fn shutdown(&mut self) {
        self.phase = Phase::Shutdown;
        self.send(0xf4, 1, self.session, &[]);
    }

    pub fn observe_tx(&mut self, cycle: u64, pulses: &[(u64, bool)]) {
        self.advance_to(cycle);
        if self.phase == Phase::Failed {
            return;
        }
        let packet = match decode(pulses) {
            Ok(packet) => packet,
            Err(error) => {
                self.fail(error);
                return;
            }
        };
        self.received_frames += 1;
        self.trace += &format!("{} WALKER {}\n", cycle, hex(&packet));
        if self.now < self.transmit_end {
            self.fail("Walker transmission collides with DS transmission".into());
            return;
        }
        if packet == [0xfc] && self.phase == Phase::Probe {
            self.phase = Phase::Acknowledgment;
            self.send(0xfa, 1, TOKEN, &[]);
            return;
        }
        if packet.len() < 8 {
            self.fail(format!("expected packet header, received {}", hex(&packet)));
            return;
        }
        let received = u16::from_le_bytes([packet[2], packet[3]]);
        let mut checked = packet.clone();
        checked[2..4].fill(0);
        if checksum(&checked) != received {
            self.fail("invalid Walker packet checksum".into());
            return;
        }
        let command = packet[0];
        let argument = packet[1];
        let token: [u8; 4] = packet[4..8].try_into().unwrap();
        let payload = &packet[8..];
        if self.phase != Phase::Acknowledgment && token != self.session {
            self.fail("reply has a different session token".into());
            return;
        }
        match self.phase {
            Phase::Acknowledgment if command == 0xf8 && argument == 2 && payload.is_empty() => {
                for index in 0..4 {
                    self.session[index] = TOKEN[index] ^ token[index];
                }
                self.phase = Phase::Status;
                self.send(0x20, 1, self.session, &[]);
            }
            Phase::Status if command == 0x22 && argument == 2 && payload.len() == 104 => {
                self.phase = Phase::Watts;
                self.send(0x0c, 2, self.session, &[0xce, 0x8a, 2]);
            }
            Phase::Watts if command == 0x0e && argument == 2 && payload.len() == 2 => {
                self.watts = Some(payload.try_into().unwrap());
                if payload != [0, 100] {
                    self.fail(format!(
                        "expected fixture Watts 0064, received {}",
                        hex(payload)
                    ));
                    return;
                }
                if let Some(page) = &self.page {
                    if page.write {
                        let address = page.address;
                        let bytes = page.bytes.clone();
                        self.phase = Phase::PageWritten;
                        self.send(
                            0x02 | (address as u8 & 0x80),
                            (address >> 8) as u8,
                            self.session,
                            &bytes,
                        );
                    } else {
                        self.read_page();
                    }
                } else {
                    self.shutdown();
                }
            }
            Phase::PageWritten
                if command == 4
                    && payload.is_empty()
                    && argument == (self.page.as_ref().unwrap().address >> 8) as u8 =>
            {
                self.read_page();
            }
            Phase::PageRead if command == 0x0e && argument == 2 && payload.len() == 128 => {
                if payload != self.page.as_ref().unwrap().bytes {
                    self.fail(format!("page readback differs: {}", hex(payload)));
                    return;
                }
                self.page_verified = true;
                self.shutdown();
            }
            _ => self.fail(format!(
                "unexpected command {command:02x}, argument {argument:02x}, payload length {}",
                payload.len()
            )),
        }
    }

    pub fn complete(&self) -> bool {
        self.phase == Phase::Complete
    }

    pub fn error(&self) -> Option<&str> {
        self.failure.as_deref()
    }

    pub fn report(&self) -> String {
        format!(
            "model=ideal-sir-charge-discharge\nphase={:?}\ncomplete={}\nerror={}\nwatts={}\npage_verified={}\nreceived_frames={}\ntransmitted_frames={}\ncharges={}\ndischarges={}\n{}",
            self.phase, self.complete(), self.error().unwrap_or("none"),
            self.watts.as_ref().map_or_else(|| "none".into(), |bytes| hex(bytes)),
            self.page_verified, self.received_frames, self.transmitted_frames,
            self.charge_count, self.discharge_count, self.trace,
        )
    }
}

fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = 2u32;
    for (index, byte) in bytes.iter().enumerate() {
        sum += u32::from(*byte) << if index & 1 == 0 { 8 } else { 0 };
    }
    sum = (sum & 0xffff) + (sum >> 16);
    sum += sum >> 16;
    sum as u16
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn decode(pulses: &[(u64, bool)]) -> Result<Vec<u8>, String> {
    let duration: u64 = pulses.iter().map(|(cycles, _)| cycles).sum();
    let cells = (duration * BAUD + CPU_HZ / 2) / CPU_HZ;
    if cells == 0 || cells % 10 != 0 || cells > 136 * 10 {
        return Err(format!("invalid Walker UART duration: {duration} cycles"));
    }
    let mut bits = vec![true; cells as usize];
    let mut elapsed = 0;
    for &(cycles, high) in pulses {
        if high {
            let index = (elapsed * BAUD + CPU_HZ / 2) / CPU_HZ;
            let phase_error = (i128::from(elapsed) * i128::from(BAUD)
                - i128::from(index) * i128::from(CPU_HZ))
            .abs();
            if index >= cells
                || phase_error > i128::from(CPU_HZ / 20)
                || cycles * BAUD < CPU_HZ / 10
                || cycles * BAUD > CPU_HZ * 3 / 10
            {
                return Err(format!("invalid Walker SIR light pulse at cycle {elapsed}"));
            }
            bits[index as usize] = false;
        }
        elapsed += cycles;
    }
    let mut result = Vec::new();
    for byte in bits.chunks_exact(10) {
        if byte[0] || !byte[9] {
            return Err("invalid Walker UART start/stop bits".into());
        }
        let wire = byte[1..9]
            .iter()
            .enumerate()
            .fold(0, |value, (bit, high)| value | (u8::from(*high) << bit));
        result.push(wire ^ 0xaa);
    }
    Ok(result)
}
