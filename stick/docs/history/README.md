# Device test records

These records collect the transfer, power, input and audio trials used to develop
Stickwalker. Each names the firmware, setup and result. Raw traces and private
saves are kept locally under `stick/.build/`.

- [Transfer trials](transfers.md) cover a retail HeartGold Game Card, the 3DS
  test peer, EEPROM readback and packet failures.
- [Power and wake trials](power-and-wake.md) cover battery sleep, button handling,
  PMIC faults, display wake and save preservation.
- [Audio trials](audio.md) cover score timing, codec startup and acoustic checks.

For current instructions, see [development](../../README.md),
[power management](../power.md), [debugging](../debugging.md) or
[release preparation](../release.md). Each release's qualification record lists
which checks ran on that version.
