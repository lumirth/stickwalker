# Diode detection evidence

Checked 2026-10-14 on a StickS3 with the BPV10NF already inserted. No lighting changes or diode removal were needed.

## Why capacitance works

The [StickS3 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf) routes GPIO4–8 directly to the Hat connector, with no external components on those nets. GPIO4 provides an unused reference; GPIO6–8 provide unpopulated controls.

The [BPV10NF datasheet](https://www.vishay.com/docs/81503/bpv10nf.pdf) gives 11 pF typical junction capacitance at zero bias, decreasing with reverse bias. That signature exists in darkness. The [ESP32-S3 touch peripheral](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/cap_touch_sens.html) can measure it with bounded one-shot scans, without optical coupling or a transmitting peer.

## Physical measurements

The final driver detected the connected GPIO5 diode on **4/4 checks**, and classified the unpopulated GPIO6 control as missing on **4/4 checks**. Checks took about 129 ms. GPIO7 and GPIO8 remained inconclusive where channel offsets or sample variation exceeded the acceptance bounds.

Representative connected counts, using 32 charge cycles:

| Charge speed | Voltage window | GPIO5 | GPIO4 | Internal 5 pF | Internal 14.8 pF |
| --- | --- | ---: | ---: | ---: | ---: |
| 3 | 0.5–0.9 V | 1278 | 862 | 576 | 1163 |
| 3 | 0.8–2.4 V | 4828 | 3471 | 2027 | 4361 |
| 7 | 0.5–0.9 V | 808 | 454 | 274 | 530 |
| 7 | 0.8–2.4 V | 2395 | 1509 | 887 | 1887 |

The calibrated added capacitance was approximately 6.95/5.70 pF at speed 3 and 13.55/8.68 pF at speed 7. Native channel offsets make a single typical-datasheet threshold insufficient; both current strengths must agree. Host tests replay the measured connected and bare signatures and cover gain, part-capacitance and steady-photocurrent variation.

Missing detection at the GPIO5 socket is inferred from the bare-control measurements and schematic; the diode was not removed from GPIO5. Other units and lighting conditions were not measured.

## Failure checks

Weak pull-up checks precede touch excitation and strong GPIO precharge. A held-low input skips both. The reversal check sweeps weak pull-up, both weak pulls, and weak pull-down. A reversed junction holds nearly the same forward voltage under the two positive biases, including in darkness; a correctly inserted diode under strong light drops substantially when the second pull is added. A short stays near zero. These distinctions follow the [illuminated-junction equations](https://www.vishay.com/docs/80086/physicstechnology.pdf).

On the inserted diode, three physical sweeps measured pull-up voltages of 3134/3131/3130 mV, both-pull voltages of 1397/1429/1413 mV, and pull-down voltages of 16/14/11 mV. This correctly rejects reversal. Dark-reversal and strong-light signatures are exercised through the production ADC path in the simulator; a physical reversed sweep was not captured.

The S3 has independent digital and RTC pull controls. ADC channel setup selects the digital mux, so the sweep uses the digital weak pulls after configuration. [SDK GPIO implementation](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_driver_gpio/src/gpio.c), [ADC setup](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_adc/adc_oneshot.c).

The IDF 5.5 one-shot routine can return success after a hardware measurement timeout, so the detector also latches its timeout callback. Hardware timeout, stalled scans, invalid references and samples without a repeatable cluster produce an internal inconclusive result with no icon. Every scan shuts down its controller and restores the GPIO without pulls. Simulator checks exercise both timeout paths, feedback clearing, bounded retries and connection recovery.
