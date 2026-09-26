# Independent controls and Light/Dark appearance

## Choices

Layout codes list the native Left/Right physical keys. Center is a chord in
two-key layouts and M in three-key layouts.

| Setting | Left | Center | Right |
| --- | --- | --- | --- |
| 2-key M/R | M | M+R | R |
| 2-key R/M | R | M+R | M |
| 3-key L/R | L | M | R |
| 3-key R/L | R | M | L |

Rotation affects the display only. The input state machine does not receive
rotation. Existing chord detection, short-tap queuing, native button bits,
wake gestures and L menu access are retained.

The original saved configuration coupled mode and rotation. Migration maps
each old combination to its actual directional behavior, including the
original reversed three-key mapping. The separate `pw-controls/layout-v2`
key stores layout, rotation and chord window. Native EEPROM is not changed.

Stick Settings has Input, Rotation, Appearance, Chord window and Test speaker.
The three-key layout displays Center: M instead of an editable chord window.
M advances, R changes and L closes the overlay. Appearance switches and shows
four swatches immediately, then persists in `pw-display/dark`. A failed write
leaves the previous appearance active. Dark remains the default for installations
with no saved appearance.

## Display research and palette

M5Stack identifies the screen as a 135×240 transmissive IPS TFT with an
ST7789P3 controller. It is a color display rather than an OLED or reflective
Pokéwalker LCD.
[M5Stack screen specification](https://docs.m5stack.com/en/accessory/display/Display_1.14_For_StickS3).

The controller supports RGB565 and RGB666 input; this port uses RGB565.
Green has six bits, while red and blue have five. Intermediate grays must
match normalized channel levels, rather than equal raw channel integers.
The controller also has gamma and display inversion controls; these are
panel settings and are distinct from swapping the application's shade palette.
[ST7789P3 datasheet, sections 8.7.11, 9.1.15–17, 9.2.25–26](https://files.waveshare.com/wiki/ESP32-S3-GEEK/ST7789P3.pdf).

The installed M5GFX `create_pc(uint16_t*, bool)` pipeline treats the buffer as
ordinary RGB565 when `setSwapBytes(true)` is selected. Without it the same
intermediate words are interpreted in the wrong byte order, while 0000 and
FFFF remain unchanged. Preserve this setting after controller reinitialization.
The existing analog/gamma initialization, RGB order and panel inversion remain
the display driver's settings. Light/Dark is implemented in software palette
selection, not by toggling hardware inversion.

The reference screenshots show four discrete shade roles. The user's requested
full white/black light-mode endpoints replace the reference's gray background
and dark-gray foreground. Intermediate roles remain approximately at one-third
and two-thirds of that digital range:

| Native shade | Light RGB565 | Dark RGB565 |
| --- | --- | --- |
| 0, background | FFFF, white | 0000, black |
| 1 | AD75, light gray | 528A, dark gray |
| 2 | 528A, dark gray | AD75, light gray |
| 3, foreground | 0000, black | FFFF, white |

With M5GFX's RGB565-to-RGB888 bit expansion, the intermediate channels are
(82,81,82) and (173,174,173): within one 8-bit level of neutral. All four
levels remain distinct and ordered. Native pixel indices and rendering cadence
stay intact, with nearest-neighbor 2× presentation and no added colors or
dithering. Native contrast continues to control the current backlight mapping.

The theme also applies to overlay text, battery text and the game border.
Changing theme invalidates the presented image; a new complete image is sent
after closing the overlay. Controller wake clears with the current background.

These are neutral digital colors, not measured panel luminances. Matching the
original LCD's exact optical transfer curve, white point or perceived shade
spacing would require panel measurements. No new gamma calibration or battery
savings from dark mode are claimed.

## Validation

The controls regression exercises all physical directions, both Center chords,
three-key simultaneous inputs, bounce/overlap rejection, short taps and the
previous quick-chord timing. The settings test checks every valid legacy
combination, all 24 new layout/rotation/window combinations, invalid encodings,
distinct labels, four monotonic neutral colors and exact Light/Dark reversal.

The production-adapter lifecycle probe additionally cycles the four options,
changes rotation independently, toggles appearance, tests a failed persistence
write, closes the overlay, checks the full light-mode repaint and reloads the
saved theme on panel initialization. Existing wake, supply, codec and sleep
regressions still pass. The TX power regressions also pass.

Commands:

    c++ -std=c++17 stick/tests/controls_test.cpp stick/controls.cpp -o stick/.build/controls-test
    stick/.build/controls-test
    c++ -std=c++17 stick/tests/device_settings_test.cpp -o stick/.build/device-settings-test
    stick/.build/device-settings-test
    python3 stick/audits/idle_power_trace.py
    python3 stick/tests/ir_tx_power_test.py
    python3 stick/build_port.py
    python3 stick/build_port.py --bench-control

The production and diagnostic candidates build. All three critical sampler functions remain
byte-identical, at the same addresses, to the frozen previous production app.
Local build evidence is under `stick/.build/device-settings-*`; source, settings
and shade checks are committed. This change includes the prior unflashed power
candidate. No device flash, paired-storage access or hardware visual validation
has occurred in this block.
