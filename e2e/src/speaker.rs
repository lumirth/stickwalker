use std::fs;
use std::path::Path;

use crate::{Rig, CYCLES_PER_MS};
use esp_periph::i2s::I2s;
use esp_soc::pins::ChipPins;
use esp_soc::SocBus;

pub struct Speaker {
    pub codec: [u8; 256],
    pub enabled_at: Option<u64>,
    pub startup_cycles: u64,
    cursor: usize,
    samples: Vec<i16>,
    rate: u32,
}

impl Speaker {
    pub fn new(startup_ms: u64) -> Self {
        Self {
            codec: [0; 256],
            enabled_at: None,
            startup_cycles: startup_ms * CYCLES_PER_MS,
            cursor: 0,
            samples: Vec::new(),
            rate: 0,
        }
    }

    pub fn capture(&mut self, cycle: u64, i2s: &I2s, connected: bool, supply_mv: u16) {
        let samples = &i2s.pcm[self.cursor.min(i2s.pcm.len())..];
        self.cursor = i2s.pcm.len();
        if samples.is_empty() {
            return;
        }
        self.rate = i2s.sample_rate;
        // ES8311 external MCLK, slave Philips I²S, 16-bit playback, active DAC.
        let clocked = self.codec[0x01] & 0xb5 == 0x35
            && self.codec[0x02] == 0
            && self.codec[0x09] & 0x1f == 0x0c;
        let dac_on = self.codec[0x00] & 0x80 != 0 && self.codec[0x12] & 0x02 == 0;
        let unmuted = self.codec[0x31] & 0x60 == 0;
        let gain = 10f64.powf((f64::from(self.codec[0x32]) - 191.0) / 40.0);
        // Differential millivolts, using nominal ES8311 full-scale output and
        // the schematic's amplifier input network. Bound by the amp supply.
        let scale = 1000.0 * std::f64::consts::SQRT_2 * 12.0 * gain / 32768.0;
        let limit = (1.5 * f64::from(supply_mv)).min(6050.0);
        let frame_cycles = CYCLES_PER_MS * 1000 / u64::from(self.rate.max(1));
        let first = cycle.saturating_sub((samples.len() - 1) as u64 * frame_cycles);
        for (index, &sample) in samples.iter().enumerate() {
            let time = first + index as u64 * frame_cycles;
            let ready = self
                .enabled_at
                .is_some_and(|at| time >= at + self.startup_cycles);
            let output = if connected && clocked && dac_on && unmuted && ready && supply_mv >= 3000
            {
                (f64::from(sample) * scale).clamp(-limit, limit) as i16
            } else {
                0
            };
            self.samples.push(output);
        }
    }
}

pub fn chirp(rig: &mut Rig, output: &Path) -> Result<f64, Box<dyn std::error::Error>> {
    let pos = rig.speaker.borrow().samples.len();
    rig.switch("power", true);
    let now = rig.machine.bus.cycles();
    rig.run_speaker(70)?;
    rig.switch("power", false);
    rig.run_speaker(300)?;
    let speaker = rig.speaker.borrow();
    let waveform = &speaker.samples[pos..];
    let file: Vec<u8> = waveform
        .iter()
        .flat_map(|sample| sample.to_le_bytes())
        .collect();
    fs::write(output.join("speaker-mv.pcm"), file)?;

    // The retail menu-move score is pitch 0x1d for six units at tempo
    // 240, non-legato. Nintendo subtracts the 0x140-clock separator and
    // mutes one period early: (floor((2048-320)/45)-1)*46 / 32768
    // is 51.94 ms of pitched output.
    let periods: Vec<usize> = waveform
        .windows(2)
        .enumerate()
        .filter_map(|(i, pair)| (pair[0] < 0 && pair[1] > 0).then_some(i))
        .collect();
    let active = waveform.iter().filter(|&&mv| mv != 0).count();
    let duration_ms = active as f64 * 1000.0 / f64::from(speaker.rate.max(1));
    let peak = waveform
        .iter()
        .map(|mv| mv.unsigned_abs())
        .max()
        .unwrap_or(0);
    let correct_periods = periods
        .windows(2)
        .filter(|pair| {
            let us = (pair[1] - pair[0]) as f64 * 1e6 / f64::from(speaker.rate);
            (1350.0..1460.0).contains(&us)
        })
        .count();
    if !(49.0..57.0).contains(&duration_ms) || !(100..=2828).contains(&peak) || correct_periods < 35
    {
        return Err(format!("Bad first chirp at the speaker: {duration_ms:.1} ms, peak {peak} mV, {correct_periods} periods, elapsed {:.0} ms",
            (rig.machine.bus.cycles() - now) as f64 / CYCLES_PER_MS as f64).into());
    }
    println!("Speaker: {duration_ms:.1} ms, modeled peak {peak} mV");
    Ok(f64::from(peak))
}

pub fn observe(rig: &mut Rig) {
    let bus = &rig.machine.bus;
    let routes = ChipPins::S3.routes(&bus.periph.gpio, &bus.periph.io_mux);
    let wires = [(14, 25), (15, 24), (17, 22), (18, 23)];
    let connected = wires
        .into_iter()
        .all(|(pin, signal)| routes.matrix_output(pin, signal));
    let pm = rig.pm.borrow();
    let mv = if pm.usb { 4200 } else { pm.battery_mv };
    rig.speaker
        .borrow_mut()
        .capture(bus.cycles(), &bus.periph.i2s0, connected && pm.rail(), mv);
}

pub fn sequence(rig: &mut Rig, output: &Path) -> Result<(), Box<dyn std::error::Error>> {
    // Retail Confirm -> Back, with the second press arriving before a cold
    // amplifier could have played Confirm if startup waited for the score.
    for attempt in 0..2 {
        observe(rig);
        let start = rig.speaker.borrow().samples.len();
        rig.switch("side", true);
        rig.run_speaker(35)?;
        rig.switch("side", false);
        rig.run_speaker(55)?;
        rig.switch("power", true);
        rig.run_speaker(35)?;
        rig.switch("power", false);
        rig.run_speaker(525)?;
        {
            let speaker = rig.speaker.borrow();
            let pcm = &speaker.samples[start..];
            fs::write(
                output.join(format!("confirm-back-{attempt}.pcm")),
                pcm.iter().flat_map(|s| s.to_le_bytes()).collect::<Vec<_>>(),
            )?;
            let crossings: Vec<_> = pcm
                .windows(2)
                .enumerate()
                .filter_map(|(i, p)| (p[0] < 0 && p[1] > 0).then_some(i))
                .collect();
            // Timer W's 32768 Hz clock and retail compare values 23, 40, 81, 54.
            // Count actual output periods, rather than observing host tone calls.
            for (compare, minimum_ms, maximum_ms) in [
                (23, 20.0, 85.0),
                (40, 27.0, 40.0),
                (81, 24.0, 36.0),
                (54, 65.0, 80.0),
            ] {
                let expected = f64::from(speaker.rate) * f64::from(compare + 1) / 32768.0;
                let periods = crossings
                    .windows(2)
                    .filter(|p| ((p[1] - p[0]) as f64 - expected).abs() < 1.5)
                    .count();
                let duration = periods as f64 * f64::from(compare + 1) * 1000.0 / 32768.0;
                if !(minimum_ms..=maximum_ms).contains(&duration) {
                    return Err(format!(
                        "Confirm/Back compare {compare}: {duration:.2} ms of output"
                    )
                    .into());
                }
                println!(
                    "Confirm/Back {attempt}: {:.2} Hz for {duration:.2} ms",
                    32768.0 / f64::from(compare + 1)
                );
            }
            let first = pcm.iter().position(|&s| s != 0).ok_or("Silent score")?;
            let last = pcm.iter().rposition(|&s| s != 0).unwrap();
            let gaps: Vec<_> = pcm[first..=last]
                .split(|&s| s != 0)
                .filter(|gap| gap.len() > 48)
                .map(|gap| gap.len() as f64 * 1000.0 / f64::from(speaker.rate))
                .collect();
            // Confirm has two notes. Depending on input/tick phase, either or
            // both separators can precede the two required Back separators.
            if !(2..=4).contains(&gaps.len()) || gaps.iter().any(|&ms| !(7.0..=13.0).contains(&ms))
            {
                return Err(format!("Back lost its Timer W separators: {gaps:?} ms").into());
            }
            println!("Back separators: {gaps:?} ms");
        }
        rig.run_speaker(3200)?;
        if rig.pm.borrow().amp_on() {
            return Err("Amplifier did not cool after the score".into());
        }
    }
    Ok(())
}
