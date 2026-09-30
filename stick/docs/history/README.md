# Hardware evidence

These consolidated records retain measured results, failed attempts, image
identities and observation limits from earlier firmware. They do not qualify
a new candidate. Raw traces and private saves remain in ignored `stick/.build/`.

- [Transfer trials](transfers.md): retail HeartGold, source-faithful 3DS operations,
  independent EEPROM readback and retained optical/checksum failures.
- [Power and wake trials](power-and-wake.md): battery-only sleep returns, PMIC
  failures, panel/RTC GPIO handoff, generated gestures and save preservation.
- [Audio trials](audio.md): stalled-note reproduction, command timing and cold
  acoustic checks.

For current behavior and procedures, use the [development guide](../../README.md),
[power guide](../power.md), [PMIC input reference](../pmic-input.md),
[debugging guide](../debugging.md) and [release checklist](../release.md).
