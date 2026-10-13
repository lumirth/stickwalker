# Audio trials, September 26-27, 2026

These tests measured score timing and acoustic output. Raw
artifacts remain in `stick/.build/sound-scheduling-2026-09-26/` and
`stick/.build/codec-startup-2026-09-27/`.

These are historical results, not qualification of the current
[native-platform redesign](../native-platform.md). It removes score reanchoring
and the fixed cold-start delay, preparing physical output separately from native
score timing. Its acoustic onset and settling need fresh device measurements.

## Score scheduling

The native score walker and Stick adapter initially failed 102 of 176 host
timing cases. An 80 ms foreground stall reduced a warm move cue's only tone
to zero duration; even an 8 ms stall clipped its first note. Anchoring new
notes to the actual output transition and selecting speaker channel 0 fixed
the tested scheduling and overlapping-voice behavior.

All 176 final cases passed across sixteen HGSS scores, including cold starts,
warm repeats, initial stalls of 0/8/20/80/200 ms and a stall during playback.
Tone/separator lifetimes agreed with unstalled playback within 200 µs.
These are host command-lifetime observations.

A USB bench recorded 20/20 complete native move cues after an 80 ms stall,
with zero tone/begin/power errors and minimum command lifetime 52,667 µs.
One I2S startup served the batch; codec/I2S flags returned to zero after the
hold expired.

The production app was
`d9e3c352cf217a292557c099923fd2c7c3fa75c28a5db3be5059a2336d90aef3`.
Its app-only flash and complete storage preservation were verified. The logical
EEPROM hash stayed
`5ccd615bca4bd063098e94bbab297c38b22475b64093869eefad74f2d421aac4`;
boot checkpointed the existing journal from generation 567 to 568.

## Cold output path

The proposed missing-clock cause was falsified: all six observations found
168 to 175 BCLK transitions per 400 µs before the first note.

The codec retained power-sequence register `0x0C=0x20` and 24-bit input while
output was 16-bit stereo. The adapter now verifies stage setup before activation,
configures DAC bias/format and handles an acknowledged first-write loss.
Cold preparation uses 200 ms, with no preparation delay for warm repeats.
At 65 ms the diagnostic ADC transient obscured the 52 ms cue; 200 ms is a
the preparation interval used by the passing tests.

The board self-test uses codec digital feedback and microphone energy, requiring
both the native cue near FFT bin 16/17 and its observed fifth harmonic near
bin 82/83. It reports the spectral measurements and discards the samples.

## Results

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

The microphone capture includes the ADC's startup transient, which obscured
the tone in the failed cold captures. The final 200 ms setup produced the
expected signal in both cold and warm tests. These measurements describe the
recorded signal spectrum; listening checks assess the resulting sound.

One intermediate spectral diagnostic overflowed loopTask's stack while printing.
That batch is invalid and remains in the evidence directory. Diagnostic buffers
were moved to bounded static storage; the final 24-case batch had no resets.

The production sampler functions retained their complete bytes and addresses.

The installed codec-startup production app was
`edcf812717e01e48ac78c1aa9bd2d6e2f930608778bf68cca3afd394deebee94`.
App readback, the complete storage partition and all logical EEPROM bytes
matched their backups; CPU0 readback found ready=1 and advancing RTC after resume.
