use esp_soc::board::BoardEdge;
use std::collections::VecDeque;

const CPU_HZ: u64 = 240_000_000;
const BAUD: u64 = 115_200;
const TURNAROUND: u64 = CPU_HZ / 1000;
const TIMEOUT: u64 = CPU_HZ / 10;
const FIXTURE_WATTS: [u8; 2] = [0, 100];
const TOKEN: [u8; 4] = [0x13, 0x57, 0x9b, 0xdf];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Phase {
    Probe,
    Acknowledgment,
    Status,
    Watts,
    PutAccepted,
    PutPage,
    PutRead,
    PutPing,
    PutCommitted,
    WalkerStatus,
    WalkerWrite,
    WalkerRead,
    WalkerInfo,
    WalkerStart,
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

// Independent continuous-time circuit model. Inputs are volts, ohms, farads,
// and amperes; it has no knowledge of gates, sample values or decoder cutoffs.
// A high GPIO recharges the node; light removes charge while it floats, and
// enabled internal pulls add their resistor current during readout.
struct Circuit {
    resistance: f64,
    capacitance: f64,
    threshold: f64,
    signal: f64,
    background: f64,
    voltage: f64,
    pull_up: bool,
    pull_down: bool,
}

impl Circuit {
    fn new(resistance: f64, capacitance_pf: f64, threshold: f64, signal_ua: f64, background_ua: f64) -> Self {
        assert!(resistance > 0.0 && capacitance_pf > 0.0);
        assert!((0.0..3.3).contains(&threshold));
        assert!(signal_ua >= 0.0 && background_ua >= 0.0);
        Self {
            resistance,
            capacitance: capacitance_pf * 1e-12,
            threshold,
            signal: signal_ua * 1e-6,
            background: background_ua * 1e-6,
            voltage: 3.3,
            pull_up: false,
            pull_down: false,
        }
    }

    fn conductance(&self) -> f64 {
        (u8::from(self.pull_up) + u8::from(self.pull_down)) as f64 / self.resistance
    }

    fn current(&self, illuminated: bool) -> f64 {
        self.background + if illuminated { self.signal } else { 0.0 }
    }

    fn target(&self, illuminated: bool) -> f64 {
        ((if self.pull_up { 3.3 / self.resistance } else { 0.0 })
            - self.current(illuminated)) / self.conductance()
    }

    fn advance(&mut self, cycles: u64, illuminated: bool) {
        let seconds = cycles as f64 / CPU_HZ as f64;
        let g = self.conductance();
        self.voltage = if g == 0.0 {
            self.voltage - self.current(illuminated) * seconds / self.capacitance
        } else {
            let target = self.target(illuminated);
            target + (self.voltage - target) * (-seconds * g / self.capacitance).exp()
        }.clamp(0.0, 3.3);
    }

    fn crossing(&self, illuminated: bool) -> Option<u64> {
        let current = self.current(illuminated);
        let g = self.conductance();
        let seconds = if g == 0.0 {
            if current == 0.0 || self.voltage <= self.threshold { return None; }
            self.capacitance * (self.voltage - self.threshold) / current
        } else {
            let target = self.target(illuminated);
            if (self.voltage > self.threshold) == (target >= self.threshold) { return None; }
            -self.capacitance / g * ((self.threshold - target) / (self.voltage - target)).ln()
        };
        seconds.is_finite().then_some((seconds * CPU_HZ as f64).ceil().max(1.0) as u64)
    }
}

struct Page {
    address: u16,
    bytes: Vec<u8>,
    write: bool,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum ConsoleOperation {
    Start,
    Update,
    Return,
}

struct Put {
    operation: ConsoleOperation,
    image: Vec<u8>,
    image_offset: usize,
    compressed: bool,
    bytes: Vec<u8>,
    offset: usize,
    records: Vec<u8>,
    record_offset: usize,
    last_address: u16,
    read_segment: usize,
    read_offset: usize,
    read_count: usize,
}

// The matching firmware exchanges animation, name, trainer-house records and
// PeerInfo in this order. Addresses and sizes come from its public record map.
const WALKER_SEGMENTS: [(usize, usize, usize); 3] = [
    (0x91be, 0xf400, 384),
    (0x993e, 0xf580, 320),
    (0xcc00, 0xdc00, 548),
];

impl Put {
    fn reads(&self) -> &'static [(u16, usize)] {
        if self.operation == ConsoleOperation::Return {
            &[(0x8f00, 42), (0xce80, 3404), (0xb800, 1736)]
        } else {
            &[(0xce80, 3404), (0xb800, 1736)]
        }
    }
}

struct Walker {
    image: Vec<u8>,
    expected: Vec<u8>,
    segment: usize,
    offset: usize,
    chunk: usize,
}

pub struct OpticalPeer {
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
    pub reply_limit: Option<usize>,
    charge_count: u64,
    discharge_count: u64,
    last_charge_cycle: u64,
    last_reply_cycle: u64,
    baud: u64,
    turnaround: u64,
    echoes: VecDeque<LightPulse>,
    put: Option<Put>,
    status: Vec<u8>,
    walker: Option<Walker>,
    malformed: Option<u8>,
    pub noise: u8,
    circuit: Option<Circuit>,
    pub byte_gap_cycles: u64,
    pub phase_cycles: u64,
    pub pulse_cycles: Option<u64>,
    pub keepalive_prefix: bool,
    pub keepalive_only: bool,
    pub delayed_frame: Option<(u8, u64)>,
    injected_keepalives: usize,
    pin_trace: Option<Vec<String>>,
}

impl OpticalPeer {
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
            reply_limit: None,
            charge_count: 0,
            discharge_count: 0,
            last_charge_cycle: 0,
            last_reply_cycle: 0,
            baud: BAUD,
            turnaround: TURNAROUND,
            echoes: VecDeque::new(),
            put: None,
            status: Vec::new(),
            walker: None,
            malformed: None,
            noise: 0,
            circuit: None,
            byte_gap_cycles: 0,
            phase_cycles: 0,
            pulse_cycles: None,
            keepalive_prefix: false,
            keepalive_only: false,
            delayed_frame: None,
            injected_keepalives: 0,
            pin_trace: std::env::var_os("STICK_E2E_PIN_TRACE").map(|_| Vec::new()),
        }
    }

    pub fn with_malformed(image: Vec<u8>, mode: u8) -> Self {
        let mut peer = Self::with_page(0xf600, image[0xf600..0xf680].to_vec(), false).unwrap();
        peer.malformed = Some(mode);
        peer
    }

    pub fn timing(mut self, baud: u64, turnaround_us: u64) -> Self {
        assert!((110_000..=120_000).contains(&baud));
        self.baud = baud;
        self.turnaround = turnaround_us * (CPU_HZ / 1_000_000);
        self
    }

    pub fn circuit(&mut self, resistance: f64, capacitance_pf: f64, threshold: f64, signal_ua: f64, background_ua: f64) {
        self.circuit = Some(Circuit::new(resistance, capacitance_pf, threshold, signal_ua, background_ua));
    }

    pub fn with_put(bytes: Vec<u8>) -> Self {
        assert_eq!(bytes.len(), 10_430);
        let mut peer = Self::new();
        peer.put = Some(Put {
            operation: ConsoleOperation::Update,
            image: Vec::new(),
            image_offset: 0,
            compressed: false,
            bytes,
            offset: 0,
            records: vec![0; 548],
            record_offset: 0,
            last_address: 0,
            read_segment: 0,
            read_offset: 0,
            read_count: 0,
        });
        peer
    }

    pub fn with_initial_put(bytes: Vec<u8>, image: Vec<u8>) -> Self {
        let mut peer = Self::with_put(bytes);
        let put = peer.put.as_mut().unwrap();
        put.operation = ConsoleOperation::Start;
        put.image = image[0x280..0x8ed0].to_vec();
        peer
    }

    pub fn with_return() -> Self {
        let mut peer = Self::with_put(vec![0; 10430]);
        peer.put.as_mut().unwrap().operation = ConsoleOperation::Return;
        peer
    }

    pub fn with_compressed_put(bytes: Vec<u8>) -> Self {
        let mut peer = Self::with_put(bytes);
        peer.put.as_mut().unwrap().compressed = true;
        peer
    }

    pub fn with_walker(mut image: Vec<u8>) -> Self {
        assert_eq!(image.len(), 65536);
        let expected = image.clone();
        let mut identity = [0; 40];
        identity[..21].copy_from_slice(b"PUBLIC OPTICAL WALKER");
        image[0xed + 16..0xed + 56].copy_from_slice(&identity);
        image[0xcc08..0xcc30].copy_from_slice(&identity);
        for &(source, _, length) in &WALKER_SEGMENTS[..2] {
            for byte in &mut image[source..source + length] {
                *byte ^= 0x5a;
            }
        }
        let mut peer = Self::new();
        peer.walker = Some(Walker {
            image,
            expected,
            segment: 0,
            offset: 0,
            chunk: 0,
        });
        peer
    }

    fn walker_write(&mut self) {
        let walker = self.walker.as_mut().unwrap();
        if walker.segment == WALKER_SEGMENTS.len() {
            walker.segment = 0;
            walker.offset = 0;
            self.walker_read();
            return;
        }
        let (source, destination, length) = WALKER_SEGMENTS[walker.segment];
        walker.chunk = (length - walker.offset).min(128);
        let bytes =
            walker.image[source + walker.offset..source + walker.offset + walker.chunk].to_vec();
        let address = destination + walker.offset;
        self.phase = Phase::WalkerWrite;
        self.send(
            2 | (address as u8 & 0x80),
            (address >> 8) as u8,
            self.session,
            &bytes,
        );
    }

    fn walker_read(&mut self) {
        let walker = self.walker.as_mut().unwrap();
        if walker.segment == WALKER_SEGMENTS.len() {
            let mut info = vec![0; 56];
            info[..4].copy_from_slice(&1000u32.to_be_bytes());
            info[4..6].copy_from_slice(&10u16.to_be_bytes());
            info[14..16].copy_from_slice(&25u16.to_le_bytes());
            self.phase = Phase::WalkerInfo;
            self.send(0x14, 2, self.session, &info);
            return;
        }
        let (source, _, length) = WALKER_SEGMENTS[walker.segment];
        walker.chunk = (length - walker.offset).min(128);
        let address = (source + walker.offset) as u16;
        let count = walker.chunk as u8;
        self.phase = Phase::WalkerRead;
        self.send(
            0x0c,
            2,
            self.session,
            &[(address >> 8) as u8, address as u8, count],
        );
    }

    fn walker_advance(&mut self) {
        let walker = self.walker.as_mut().unwrap();
        walker.offset += walker.chunk;
        if walker.offset == WALKER_SEGMENTS[walker.segment].2 {
            walker.segment += 1;
            walker.offset = 0;
        }
    }

    fn put_page(&mut self) {
        let put = self.put.as_mut().unwrap();
        if put.image_offset == put.image.len()
            && put.offset == put.bytes.len()
            && put.record_offset == put.records.len()
        {
            self.put_read();
        } else {
            let (address, bytes) = if put.image_offset < put.image.len() {
                let address = 0x280 + put.image_offset;
                let end = (put.image_offset + 128).min(put.image.len());
                let bytes = put.image[put.image_offset..end].to_vec();
                put.image_offset = end;
                (address, bytes)
            } else if put.offset < put.bytes.len() {
                let address = 0xd700 + put.offset;
                let end = (put.offset + 128).min(put.bytes.len());
                let bytes = put.bytes[put.offset..end].to_vec();
                put.offset = end;
                (address, bytes)
            } else {
                let address = 0xd480 + put.record_offset;
                let end = (put.record_offset + 128).min(put.records.len());
                let bytes = put.records[put.record_offset..end].to_vec();
                put.record_offset = end;
                (address, bytes)
            };
            put.last_address = address as u16;
            let mut command = 0x02 | (address as u8 & 0x80);
            let mut bytes = bytes;
            if put.compressed {
                // HGSS compresses complete pages independently. Its size table
                // marks the final partial page as an uncompressed 128-byte page.
                let complete_page = bytes.len() == 128;
                bytes.resize(128, 0);
                let packed = compress_lz_page(&bytes);
                if complete_page && packed.len() < 128 {
                    command &= 0x80;
                    bytes = packed;
                }
            }
            self.phase = Phase::PutPage;
            self.send(command, (address >> 8) as u8, self.session, &bytes);
        }
    }

    fn put_read(&mut self) {
        // HGSS phc_lib.c: course, trainer record, PHCData, distribution data,
        // checksum/ping, put commit and one-way shutdown.
        let put = self.put.as_mut().unwrap();
        let reads = put.reads();
        if put.read_segment == reads.len() {
            self.phase = Phase::PutPing;
            self.send(0x24, 1, self.session, &[]);
        } else {
            let (base, length) = reads[put.read_segment];
            let address = base + put.read_offset as u16;
            put.read_count = (length - put.read_offset).min(128);
            let count = put.read_count as u8;
            self.phase = Phase::PutRead;
            self.send(
                0x0c,
                2,
                self.session,
                &[(address >> 8) as u8, address as u8, count],
            );
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
        if let Some(circuit) = self.circuit.as_ref() {
            if self.output_enabled { return None; }
            let transition = self.light_boundary();
            let crossing = circuit.crossing(self.illuminated()).map(|after| self.now + after);
            return [transition, crossing].into_iter().flatten().min();
        }
        if self.output_enabled || !self.charged {
            return None;
        }
        [self.pulses.front(), self.echoes.front()]
            .into_iter()
            .flatten()
            .map(|pulse| pulse.start.max(self.now))
            .min()
    }

    fn illuminated(&self) -> bool {
        [self.pulses.front(), self.echoes.front()]
            .into_iter().flatten()
            .any(|pulse| pulse.start <= self.now && pulse.end > self.now)
    }

    fn light_boundary(&self) -> Option<u64> {
        [self.pulses.front(), self.echoes.front()]
            .into_iter().flatten()
            .map(|pulse| if pulse.start > self.now { pulse.start } else { pulse.end })
            .filter(|cycle| *cycle > self.now)
            .min()
    }

    pub fn gpio_pulls(&mut self, cycle: u64, up: bool, down: bool) {
        self.advance_to(cycle);
        if let Some(trace) = self.pin_trace.as_mut() {
            if trace.len() < 8192 { trace.push(format!("PIN {cycle} pulls {} {}", u8::from(up), u8::from(down))); }
        }
        if let Some(circuit) = self.circuit.as_mut() {
            circuit.pull_up = up;
            circuit.pull_down = down;
        }
    }

    pub fn reply_pending(&self) -> bool {
        !matches!(
            self.phase,
            Phase::Probe | Phase::Shutdown | Phase::Complete | Phase::Failed
        )
    }

    pub fn local_light(&mut self, cycle: u64, pulses: &[(u64, bool)]) {
        let mut start = cycle;
        for &(duration, high) in pulses {
            if high && duration != 0 {
                self.echoes.push_back(LightPulse {
                    start,
                    end: start + duration,
                });
            }
            start += duration;
        }
    }

    pub fn gpio_output(&mut self, cycle: u64, enabled: u64, output: u64) {
        self.advance_to(cycle);
        let enabled = enabled & (1 << 5) != 0;
        let high = output & (1 << 5) != 0;
        if enabled != self.output_enabled || high != self.output_high {
            if let Some(trace) = self.pin_trace.as_mut() {
                if trace.len() < 8192 { trace.push(format!("PIN {cycle} output {} {}", u8::from(enabled), u8::from(high))); }
            }
        }
        if enabled && high && (!self.output_enabled || !self.output_high) {
            self.charge_count += 1;
            self.last_charge_cycle = cycle;
        }
        self.output_enabled = enabled;
        self.output_high = high;
        if let Some(circuit) = self.circuit.as_mut() {
            if enabled { circuit.voltage = if high { 3.3 } else { 0.0 }; }
            let level = circuit.voltage > circuit.threshold;
            self.edge(cycle, level);
            return;
        }
        if enabled {
            self.edge(cycle, high);
        } else if self.charged
            && [self.pulses.front(), self.echoes.front()]
                .into_iter()
                .flatten()
                .any(|pulse| pulse.start <= cycle && pulse.end > cycle)
        {
            self.edge(cycle, false);
        }
    }

    pub fn advance_to(&mut self, cycle: u64) {
        if cycle < self.now {
            self.fail("board clock reset without resetting peer session".into());
            return;
        }
        if self.circuit.is_some() {
            while self.now < cycle {
                let illuminated = self.illuminated();
                let until = self.discharge_deadline().unwrap_or(cycle).min(cycle);
                let circuit = self.circuit.as_mut().unwrap();
                if !self.output_enabled { circuit.advance(until - self.now, illuminated); }
                let high = circuit.voltage > circuit.threshold;
                self.now = until;
                self.edge(until, high);
                while self.pulses.front().is_some_and(|pulse| pulse.end <= self.now) {
                    self.pulses.pop_front();
                }
                while self.echoes.front().is_some_and(|pulse| pulse.end <= self.now) {
                    self.echoes.pop_front();
                }
            }
        } else if let Some(discharge) = self.discharge_deadline() {
            if discharge <= cycle {
                self.edge(discharge, false);
            }
        }
        self.now = cycle;
        while self.pulses.front().is_some_and(|pulse| pulse.end <= cycle) {
            self.pulses.pop_front();
        }
        while self.echoes.front().is_some_and(|pulse| pulse.end <= cycle) {
            self.echoes.pop_front();
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
        self.send_raw(&packet);
    }

    fn send_raw(&mut self, packet: &[u8]) {
        if self
            .reply_limit
            .is_some_and(|limit| self.transmitted_frames >= limit)
        {
            self.timeout = None;
            return;
        }
        self.trace += &format!(
            "{} {} {}\n",
            self.now,
            if self.walker.is_some() { "PEER" } else { "DS" },
            hex(&packet)
        );
        let delay = match self.delayed_frame {
            Some((command, us)) if packet[0] == command => {
                self.delayed_frame = None;
                Some(us * (CPU_HZ / 1_000_000))
            }
            _ => None,
        };
        let mut start = self.now + delay.unwrap_or(self.turnaround) + self.phase_cycles;
        self.pulses.clear();
        if self.noise != 0 {
            for lead_us in [500, if self.noise == 2 { 87 } else { 26 }] {
                self.pulses.push_back(LightPulse {
                    start: start - lead_us * (CPU_HZ / 1_000_000),
                    end: start - lead_us * (CPU_HZ / 1_000_000) + 400,
                });
            }
        }
        // Ignored keepalives have their own valid framing, token and checksum.
        // They leave the peer's turn open; no response is expected for them.
        if packet[0] != 0xfa && (self.keepalive_prefix || self.keepalive_only) {
            let mut keepalive = vec![0x2e, 2, 0, 0];
            keepalive.extend_from_slice(&packet[4..8]);
            let sum = checksum(&keepalive).to_le_bytes();
            keepalive[2..4].copy_from_slice(&sum);
            let count = if self.keepalive_only { 100 } else { 1 };
            for _ in 0..count {
                let end = self.append_wire_packet(start, &keepalive);
                start = end + CPU_HZ / 10_000; // 100 us physical frame separation
            }
            self.injected_keepalives += count;
            self.transmitted_frames += count;
            if self.keepalive_only {
                self.transmit_end = start - CPU_HZ / 10_000;
                self.timeout = Some(self.now + TIMEOUT);
                return;
            }
        }
        self.transmit_end = self.append_wire_packet(start, packet);
        if delay.is_some() {
            self.trace += &format!("{start} DELAYED {:02x} {}\n", packet[0], self.transmit_end);
        }
        self.timeout = Some(self.now + TIMEOUT);
        self.transmitted_frames += 1;
    }

    fn append_wire_packet(&mut self, start: u64, packet: &[u8]) -> u64 {
        for (index, byte) in packet.iter().enumerate() {
            let wire = byte ^ 0xaa;
            for bit in 0..10 {
                let zero = bit == 0 || (bit < 9 && wire & (1 << (bit - 1)) == 0);
                if zero {
                    let start = start + (index as u64 * 10 + bit) * CPU_HZ / self.baud
                        + index as u64 * self.byte_gap_cycles;
                    self.pulses.push_back(LightPulse {
                        start,
                        end: start + self.pulse_cycles.unwrap_or(CPU_HZ * 3 / (self.baud * 16)),
                    });
                }
            }
        }
        start + packet.len() as u64 * 10 * CPU_HZ / self.baud
            + packet.len().saturating_sub(1) as u64 * self.byte_gap_cycles
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
        self.last_reply_cycle = cycle;
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
            self.send(0xfa, if self.walker.is_some() { 2 } else { 1 }, TOKEN, &[]);
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
                if let Some(walker) = &self.walker {
                    let status = walker.image[0xed..0xed + 104].to_vec();
                    self.phase = Phase::WalkerStatus;
                    self.send(0x10, 2, self.session, &status);
                } else {
                    self.phase = Phase::Status;
                    self.send(0x20, 1, self.session, &[]);
                }
            }
            Phase::Status if command == 0x22 && argument == 2 && payload.len() == 104 => {
                self.status = payload.to_vec();
                if let Some(put) = self.put.as_mut() {
                    put.records[8..48].copy_from_slice(&payload[16..56]);
                }
                self.phase = Phase::Watts;
                self.send(0x0c, 2, self.session, &[0xce, 0x8a, 2]);
            }
            Phase::Watts if command == 0x0e && argument == 2 && payload.len() == 2 => {
                self.watts = Some(payload.try_into().unwrap());
                if payload != FIXTURE_WATTS {
                    self.fail(format!(
                        "expected fixture Watts 0064, received {}",
                        hex(payload)
                    ));
                    return;
                }
                if self.put.is_some() {
                    self.phase = Phase::PutAccepted;
                    let command = match self.put.as_ref().unwrap().operation {
                        ConsoleOperation::Start => 0x32,
                        ConsoleOperation::Update => 0x52,
                        ConsoleOperation::Return => 0x40,
                    };
                    self.send(command, 1, self.session, &self.status.clone());
                } else if let Some(mode) = self.malformed.take() {
                    match mode {
                        0 => self.send_raw(&[0x02, 0xf6, 0xf8, 0x02]),
                        1 => self.send(0x0a, 0xf6, self.session, &[]),
                        2 => self.send(0x0c, 2, self.session, &[0xf6, 0, 200]),
                        3 => self.send(0x06, 0, self.session, &[0, 42]),
                        _ => self.send(0x10, 2, self.session, &[0; 4]),
                    }
                    let mut invalid = std::mem::take(&mut self.pulses);
                    let previous = self.now;
                    self.now = self.transmit_end + CPU_HZ / 500;
                    self.read_page();
                    self.now = previous;
                    invalid.append(&mut self.pulses);
                    self.pulses = invalid;
                } else if let Some(page) = &self.page {
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
            Phase::PutAccepted
                if argument == 2
                    && payload.is_empty()
                    && command
                        == match self.put.as_ref().unwrap().operation {
                            ConsoleOperation::Start => 0x34,
                            ConsoleOperation::Update => 0x54,
                            ConsoleOperation::Return => 0x42,
                        } =>
            {
                if self.put.as_ref().unwrap().operation == ConsoleOperation::Return {
                    self.put_read();
                } else {
                    self.put_page();
                }
            }
            Phase::PutPage if command == 4 && payload.is_empty() => {
                if argument != (self.put.as_ref().unwrap().last_address >> 8) as u8 {
                    self.fail("put page acknowledgment has the wrong address".into());
                    return;
                }
                self.put_page();
            }
            Phase::PutCommitted
                if argument == 2
                    && payload.is_empty()
                    && command
                        == match self.put.as_ref().unwrap().operation {
                            ConsoleOperation::Start => 0x38,
                            ConsoleOperation::Update => 0x5a,
                            ConsoleOperation::Return => 0x50,
                        } =>
            {
                self.shutdown();
            }
            Phase::PutRead if command == 0x0e && argument == 2 => {
                let put = self.put.as_mut().unwrap();
                if payload.len() != put.read_count {
                    self.fail("DS PHC/distribution read has the wrong length".into());
                    return;
                }
                if put.reads()[put.read_segment].0 == 0xce80
                    && put.read_offset == 0
                    && payload[10..12] != FIXTURE_WATTS
                {
                    self.fail("DS PHCData contains incorrectly ordered Watts".into());
                    return;
                }
                put.read_offset += put.read_count;
                let length = put.reads()[put.read_segment].1;
                if put.read_offset == length {
                    put.read_segment += 1;
                    put.read_offset = 0;
                }
                self.put_read();
            }
            Phase::PutPing if command == 0x26 && argument == 2 && payload.is_empty() => {
                self.phase = Phase::PutCommitted;
                let command = match self.put.as_ref().unwrap().operation {
                    ConsoleOperation::Start => 0x38,
                    ConsoleOperation::Update => 0x5a,
                    ConsoleOperation::Return => 0x4e,
                };
                self.send(command, 1, self.session, &[]);
            }
            Phase::WalkerStatus if command == 0x12 && argument == 2 && payload.len() == 104 => {
                if payload[91] & 3 != 3 {
                    self.fail("Walker peer play requires registration and a companion".into());
                    return;
                }
                self.walker_write();
            }
            Phase::WalkerWrite if command == 4 && payload.is_empty() => {
                let walker = self.walker.as_ref().unwrap();
                let address = WALKER_SEGMENTS[walker.segment].1 + walker.offset;
                if argument != (address >> 8) as u8 {
                    self.fail("peer page acknowledgment has the wrong address".into());
                    return;
                }
                self.walker_advance();
                self.walker_write();
            }
            Phase::WalkerRead if command == 0x0e && argument == 2 => {
                let walker = self.walker.as_ref().unwrap();
                let source = WALKER_SEGMENTS[walker.segment].0 + walker.offset;
                if payload != &walker.expected[source..source + walker.chunk] {
                    self.fail("peer animation/name/records differ from the public fixture".into());
                    return;
                }
                self.walker_advance();
                self.walker_read();
            }
            Phase::WalkerInfo if command == 0x14 && argument == 2 && payload.len() == 56 => {
                self.phase = Phase::WalkerStart;
                self.send(0x16, 2, self.session, &[]);
            }
            Phase::WalkerStart if command == 0x16 && argument == 2 && payload.is_empty() => {
                self.phase = Phase::Complete;
                self.timeout = None;
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
        let model = match self.circuit.as_ref() {
            Some(circuit) => format!(
                "continuous-rc-photocurrent\nresistance_ohm={}\ncapacitance_pf={}\nthreshold_volts={}\nsignal_ua={}\nbackground_ua={}",
                circuit.resistance, circuit.capacitance * 1e12, circuit.threshold,
                circuit.signal * 1e6, circuit.background * 1e6,
            ),
            None => "ideal-sir-charge-discharge".into(),
        };
        let mut report = format!(
            "model={}\nphase_cycles={}\npulse_cycles={}\nbyte_gap_cycles={}\nphase={:?}\ncomplete={}\nerror={}\nwatts={}\npage_verified={}\nreceived_frames={}\ntransmitted_frames={}\ncharges={}\ndischarges={}\nlast_charge_cycle={}\nlast_reply_cycle={}\ninjected_keepalives={}\n{}",
            model, self.phase_cycles, self.pulse_cycles.unwrap_or(CPU_HZ * 3 / (self.baud * 16)),
            self.byte_gap_cycles, self.phase, self.complete(), self.error().unwrap_or("none"),
            self.watts.as_ref().map_or_else(|| "none".into(), |bytes| hex(bytes)),
            self.page_verified, self.received_frames, self.transmitted_frames,
            self.charge_count, self.discharge_count, self.last_charge_cycle,
            self.last_reply_cycle, self.injected_keepalives, self.trace,
        );
        if let Some(trace) = self.pin_trace.as_ref() {
            report.push_str(&trace.join("\n"));
            report.push('\n');
        }
        report
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

// Nintendo LZ10: an advertised 128-byte page, MSB-first flags, and 3..18-byte
// phrases from earlier output. The peer chooses raw transfer when it grows.
fn compress_lz_page(bytes: &[u8]) -> Vec<u8> {
    assert_eq!(bytes.len(), 128);
    let mut packed = vec![0x10, 128, 0, 0];
    let mut cursor = 0;
    while cursor < bytes.len() {
        let flags = packed.len();
        packed.push(0);
        for bit in 0..8 {
            if cursor == bytes.len() {
                break;
            }
            let mut best_length = 0;
            let mut best_distance = 0;
            for distance in 1..=cursor {
                let mut length = 0;
                while length < 18
                    && cursor + length < bytes.len()
                    && bytes[cursor + length] == bytes[cursor + length - distance]
                {
                    length += 1;
                }
                if length > best_length {
                    best_length = length;
                    best_distance = distance;
                }
            }
            if best_length >= 3 {
                packed[flags] |= 0x80 >> bit;
                packed.push(((best_length - 3) << 4) as u8);
                packed.push((best_distance - 1) as u8);
                cursor += best_length;
            } else {
                packed.push(bytes[cursor]);
                cursor += 1;
            }
        }
    }
    packed
}
