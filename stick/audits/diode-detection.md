# Diode presence investigation

Research checked 2026-10-14. A bounded TOUCH5 capacitance measurement is a practical candidate for detecting an empty socket, including in darkness. Its classification thresholds have not been qualified; the current firmware therefore keeps ambiguous results as `Unknown`.

## Electrical evidence

- The [StickS3 V0.6 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf), sheets 3–4, routes U20 GPIO5 through J2/CN1 to the Hat socket. That net has no drawn external resistor, capacitor, TVS or shared peripheral. Connector, trace and package capacitance still exist; their values are unspecified. [M5's pin map](https://docs.m5stack.com/en/core/StickS3#hat2-bus) identifies the adjacent GND/G5 pair.
- [Vishay's BPV10NF datasheet](https://www.vishay.com/docs/81503/bpv10nf.pdf), table on page 2 and figure 5 on page 3, specifies 11 pF typical at zero bias, with no minimum or maximum. The plotted capacitance decreases with reverse bias: approximately 8 pF at 0.5 V, 6.6 pF at 1 V and 5 pF near 2.7 V. Darkness removes photocurrent, not junction capacitance. These curve readings are approximate typical values, not acceptance limits.
- The [ESP32-S3 datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf), analog pin table and DC characteristics, identifies GPIO5 as ADC1_CH4/TOUCH5. Pin capacitance is 2 pF typical and weak pulls are 45 kΩ typical, neither with min/max limits. Its specified ADC rate is 100 kSPS, with accuracy characterized using an external 100 nF capacitor. Ordinary ADC reads cannot resolve this bare diode's sub-microsecond charging transient reliably.

## Most useful experiment

The [IDF 5.5 touch driver](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/cap_touch_sens.html) measures the time for repeated constant-current charge/discharge cycles. It supports explicit one-shot scans, deadlines, selectable current strength and shutdown. There is no need for continuous scans or a background polling task.

The [SDK voltage definitions](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/include/hal/touch_sens_types.h) and [S3 register mapping](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/esp32s3/include/hal/touch_sensor_ll.h) permit upper limits of 0.9–2.7 V and lower limits of 0.5–0.8 V. Test two windows: 0.5–0.9 V and 0.8–2.4 V, alternating their order.

**Idealized calculation, not a measured result:** with equal cycle count and current, normalized duration is proportional to average capacitance over the window. Define `R = 4 * duration_low / duration_high`. The diode curve suggests average capacitances around 7.4 and 6.0 pF, giving:

| Assumed linear parasitic capacitance | Empty socket R | Diode present R |
| --- | --- | --- |
| 2 pF | 1.00 | 1.18 |
| 10 pF | 1.00 | 1.09 |

These parasitic values illustrate sensitivity; they are not Stick measurements. Constant photocurrent ideally contributes the same factor `2I/(I²−Iphoto²)` in both windows and cancels from the ratio. Voltage-dependent photocurrent, changing illumination, unequal source/sink currents and comparator delay break that simplification.

A flat result alone cannot yet establish absence. GPIO protection junctions may contribute voltage dependence; unknown parasitics dilute the diode signature; per-cycle overhead changes the ratio even after fixed scan setup time is removed. Comparing G4 can track common supply effects, but its routing and channel offsets cannot be assumed identical to G5. Espressif's [S3 touch guidance](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/peripherals/touch_pad.html) specifically calls for establishing thresholds from readings and checking temperature and supply changes.

## Qualification and integration

1. Keep the weak pull-up preflight before any charging. A low input must skip touch probing and strong GPIO output. Positive-voltage touch excitation reverse-biases a correctly inserted diode; a reversed diode can clamp it and produce a timeout. Timeout means blocked/inconclusive, never missing.
2. Use a diagnostic build to capture repeated raw counts in both windows at two current strengths and two cycle counts, with a hard total deadline. Capture one labeled empty/correct/reversed set, dark/room light/bright IR, USB/battery, and hands near/away from the socket. This needs no DS connection attempts. Always disable/delete the touch controller and restore GPIO5 afterward.
3. Separate fixed setup overhead from per-cycle behavior; check ordering, repeatability, current dependence and timeout paths. Establish disjoint empty/present envelopes with an explicit unknown band. If they overlap, keep ambiguous results unknown rather than deriving a threshold from the typical curve.
4. After qualification, run only on wake and before connecting, using the existing temporary feedback expiry. A received packet can also establish a per-device present signature; a present baseline by itself does not establish an empty baseline.

A brief built-in IR pulse can provide positive evidence if the diode receives it, but failure to receive a pulse cannot prove absence: the datasheet's narrow angular response and physical placement leave optical coupling uncertain. Capacitance is the stronger route because it does not require light or a peer.
