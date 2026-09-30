# PMIC button input

The PMIC exposes raw button state separately from classified-click timing.
This source review establishes their documented meanings, not physical latency.

## What the timing field establishes

`BTN_CFG_1` (`0x49`) bits 2:1 select the PMIC's single-click timing from
125/250/500/1000 ms. The English datasheet labels this field SINGLE; the host
driver calls it click delay. Its timing origin is unspecified: the examined
sources do not establish debounce time, required hold duration, or a fixed
post-release wait. The declared register default is `0x2A`, whose SINGLE field
selects 250 ms, so 125 ms is the minimum available selection, not evidence of
the currently installed setting. [English datasheet, printed pp.23-24](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=25),
[official register definitions](https://github.com/m5stack/M5PM1/blob/be9a5456c007c333e7ac963f33bfde1ffa5d82ee/src/M5PM1.h).

## Separate raw interface

`0x48` bit0 reports the current pressed/released state; bit7 records that a
press occurred and clears on read. No 125 ms minimum is documented for this
interface. That does not prove zero internal debounce or a particular electrical
latency. The published ESP32 driver reads this register; it does not implement
the PMIC's internal button classifier. [English datasheet, printed p.23](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=25),
[official driver implementation](https://github.com/m5stack/M5PM1/blob/be9a5456c007c333e7ac963f33bfde1ffa5d82ee/src/M5PM1.cpp).

The current port reads both raw fields together, including short-tap latching.
It preserves SINGLE configuration bits when changing the long-hold recovery
threshold and disabling reset/shutdown. It imposes no 125 ms input timer.
Awake polls are scheduled every 5 ms; processor sleep and foreground work can
lengthen that interval. The visible sleep cap is 100 ms, normally preempted by
the 62.5 ms native work deadline. The separate 50 ms quiet-release condition
rearms the next L gesture; it is not a wait added to the beginning of each tap.
These are source scheduling bounds, not measured end-to-end L latency.
[Current input adapter](../input_bridge.cpp),
[current scheduler](../board_runtime.cpp), [sleep policy](../power_sleep.cpp).

## Why GPIO13 IRQ is not yet an established replacement

The Stick's PMIC IRQ output reaches ESP32 GPIO13. Button IRQ status distinguishes
single-click, double-click and power-on; the WAKEUP bit is tied to retained
power-on state, not documented as an ordinary running-device L-down edge.
The single-click IRQ replaces the disabled reset action. There is no documented
raw L edge IRQ in the examined register definitions. [StickS3 wiring](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1),
[English datasheet, printed pp.21-23](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=23).

The datasheet additionally specifies automatic LED flashing with a GPIO IRQ
function enabled: 200 ms for disabled button reset, 100 ms for disabled
double-click power-off. The port uses these disable settings. Flag acknowledgement
is not documented as suppressing that indication. This gives a primary-source
explanation for the earlier IRQ-mode green LED behavior; enabling IRQ is not
an established energy improvement. [English datasheet, printed p.27](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf#page=29),
[port board initialization](../board_hal.cpp).

## Decision

Keep raw-state/latch input for responsive L navigation and held-L Settings
access. Do not replace it with the classified click IRQ based on the 125 ms
field alone. An IRQ-assisted design would need evidence of press/release timing
and a way to preserve LED-off behavior; the published docs do not settle either.
M-only dark wake already eliminates dark-screen L polling without that change.

The official repository tree and M5Stack repository search did not reveal the
PM1 internal button-state-machine source. This is a search limitation, not a
claim that it exists nowhere. The available host driver only writes configuration
and reads status. [Official repository](https://github.com/m5stack/M5PM1).

Downloaded English and Chinese PDFs, extracted text, the inspected Chinese
button-flow diagram and hashes are retained under ignored
`stick/.build/pmic-click-research-2026-09-27/`. Research used the official English
V1.9 datasheet, Chinese V1.9 datasheet and the official host driver pinned above;
no new physical latency or current measurement was made.
