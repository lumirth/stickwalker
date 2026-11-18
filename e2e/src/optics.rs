use crate::peer::Peer;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::fs;
use std::path::Path;
use std::rc::Weak;

const CYCLES_PER_US: u64 = 240;
const CYCLES_PER_BIT: f64 = 240_000_000.0 / 115_200.0;
// The fitted nominal model uses 19 pF and 45 kΩ. The weak cases vary
// received current, pad trip level, and timing independently of the decoder.
#[derive(Clone, Copy)]
pub enum PathModel {
    Nominal,
    Weak,
    WeakLowTrip,
    WidePad,
    SlowPad,
    Bright,
}

impl PathModel {
    fn capacitance_pf(self) -> f64 {
        if matches!(self, Self::SlowPad) {
            50.0
        } else if matches!(self, Self::WidePad) {
            35.0
        } else {
            19.0
        }
    }

    fn resistance_kohm(self) -> f64 {
        if matches!(self, Self::SlowPad) {
            80.0
        } else if matches!(self, Self::WidePad) {
            60.0
        } else {
            45.0
        }
    }

    fn discharge(self, voltage: f64, cycles: u64, current: f64, pull: bool) -> f64 {
        let ns = cycles as f64 * (1000.0 / CYCLES_PER_US as f64);
        let capacitance = self.capacitance_pf();
        let resistance = self.resistance_kohm();
        let remaining = if pull {
            let offset = current * resistance / 1000.0;
            (voltage + offset) * (-ns / (resistance * capacitance)).exp() - offset
        } else {
            voltage - current * ns / (capacitance * 1000.0)
        };
        remaining.max(0.0)
    }

    fn light_ua(self) -> f64 {
        match self {
            Self::Nominal => 28.5,
            Self::Weak | Self::WeakLowTrip => 3.5,
            Self::WidePad | Self::SlowPad => 10.0,
            Self::Bright => 500.0,
        }
    }

    fn trip_v(self) -> f64 {
        match self {
            Self::Nominal | Self::Weak | Self::WidePad | Self::SlowPad | Self::Bright => 1.55,
            Self::WeakLowTrip => 0.825,
        }
    }

    fn ambient_ua(self, at: u64) -> f64 {
        match self {
            Self::Weak | Self::WeakLowTrip => {
                // Slow drift during Connect and through the exchange.
                let phase = (at / (1000 * CYCLES_PER_US)) % 500;
                1.0 + phase.min(500 - phase) as f64 * (1.0 / 250.0)
            }
            Self::Nominal | Self::WidePad | Self::SlowPad => 1.5,
            Self::Bright => 20.0 + 5.0 * ((at as f64 / 240_000_000.0) * 2.0).sin(),
        }
    }

    fn pulse_cycles(self) -> u64 {
        match self {
            Self::Nominal | Self::WidePad | Self::SlowPad | Self::Bright => 390,
            Self::Weak => 350,
            Self::WeakLowTrip => 390,
        }
    }

    fn bit_cycles(self) -> f64 {
        CYCLES_PER_BIT
            * match self {
                Self::Nominal => 1.00156,
                Self::Weak => 0.994,
                Self::WeakLowTrip => 1.006,
                Self::WidePad | Self::SlowPad => 1.0087,
                Self::Bright => 0.9913,
            }
    }
}

pub enum TornWrite {
    Upload,
    Publish,
}

impl TornWrite {
    pub fn matches(&self, after_commit: bool, data: &[u8]) -> bool {
        match self {
            Self::Upload => !after_commit,
            Self::Publish => after_commit && !data.is_empty(),
        }
    }
}

struct Turn {
    walker: Vec<u8>,
    console: Vec<u8>,
}

pub struct Optics {
    pub path: PathModel,
    pub remote: Option<Weak<RefCell<Optics>>>,
    turns: VecDeque<Turn>,
    incoming: VecDeque<(u64, u64)>,
    voltage: f64,
    pad_time: u64,
    pull: bool,
    pending_pull: Option<(u64, bool)>,
    drive: Option<bool>,
    session_token: Option<[u8; 4]>,
    pub sent: Vec<Vec<u8>>,
    pub peer: Option<Peer>,
    pub cut: Option<TornWrite>,
    pub commit_received: bool,
    pub lose_commit_reply: bool,
    pub lost_reply: bool,
    pub power_cut: bool,
    pub drop_frame_pulse: Option<usize>,
    pub exchanges: usize,
    pub missed: Option<String>,
    pub timed: bool,
    pub replies: Vec<(u64, Vec<u8>)>,
    rng: u32,
    pub aligned_after: u64,
    pub stress_seed: u32,
    pub last_sample_cycle: u64,
    pub lcd_overlap: bool,
    last_stall_slot: u64,
    pub pad_stalls: u64,
    pub startup_light_samples: u64,
    pub startup_interference: bool,
    pub missed_probes: u32,
    pub lose_shutdown: bool,
    pub lost_shutdown: bool,
}

impl Optics {
    pub fn new() -> Self {
        Self {
            path: PathModel::Nominal,
            remote: None,
            turns: VecDeque::new(),
            incoming: VecDeque::new(),
            voltage: 0.0,
            pad_time: 0,
            pull: false,
            pending_pull: None,
            drive: None,
            session_token: None,
            sent: Vec::new(),
            peer: None,
            cut: None,
            commit_received: false,
            lose_commit_reply: false,
            lost_reply: false,
            power_cut: false,
            drop_frame_pulse: None,
            exchanges: 0,
            missed: None,
            timed: false,
            replies: Vec::new(),
            rng: 0xf92b_c421,
            aligned_after: 0,
            stress_seed: 0,
            last_sample_cycle: 0,
            lcd_overlap: false,
            last_stall_slot: u64::MAX,
            pad_stalls: 0,
            startup_light_samples: 0,
            startup_interference: false,
            missed_probes: 0,
            lose_shutdown: false,
            lost_shutdown: false,
        }
    }

    pub fn advance_peer(&mut self, cycle: u64) -> Result<(), String> {
        if let Some(peer) = &mut self.peer {
            let replies = peer
                .advance(cycle / CYCLES_PER_US)
                .map_err(|e| e.to_string())?;
            let mut at = cycle + 380 * CYCLES_PER_US;
            for reply in replies {
                self.illuminate(&reply, at);
                self.replies.push((at, reply.clone()));
                at += (reply.len() as f64 * 10.0 * self.path.bit_cycles()) as u64
                    + 380 * CYCLES_PER_US;
            }
        }
        Ok(())
    }

    pub fn stall(&mut self, cycle: u64) -> u32 {
        if self.stress_seed == 0 || !self.timed {
            return 0;
        }
        // Three contention events about 4 us apart, every 527 us. Charge each
        // once, not again on every poll during the same event window.
        let slot = cycle / 997;
        let phase = (slot + u64::from(self.stress_seed)) % 127;
        if phase < 3 && slot != self.last_stall_slot {
            self.last_stall_slot = slot;
            self.pad_stalls += 1;
            4 + ((cycle ^ u64::from(self.stress_seed)) % 27) as u32
        } else {
            0
        }
    }

    pub fn trace(&mut self, path: &Path) -> Result<(), String> {
        let bytes = fs::read(path).map_err(|err| err.to_string())?;
        if bytes.len() < 16 || bytes[..8] != *b"HGSSLOG1" {
            return Err("Expected an HGSS hardware capture".into());
        }
        let mut walker = None;
        let mut position = 16;
        while position + 32 <= bytes.len() {
            let kind = bytes[position];
            let count =
                u16::from_be_bytes(bytes[position + 2..position + 4].try_into().unwrap()) as usize;
            position += 32;
            if position + 2 * count > bytes.len() {
                return Err("Truncated HGSS capture".into());
            }
            let wire = &bytes[position..position + count];
            if kind == 1 && !wire.is_empty() {
                walker = Some(wire.to_vec());
            }
            if kind == 2 && !wire.is_empty() {
                let expected = walker.take().ok_or("Console frame without a walker turn")?;
                self.turns.push_back(Turn {
                    walker: expected,
                    console: wire.to_vec(),
                });
            }
            position += 2 * count;
        }
        if position != bytes.len() || self.turns.is_empty() {
            return Err("Empty or truncated exchange".into());
        }
        Ok(())
    }

    pub fn set_pull(&mut self, cycle: u64, pull: bool) {
        self.pending_pull = Some((cycle, pull));
    }

    fn integrate(&mut self, cycle: u64) {
        if let Some((at, pull)) = self.pending_pull.filter(|(at, _)| *at <= cycle) {
            self.integrate_until(at);
            self.pull = pull;
            self.pending_pull = None;
        }
        self.integrate_until(cycle);
    }

    fn integrate_until(&mut self, cycle: u64) {
        let pull = self.pull;
        if cycle <= self.pad_time {
            return;
        }
        while self
            .incoming
            .front()
            .is_some_and(|(_, end)| *end <= self.pad_time)
        {
            self.incoming.pop_front();
        }
        if let Some(high) = self.drive {
            self.voltage = if high { 3.3 } else { 0.0 };
        } else {
            // Piecewise constant current during and between SIR pulses.
            let mut start = self.pad_time;
            let ambient = self.path.ambient_ua(start)
                + if self.stress_seed != 0 {
                    0.4 * (1.0
                        + (start as f64 / 240_000_000.0 * std::f64::consts::TAU * 120.0
                            + f64::from(self.stress_seed))
                        .sin())
                } else {
                    0.0
                };
            for &(on, off) in self.incoming.iter().take_while(|(on, _)| *on < cycle) {
                if start < on {
                    self.voltage = self.path.discharge(self.voltage, on - start, ambient, pull);
                }
                let end = cycle.min(off);
                self.voltage = self.path.discharge(
                    self.voltage,
                    end.saturating_sub(start.max(on)),
                    ambient + self.path.light_ua(),
                    pull,
                );
                start = end;
            }
            self.voltage = self
                .path
                .discharge(self.voltage, cycle - start, ambient, pull);
        }
        self.pad_time = cycle;
    }

    pub fn drive(&mut self, cycle: u64, value: Option<bool>) {
        if value != self.drive {
            self.integrate(cycle);
            self.drive = value;
            if let Some(high) = value {
                self.voltage = if high { 3.3 } else { 0.0 };
            }
        }
    }

    pub fn high(&mut self, cycle: u64, _pull: bool) -> bool {
        self.last_sample_cycle = cycle;
        self.integrate(cycle);
        if self.sent.is_empty()
            && self
                .incoming
                .iter()
                .any(|&(on, off)| on <= cycle && cycle < off)
        {
            self.startup_light_samples += 1;
        }
        // Comparator/pad noise is independent of bus contention and optical
        // edge jitter. Do not manufacture delayed dedicated-GPIO instructions.
        let input_noise = if self.stress_seed != 0 {
            0.005 * (cycle as f64 / 997.0 + f64::from(self.stress_seed)).sin()
        } else { 0.0 };
        self.voltage + input_noise > self.path.trip_v()
    }

    pub fn illuminate(&mut self, frame: &[u8], at: u64) {
        if self.lose_shutdown && frame.first() == Some(&(0xf4 ^ 0xaa)) {
            self.lost_shutdown = true;
            return;
        }
        let mut drop = self.drop_frame_pulse == Some(self.replies.len());
        for (index, &byte) in frame.iter().enumerate() {
            let serial = 0x200u16 | (u16::from(byte) << 1);
            for bit in 0..10 {
                if serial & (1 << bit) == 0 {
                    if drop && bit != 0 {
                        drop = false;
                        continue;
                    }
                    self.rng ^= self.rng << 13;
                    self.rng ^= self.rng >> 17;
                    self.rng ^= self.rng << 5;
                    let offset =
                        ((index * 10 + bit) as f64 * self.path.bit_cycles()).round() as i64;
                    let jitter = i64::from(self.rng % 49) - 24;
                    let start = at.saturating_add_signed(offset + jitter);
                    self.incoming
                        .push_back((start, start + self.path.pulse_cycles()));
                }
            }
        }
    }

    pub fn forward_symbol(&self, mut start: u64, word: u32, tick: u32) {
        let Some(remote) = self.remote.as_ref().and_then(Weak::upgrade) else {
            return;
        };
        let mut remote = remote.borrow_mut();
        // A 256-cycle channel latency permits causal 128-cycle VM slices.
        // Pulse widths and spacing come directly from the transmitting RMT.
        start += 256;
        for half in [word & 0xffff, word >> 16] {
            let end = start + u64::from((half & 0x7fff) * tick);
            if half & 0x8000 != 0 {
                assert!(
                    start >= remote.pad_time,
                    "Optical event at {start} arrived after receiver sample {}",
                    remote.pad_time
                );
                remote.incoming.push_back((start, end));
            }
            start = end;
        }
    }

    pub fn transmit(&mut self, ended: u64, words: &[u32], tick: u32) {
        let (characters, tail) = words.as_chunks::<10>();
        if characters.is_empty() || !tail.is_empty() {
            self.missed = Some(format!("RMT emitted {} symbols", words.len()));
            return;
        }
        let mut wire = Vec::new();
        for char_bits in characters {
            let mut serial = 0u16;
            for (bit, &word) in char_bits.iter().enumerate() {
                let first = word & 0x7fff;
                let second = (word >> 16) & 0x7fff;
                let period = f64::from((first + second) * tick);
                let high = word & 0x8000 != 0;
                if (period - CYCLES_PER_BIT).abs() > 9.0
                    || word & 0x8000_0000 != 0
                    || (high && !(350..=430).contains(&(first * tick)))
                {
                    self.missed = Some(format!("Invalid RMT symbol {word:08x}, tick {tick}"));
                    return;
                }
                if !high {
                    serial |= 1 << bit;
                }
            }
            if serial & 0x201 != 0x200 {
                self.missed = Some("SIR framing".into());
                return;
            }
            wire.push((serial >> 1) as u8);
        }
        if wire.len() > 1 && !valid_packet(&wire) {
            self.missed = Some("Invalid walker packet checksum".into());
            return;
        }
        if self.startup_interference {
            // Discovery interference stops when it hears the first Walker probe.
            self.incoming.clear();
            self.startup_interference = false;
        }
        self.sent.push(wire.clone());
        if self.remote.is_some() {
            return;
        }
        if wire.len() >= 8 {
            if let Some((started, request)) = self.replies.last() {
                if request.len() >= 8 && ended.saturating_sub(*started) > 100_000 * CYCLES_PER_US {
                    self.missed = Some("Reply exceeded the cartridge's 100 ms timeout".into());
                    return;
                }
            }
        }
        if self.lose_commit_reply && matches!(wire[0] ^ 0xaa, 0x38 | 0x50 | 0x5a) {
            self.lost_reply = true;
            return;
        }
        if ended < self.aligned_after {
            if wire == [0xfc ^ 0xaa] { self.missed_probes += 1; }
            return;
        }
        if let Some(peer) = &mut self.peer {
            match peer.receive(ended / CYCLES_PER_US, &wire) {
                Ok(replies) => {
                    // Vary the start phase over one microsecond between replies.
                    let phase = (self.replies.len() as u64 * 73) % CYCLES_PER_US;
                    let turnaround = if std::env::var_os("STICK_FAST_REPLY").is_some() { 140 } else { 380 };
                    let mut at = ended + turnaround * CYCLES_PER_US + phase;
                    for reply in replies {
                        let duration = (reply.len() as f64 * 10.0 * self.path.bit_cycles()) as u64;
                        if matches!(reply[0] ^ 0xaa, 0x38 | 0x4e | 0x5a) {
                            self.commit_received = true;
                        }
                        self.illuminate(&reply, at);
                        self.replies.push((at, reply));
                        at += duration + 380 * CYCLES_PER_US;
                    }
                }
                Err(error) => self.missed = Some(error.to_string()),
            }
            return;
        }
        let Some(turn) = self.turns.front() else {
            return;
        };
        let command = wire[0] ^ 0xaa;
        let expected = turn.walker[0] ^ 0xaa;
        if command != expected || wire.len() != turn.walker.len() {
            self.missed = Some(format!(
                "Turn {} expected {:02x}/{} bytes, saw {:02x}/{} bytes",
                self.exchanges,
                expected,
                turn.walker.len(),
                command,
                wire.len()
            ));
            return;
        }
        let turn = self.turns.pop_front().unwrap();
        self.exchanges += 1;
        let mut reply: Vec<u8> = turn.console.iter().map(|byte| byte ^ 0xaa).collect();
        // Rebase captured packets onto the live exchange's randomly generated token.
        if command == 0xf8 {
            let local: [u8; 4] = wire[4..8].try_into().unwrap();
            self.session_token = Some(std::array::from_fn(|i| {
                local[i] ^ 0xaa ^ response_token()[i]
            }));
        }
        if reply.len() >= 8 {
            if reply[0] == 0xfa {
                reply[4..8].copy_from_slice(&response_token());
            } else if let Some(token) = self.session_token {
                reply[4..8].copy_from_slice(&token);
            }
            reply[2..4].fill(0);
            let mut sum = 2 + reply
                .chunks(2)
                .map(|pair| u32::from(u16::from_be_bytes([pair[0], *pair.get(1).unwrap_or(&0)])))
                .sum::<u32>();
            sum = (sum >> 16) + (sum & 0xffff);
            sum = (sum >> 16) + (sum & 0xffff);
            reply[2..4].copy_from_slice(&(sum as u16).to_le_bytes());
        }
        let reply: Vec<_> = reply.into_iter().map(|byte| byte ^ 0xaa).collect();
        if matches!(reply[0] ^ 0xaa, 0x38 | 0x4e | 0x5a) {
            self.commit_received = true;
        }
        let start = ended + 380 * CYCLES_PER_US;
        self.replies.push((start, reply.clone()));
        self.illuminate(&reply, start);
    }

    pub fn remaining(&self) -> usize {
        self.turns.len()
    }
}

fn response_token() -> [u8; 4] {
    [0x17, 0xb9, 0x5e, 0x15]
}

fn valid_packet(wire: &[u8]) -> bool {
    if wire.len() < 8 {
        return false;
    }
    let mut decoded: Vec<_> = wire.iter().map(|byte| byte ^ 0xaa).collect();
    let original = u16::from_le_bytes([decoded[2], decoded[3]]);
    decoded[2..4].fill(0);
    let sum = 2 + decoded
        .chunks(2)
        .map(|pair| u32::from(u16::from_be_bytes([pair[0], *pair.get(1).unwrap_or(&0)])))
        .sum::<u32>();
    let folded = (sum >> 16) + (sum & 0xffff);
    original == (folded + (folded >> 16)) as u16
}
