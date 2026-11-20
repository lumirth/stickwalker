// The HGSS side of a Pokéwalker connection, written from the behavior of
// pokewalker_ir.c in the pokeheartgold decompilation: the 0xAA-framed packet
// layer, the passive handshake, and the 0x52 (send) and 0x40 (return) requests.
use std::io;

const TOKEN: u32 = 0x15ba_196b;
const RTC_SECONDS: u32 = 123_456_789;
const TIMEOUT_US: u64 = 100_000;
const COURSE: (u16, usize) = (0xd700, 10430);
const RECORDS: (u16, usize) = (0xd480, 548);
const NOT_CONNECTED: i32 = 1;
const TIMED_OUT: i32 = 2;
const PROTOCOL: i32 = 12;
const DONE: i32 = 15;

#[derive(Clone, Copy, PartialEq)]
enum Request {
    Send = 0x52,
    Return = 0x40,
}

enum Transfer {
    Idle,
    // Pages written to the Walker; `pages` holds the wire payload of each.
    Write { step: u8, address: u16, pages: Vec<(u8, Vec<u8>)>, next: usize },
    // Bytes read from the Walker into `into`.
    Read { step: u8, address: u16, remaining: usize, into: Vec<u8> },
}

pub struct Peer {
    request: Request,
    identity: [u8; 104],
    walker: [u8; 104],
    records: Vec<u8>,
    course: Vec<u8>,
    session: u32,
    handshake_started: bool,
    active: bool,
    closing: bool,
    last_receive: u64,
    now: u64,
    transfer: Transfer,
    error: i32,
    outbox: Vec<Vec<u8>>,
    pub result: i32,
    pub connected: bool,
}

impl Peer {
    pub fn open(mode: &str, eeprom: &[u8], course: Option<&[u8]>) -> io::Result<Self> {
        let request = match mode {
            "put" => Request::Send,
            "back" => Request::Return,
            _ => return Err(io::Error::other(format!("Unknown HGSS request {mode}"))),
        };
        // The game's identity carries the trainer's pairing, language and version.
        let mut identity: [u8; 104] = eeprom[0xed..0xed + 104].try_into().unwrap();
        if request == Request::Send {
            identity.copy_within(0..4, 4);
            identity.copy_within(8..10, 10);
        }
        identity[0x60..0x64].copy_from_slice(&RTC_SECONDS.to_be_bytes());
        let course = course.map(<[u8]>::to_vec).unwrap_or_default();
        if request == Request::Send && course.len() != COURSE.1 {
            return Err(io::Error::other("Sending needs the 10430-byte course"));
        }
        Ok(Self {
            request,
            identity,
            walker: [0; 104],
            records: eeprom[0xcc00..0xcc00 + RECORDS.1].to_vec(),
            course,
            session: TOKEN,
            handshake_started: false,
            active: false,
            closing: false,
            last_receive: 0,
            now: 0,
            transfer: Transfer::Idle,
            error: NOT_CONNECTED,
            outbox: Vec::new(),
            result: NOT_CONNECTED,
            connected: false,
        })
    }

    pub fn receive(&mut self, at_us: u64, wire: &[u8]) -> io::Result<Vec<Vec<u8>>> {
        self.now = at_us;
        if !self.finished() {
            self.poll(wire);
        }
        Ok(self.flush())
    }

    pub fn advance(&mut self, at_us: u64) -> io::Result<Vec<Vec<u8>>> {
        if at_us < self.now {
            return Err(io::Error::other("Peer clock moved backwards"));
        }
        self.now = at_us;
        if !self.finished()
            && (self.active || self.handshake_started)
            && self.now - self.last_receive >= TIMEOUT_US
        {
            self.close(true);
            self.fail(TIMED_OUT);
        }
        Ok(self.flush())
    }

    // The game leaves the link screen once a request has an outcome.
    fn finished(&self) -> bool {
        !self.active && self.error != 0 && self.error != NOT_CONNECTED
    }

    fn flush(&mut self) -> Vec<Vec<u8>> {
        self.result = self.error;
        self.connected = self.active;
        std::mem::take(&mut self.outbox)
    }

    fn poll(&mut self, wire: &[u8]) {
        let packet: Vec<u8> = wire.iter().map(|b| b ^ 0xaa).collect();
        match packet.len() {
            1 if self.active || packet[0] != 0xfc => return,
            2..=7 => return,
            0 => return,
            _ => {}
        }
        if packet.len() >= 8 {
            let mut zeroed = packet.clone();
            zeroed[2..4].fill(0);
            if u16::from_le_bytes([packet[2], packet[3]]) != checksum(&zeroed) {
                if self.active {
                    self.closing = true;
                }
                return;
            }
        }
        self.last_receive = self.now;
        let command = packet[0];
        let session = (packet.len() >= 8)
            .then(|| u32::from_le_bytes(packet[4..8].try_into().unwrap()))
            .unwrap_or(0);
        if command < 0xf0 && (!self.active || session != self.session) {
            return;
        }
        match command {
            0xfc => {
                if self.handshake_started {
                    return;
                }
                self.handshake_started = true;
                if packet.len() > 1 && !self.active {
                    return;
                }
                self.packet(&[], 0xfa, 1, TOKEN);
                if self.active {
                    self.closing = true;
                }
            }
            0xf8 if self.handshake_started => {
                self.session = session ^ TOKEN;
                self.active = true;
                self.closing = false;
                self.handshake_started = false;
                self.connect(packet[1]);
            }
            0xf6 => self.closing = true,
            0xf0.. => {}
            _ if !self.closing => self.on_receive(command, &packet[8..]),
            _ => {}
        }
    }

    fn connect(&mut self, result: u8) {
        self.error = 0;
        if !(1..=2).contains(&result) {
            return self.fail_and_close(PROTOCOL);
        }
        self.send(&[], 0x20, 1);
    }

    fn on_receive(&mut self, command: u8, data: &[u8]) {
        match command {
            0x22 if data.len() >= 104 => {
                self.walker.copy_from_slice(&data[..104]);
                self.validate();
            }
            0x26 => match self.request {
                Request::Send => self.send(&[], 0x5a, 1),
                Request::Return => {
                    self.send(&[], 0x4e, 1);
                    self.fail_and_close(DONE);
                }
            },
            0x28 => self.fail_and_close(3),
            0x9c | 0x9e => self.fail_and_close(PROTOCOL),
            0x54 if self.request == Request::Send => {
                let pages = compressed_pages(&self.course);
                self.begin_write(0x3c, COURSE.0, pages);
            }
            0x42 if self.request == Request::Return => self.begin_read(0x46, 0x8f00, 42),
            0x44 | 0x64 => self.fail_and_close(14),
            0x5a | 0x5e => self.fail_and_close(DONE),
            0x04 => self.write_acknowledged(),
            0x0e => self.read_received(data),
            _ => {}
        }
    }

    fn validate(&mut self) {
        let (own, walker) = (&self.identity, &self.walker);
        let u16_at = |b: &[u8; 104], at: usize| u16::from_le_bytes([b[at], b[at + 1]]);
        let registered = walker[0x5b] & 1 != 0;
        let strolling = walker[0x5b] & 2 != 0;
        let same_trainer = walker[0x0c..0x10] == own[0x0c..0x10];
        let newer_walker = walker[0x5d] > own[0x5d];
        let failure = match self.request {
            Request::Send => (!registered).then_some(5)
                .or(strolling.then_some(7))
                .or((walker[0..4] != own[0..4]).then_some(8))
                .or((u16_at(walker, 8) >> 2 != u16_at(own, 8) >> 2).then_some(9))
                .or((!same_trainer).then_some(11))
                .or(newer_walker.then_some(10)),
            Request::Return => (!registered).then_some(5)
                .or((!strolling).then_some(6))
                .or((!same_trainer).then_some(11))
                .or((walker[4..8] != own[4..8]).then_some(8))
                .or((u16_at(walker, 10) >> 2 != u16_at(own, 10) >> 2).then_some(9))
                .or(newer_walker.then_some(10)),
        };
        let request = self.request as u8;
        match failure {
            Some(error) => {
                self.send(&[], request + 4, 1);
                self.fail_and_close(error);
            }
            None => {
                let identity = self.identity;
                self.send(&identity, request, 1);
            }
        }
    }

    fn begin_write(&mut self, step: u8, address: u16, pages: Vec<(u8, Vec<u8>)>) {
        self.transfer = Transfer::Write { step, address, pages, next: 0 };
        self.write_next();
    }

    fn write_next(&mut self) {
        let Transfer::Write { address, pages, next, .. } = &mut self.transfer else { return };
        let (command, payload) = pages[*next].clone();
        let page = *address + (*next as u16) * 0x80;
        *next += 1;
        self.send(&payload, command | (page & 0x80) as u8, (page >> 8) as u8);
    }

    fn write_acknowledged(&mut self) {
        let Transfer::Write { step, pages, next, .. } = &self.transfer else { return };
        if *next < pages.len() {
            return self.write_next();
        }
        match step {
            0x3c => {
                // The Walker keeps the game's clock with the trainer records.
                self.records[8..0x30].copy_from_slice(&self.walker[0x10..0x38]);
                let pages = raw_pages(&self.records);
                self.begin_write(0x3e, RECORDS.0, pages);
            }
            0x3e => self.begin_read(0x48, 0xce80, 0xd4c),
            _ => self.fail_and_close(PROTOCOL),
        }
    }

    fn begin_read(&mut self, step: u8, address: u16, size: usize) {
        self.transfer = Transfer::Read { step, address, remaining: size, into: Vec::new() };
        self.read_next();
    }

    fn read_next(&mut self) {
        let Transfer::Read { address, remaining, .. } = &self.transfer else { return };
        let count = (*remaining).min(0x80) as u8;
        let [high, low] = address.to_be_bytes();
        self.send(&[high, low, count], 0x0c, 2);
    }

    fn read_received(&mut self, data: &[u8]) {
        let Transfer::Read { step, address, remaining, into } = &mut self.transfer else { return };
        let count = data.len().min(*remaining);
        into.extend_from_slice(&data[..count]);
        *address = address.wrapping_add(count as u16);
        *remaining -= count;
        if *remaining > 0 {
            return self.read_next();
        }
        match (*step, self.request) {
            (0x46, _) => self.begin_read(0x48, 0xce80, 0xd4c),
            (0x48, _) => self.begin_read(0x4a, 0xb800, 1736),
            (0x4a, Request::Send) => self.send(&[], 0x24, 1),
            (0x4a, Request::Return) => self.begin_read(0x4c, 0xde24, 5480),
            (0x4c, _) => self.send(&[], 0x24, 1),
            _ => self.fail_and_close(PROTOCOL),
        }
    }

    fn fail(&mut self, error: i32) {
        if self.error == 0 {
            self.error = error;
        }
    }

    fn fail_and_close(&mut self, error: i32) {
        self.fail(error);
        self.close(false);
    }

    fn close(&mut self, timed_out: bool) {
        if self.active && !timed_out {
            self.packet(&[], 0xf4, 1, self.session);
        }
        self.active = false;
        self.closing = false;
        self.handshake_started = false;
        self.transfer = Transfer::Idle;
    }

    fn send(&mut self, payload: &[u8], command: u8, arg: u8) {
        if self.active {
            self.packet(payload, command, arg, self.session);
        }
    }

    fn packet(&mut self, payload: &[u8], command: u8, arg: u8, session: u32) {
        let mut packet = vec![command, arg, 0, 0];
        packet.extend_from_slice(&session.to_le_bytes());
        packet.extend_from_slice(payload);
        let sum = checksum(&packet);
        packet[2..4].copy_from_slice(&sum.to_le_bytes());
        self.outbox.push(packet.iter().map(|b| b ^ 0xaa).collect());
    }
}

fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = 2u32;
    for (index, &byte) in bytes.iter().enumerate() {
        sum += if index & 1 == 0 { u32::from(byte) << 8 } else { u32::from(byte) };
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;
    sum as u16
}

// Raw writes send each 128-byte page, with a short final page.
fn raw_pages(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    data.chunks(0x80).map(|page| (0x02, page.to_vec())).collect()
}

// Compressed writes send each full page as LZ10 when that is smaller. The
// final partial page is stored as a padded 128-byte page.
fn compressed_pages(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    data.chunks(0x80)
        .map(|page| {
            if page.len() < 0x80 {
                let mut padded = page.to_vec();
                padded.resize(0x80, 0);
                return (0x02, padded);
            }
            match lz10(page) {
                Some(packed) => (0x00, packed),
                None => (0x02, page.to_vec()),
            }
        })
        .collect()
}

fn lz10(source: &[u8]) -> Option<Vec<u8>> {
    let mut out = vec![0x10, source.len() as u8, (source.len() >> 8) as u8, 0];
    let mut position = 0;
    while position < source.len() {
        let flags_at = out.len();
        out.push(0);
        for bit in 0..8 {
            if position >= source.len() {
                break;
            }
            let (length, distance) = (1..=position.min(0x1000))
                .map(|distance| {
                    let length = (0..18)
                        .take_while(|&k| {
                            position + k < source.len()
                                && source[position + k] == source[position + k - distance]
                        })
                        .count();
                    (length, distance)
                })
                .max_by_key(|&(length, distance)| (length, std::cmp::Reverse(distance)))
                .unwrap_or((0, 0));
            if length >= 3 {
                out[flags_at] |= 0x80 >> bit;
                out.push((((length - 3) << 4) | ((distance - 1) >> 8)) as u8);
                out.push((distance - 1) as u8);
                position += length;
            } else {
                out.push(source[position]);
                position += 1;
            }
        }
    }
    (out.len() < source.len()).then_some(out)
}

pub fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unlz10(packed: &[u8]) -> Vec<u8> {
        let size = usize::from(packed[1]) | usize::from(packed[2]) << 8;
        let (mut out, mut at) = (Vec::new(), 4);
        while out.len() < size {
            let flags = packed[at];
            at += 1;
            for bit in 0..8 {
                if out.len() >= size {
                    break;
                }
                if flags & (0x80 >> bit) == 0 {
                    out.push(packed[at]);
                    at += 1;
                } else {
                    let length = usize::from(packed[at] >> 4) + 3;
                    let distance = (usize::from(packed[at] & 15) << 8 | usize::from(packed[at + 1])) + 1;
                    at += 2;
                    for _ in 0..length {
                        out.push(out[out.len() - distance]);
                    }
                }
            }
        }
        out
    }

    #[test]
    fn compressed_pages_decode_to_the_course() {
        let course: Vec<u8> = (0..COURSE.1).map(|i| ((i / 7) % 13) as u8 ^ (i as u8 & 0x40)).collect();
        let pages = compressed_pages(&course);
        assert!(pages.iter().any(|(command, _)| *command == 0));
        let decoded: Vec<u8> = pages
            .iter()
            .flat_map(|(command, page)| if *command == 0 { unlz10(page) } else { page.clone() })
            .collect();
        assert_eq!(&decoded[..COURSE.1], &course[..]);
        assert!(decoded[COURSE.1..].iter().all(|&b| b == 0));
    }
}
