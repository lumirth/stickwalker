# Cold short-cue startup

## Findings

The reported intermittent silence survived the previous score-clock fix.
That fix prevents foreground stalls from consuming a new note, but its tone
counters cannot establish acoustic output.

The review's specific clock prediction was falsified on this board. Before
servicing the first native note, GPIO17 had 168–175 observed BCLK transitions
per 400 µs observation in all six checks. The installed M5 speaker task starts
I2S while filling silence, and retains its clocks until `Speaker.end()`.
These transition counts establish clock presence, not frequency accuracy.

The adapter did have incomplete codec initialization. Register `0x0C` was
left at POR `0x20`, which configures internal power-up sequencing. Espressif
explicitly programs `0x0B/0x0C` to zero, DAC bias `0x10/0x11` to `0x1F/0x7F`,
and serial word length to match its stream. The port had also left the codec
at 24-bit input format while generating 16-bit stereo I2S.
[Everest ES8311 datasheet, registers 09–11](https://www.lcdwiki.com/res/PublicFile/ES8311_DS.pdf),
[Espressif ES8311 initialization and format setup](https://github.com/espressif/esp-adf/blob/release/v2.x/components/esp_codec_dev/device/es8311/es8311.c).

## Implementation

The sound adapter now prepares and verifies `0x0C=0` before any codec
state-machine activation, including restoration of a retained image. It
retries once if the value did not land and tears the output down if it cannot
verify the setting. Espressif documents occasional first-write loss on this
chip; an acknowledged transaction is therefore insufficient evidence.

DAC bias, stage-A/B timing and 16-bit input format are explicitly configured.
ADC operation is enabled only by the bench acoustic test; production retains
the DAC-only clock configuration and existing shutdown/rail ownership.

Cold score preparation is now **200 ms**, with no preparation delay on a warm
repeat during the existing 500 ms hold. This is a conservative interval
validated with the complete local measurement path, not a measured minimum
settling time of the DAC alone. At 65 ms, the diagnostic ADC still has a large
startup transient. The native score, pitch, waveform, note counts and note
durations remain authoritative; `pw_buzzer.c` was not changed in this work.
The first cue after shutdown therefore has additional latency. Input handling
and drawing do not wait in a blocking 200 ms sleep, but native BeepTick owns
foreground execution while its score is pending, as it already did.

## Physical feedback loop

Bench-only `K` loads the existing EEPROM move score through the native task
handoff. I2S1 receives as a slave to the existing I2S0 clock, without replacing
its output routes. ES8311 digital feedback occupies the left slot and its
microphone ADC occupies the right slot. The microphone pin is GPIO16.
The test temporarily enables ADC operation and restores its registers.

Only energy bins and spectrum results leave the device. One bounded 512-frame
note buffer is analyzed locally, then cleared; no audio recording is retained.
The digital cue peaks at FFT bin 16/17 (about 712 Hz). The small speaker's
strongest observed component is the square wave's fifth harmonic, bin 82/83
(about 3.56 kHz). The test requires both spectra and at least 512 samples with
adequate microphone energy in that harmonic. A startup transient alone fails.
`U` checks the physical outgoing PCM; `Q` provides a longer calibration tone;
`O` is the amplifier-disabled negative control. Production excludes these
commands and the entire capture implementation.

Run the focused check on a diagnostic build:

    ../bench/.venv/bin/python stick/audits/sound_acoustic_probe.py --port /dev/cu.usbmodem1101 --pairs 12 --output stick/.build/codec-startup-2026-09-27/acoustic-settled.json

The native move score in the paired EEPROM matches the HGSS source's move
score. Other stored scores differ from the source archive; the on-device test
uses the real saved score rather than replacing that archive.

## Results and limits

- BCLK-before-tone: 6/6 present.
- Physical outgoing PCM: cold and warm native cues both present.
- Original ADC startup: samples stayed zero until approximately 730 ms during
  a sustained calibration tone. Selecting fast VMID charge alone did not
  remove that delay in the tested configuration.
- Clearing the stage timing removed the long ADC gate, but cold 65 ms captures
  still had a large analog transient. In the retained eight-case replay, all
  four cold captures failed the spectral criterion; all four warm cues passed.
- Final 200 ms implementation: **24/24 acoustic cases**, 12 cold rail cycles
  and 12 warm repeats on the same boot. Minimum qualifying sample count was
  1,367. All 24 had complete 512-frame spectra and the expected spectral bins.
  Twelve I2S starts, no begin/power/tone errors, and final codec/I2S flags zero.
- Amplifier-disabled control: the digital peak stayed at bin 17, while the
  microphone peak moved from bin 83 to bin 6. Maximum note-band amplitude
  fell from 293 to 8. This distinguishes acoustic pickup from digital leakage.
- Native score timing: **176/176** cases over the 16 HGSS scores, initial
  foreground stalls and mid-note stalls. Pitch and note/separator durations
  remain within the existing 200 µs comparison tolerance.
- Register-order/fault regression: the previous adapter fails every case;
  the final adapter passes normal startup, first-write loss, retained resume,
  full rail loss, and safe failure after persistent write loss.
- All 16 existing lifecycle/fault cases pass with the updated cold deadline.

The ADC shares the codec and its own startup can obscure a preexisting acoustic
signal. Consequently the failed cold captures do **not** independently prove
that the old DAC emitted no sound. The register initialization defects are
directly established; their responsibility for every user-reported miss is an
inference. The passing final captures demonstrate signal output with the
measurement path enabled. Normal game playback and subjective loudness still
benefit from the user's ears; the test is not a calibrated SPL measurement.

One intermediate spectral diagnostic overflowed loopTask's stack while printing.
That batch is invalid and remains in the evidence directory. Diagnostic buffers
were moved to bounded static storage; the final 24-case batch had no resets.
The initial GPIO38 microphone prototype observed the LCD backlight pin and is
also excluded. Neither failed prototype exists in production.

All attempts and intermediate build/capture failures remain under ignored
`stick/.build/codec-startup-2026-09-27/`. The pre/post filesystem and all 65,536
logical EEPROM bytes compare exactly, including the paired Pokémon. The save
hash is `5ccd615bca4bd063098e94bbab297c38b22475b64093869eefad74f2d421aac4`.
Only the application partition is flashed. Installation and frozen receiver
machine-code checks are recorded in that directory's final manifest.

The installed production application was verified against SHA-256
`edcf812717e01e48ac78c1aa9bd2d6e2f930608778bf68cca3afd394deebee94`.
The complete storage partition also matched its backup after flashing.
CPU0 debugging confirmed application readiness and an advancing game RTC;
the core was resumed afterward. All three frozen receiver sampler bodies and
their addresses match the prior production implementation exactly. The
production binary contains neither the acoustic probe nor its bench commands.
