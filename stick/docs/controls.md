# Stick controls

The default screen orientation is left-side-down.

Hold M for about half a second to wake the screen.

| Layout | Left | Center | Right | Stick Settings |
| --- | --- | --- | --- | --- |
| 3-key L/R, default | L | M | R | Hold L+R for one second |
| 3-key R/L | R | M | L | Hold L+R for one second |
| 2-key M/R | M | M+R | R | L |
| 2-key R/M | R | M+R | M | L |

For two-key layouts, M+R window sets how much time you have to press the second button.

Contrast controls the Stick's backlight.

The connector margin shows connection feedback and battery warnings without covering the Walker display.

| Diode indicator | Meaning |
| --- | --- |
| Outline with × | Diode not detected. Check insertion and contact. |
| Solid with opposing arrows | Diode appears reversed. |
| Outline with ? | The failed attempt's diode check was inconclusive. |
| Outline with ! | Receiver input blocked. |
| Solid diode | Diode detected during connection or briefly after correcting a warning. |
| Solid with curved waves | A valid packet was received during this attempt. |

The waves remain steady once communication has been observed. They can accompany a failed connection; the game's result reports whether the operation completed. Successful operations clear the indicator. Warnings remain while the result is visible, and leave when the result closes. Ordinary play has no diode indicator.

While a warning's result is visible, the Stick rechecks at two-second intervals outside communication. Two consistent safe checks change the indication; an unsafe input is handled immediately. Correcting the diode shows the solid icon for two seconds, then clears it. Opening Settings pauses these checks and preserves the attempt's evidence.

A healthy battery has no margin icon, including on wake. A confirmed low battery shows a steady warning while the screen is awake and USB power is absent. Confirmed USB changes while the screen is already on show the battery indication for 2.5 seconds. The USB mark means power is present; it does not establish whether the battery is charging. Stable USB power hides the low warning. Power changes during sleep, Settings or communication do not replay as old notifications.

Stick Settings shows the voltage and approximate percentage. Its **Last connection** entry shows the original diode check, whether receiving could start, accepted packets and the final operation result. This record remains until the next attempt or restart.

[Connection troubleshooting](troubleshooting.md).
