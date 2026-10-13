# Settings and display implementation

For button instructions, see [controls and settings](controls.md). This guide
covers persistence and pixel presentation for changes to the adapters.

## Persist settings

`pw-controls/layout-v2` stores layout, rotation and the two-key window. Migration
maps each legacy configuration to its directional behavior, including the
reversed three-key mapping. The display adapter applies rotation. Button mapping
uses the selected layout.
Game progress is stored in the EEPROM image.

`pw-display/dark` stores appearance. Unsaved installations default to Dark.
A failed persistence write leaves the previous appearance active. A successful
change repaints the overlay immediately and invalidates the game image so it
receives a complete repaint when the overlay closes.

## Present native shades

The port scales native pixels 2× with nearest-neighbor sampling and preserves
native rendering cadence. Its RGB565 palette is:

| Native shade | Light | Dark |
| --- | --- | --- |
| 0, background | `FFFF`, white | `0000`, black |
| 1 | `AD75`, light gray | `528A`, dark gray |
| 2 | `528A`, dark gray | `AD75`, light gray |
| 3, foreground | `0000`, black | `FFFF`, white |

RGB565 gives green six bits and red/blue five. The intermediate channels expand
to (173,174,173) and (82,81,82) with M5GFX, within one 8-bit level of neutral.

Call `setSwapBytes(true)` after panel reinitialization. M5GFX's
`create_pc(uint16_t*, bool)` path otherwise interprets intermediate buffer words
in the wrong byte order. Black and white still look correct, which can conceal
the mistake. Light/Dark selects the software palette. Panel inversion, gamma
and RGB order remain driver settings.

The selected theme also applies to Settings, battery text and the game border.
Wake clears the controller with the current background. Native Contrast maps
to backlight brightness.

## Check changes

The presenter consumes an immutable native LCD snapshot and revision. A call
transfers at most eight native rows and finishes with SPI idle, retaining no
caller pointer. Remaining strips are scheduled independently of native refresh;
appearance, rotation and overlay changes also invalidate presentation.

`uv run python stick/check.py` exercises production input gestures, layout and
appearance persistence, and returning from Settings to the native screen.
After a display change, check the four shades and Settings on the board in both
appearances and after wake.
