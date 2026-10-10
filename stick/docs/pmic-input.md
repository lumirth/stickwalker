# Reading the L button

The Stick's L button connects to the M5PM1 power-management chip. Stickwalker
reads the chip's button status over I2C. M and R connect directly to ESP32 GPIOs.

## Status and click timing

| Register | Field | Use |
| --- | --- | --- |
| `BTN_Status`, `0x48` | Bit 0 | Current pressed or released state |
| `BTN_Status`, `0x48` | Bit 7 | A press occurred; reading the register clears it |
| `BTN_CFG_1`, `0x49` | Bits 2:1, SINGLE | Classified single-click timing: 125, 250, 500 or 1000 ms |

Stickwalker reads both status bits together. The press latch catches taps that
end between polls; the current-state bit supports holding L. SINGLE controls
the PMIC's classified-click behavior, which is separate from these raw reads.
The documented register default is `0x2A`, selecting 250 ms. See the
[M5PM1 datasheet, pp. 23–24](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=25)
and [register definitions](https://github.com/m5stack/M5PM1/blob/be9a5456c007c333e7ac963f33bfde1ffa5d82ee/src/M5PM1.h).

## Polling and release

Awake input polls run every 5 ms. During visible operation, sleep lasts at most
100 ms, and the native 62.5 ms work deadline usually wakes the CPU sooner. After
an L gesture, 50 ms of released state rearms it. This prevents one press from
opening and immediately closing Settings.

With the screen off, M wakes the Stick and L polling stops. After wake, the
adapter clears any retained L press and waits for button release before
resuming navigation. See [`input_bridge.cpp`](../input_bridge.cpp) and
[`power_sleep.cpp`](../power_sleep.cpp).

## Why the port polls

The PMIC's IRQ output connects to GPIO13. Its documented button events are
classified single-click, double-click and power-on events. Polling `0x48` gives
the port the pressed state and short-tap latch it needs for navigation and held-L
Settings access.

The IRQ function also flashes the PMIC's status LED when button reset or
power-off is disabled, as they are in Stickwalker. The current setup keeps that
IRQ output disabled. An IRQ-based implementation would need to preserve both
press/release handling and LED behavior. See the
[board wiring](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1),
[datasheet, pp. 21–27](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=23)
and [`board_hal.cpp`](../board_hal.cpp).
