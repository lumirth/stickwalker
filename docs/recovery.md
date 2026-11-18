# Recover over USB

Connect the StickS3 to your computer with a USB data cable. Hold Power until the green light blinks, about four seconds. Keep holding through Stick Settings if it appears, then release. Open Stickwalker Desktop and install the firmware on the port that appears.

If an installation loses power, reconnect and select the same Stick to try again. Desktop can reinstall the application while keeping recognized save storage. **Erase and Install** clears the save and device settings.

## Flash a local build

Use Desktop for an upgrade from a public candidate so it can transfer the old save. For a Stick with the current partition layout already in download mode, use the ESP-IDF Python environment and your [local firmware build](building.md).

```sh
cd build/firmware
python -m esptool --chip esp32s3 --port "$PORT" \
  --before no_reset --after watchdog_reset write_flash @flash_args
```

## If the Power hold has no effect

An older installation can leave the PM1 power chip's download lock set. Desktop can still request the ESP32-S3 ROM downloader over a working USB connection. A reset of the ESP32 alone leaves the PM1 lock in place.

The PM1 register at `0x49` controls button timing and the download lock. Stickwalker sets the button timing to `0x1f`. On affected PM1 revisions, the lock also blocks the `SYS_CMD=0xA3` download request. Restarting the PM1 application clears the lock. On HW5 with PM1 revision `0x4f`, returning to the application from M5Stack's PM1 updater cleared the lock without rewriting PM1 firmware.

The PM1 watchdog can restart a frozen game after startup has armed it. Damaged startup firmware requires USB recovery. [StickS3 download mode](https://docs.m5stack.com/en/core/StickS3#download-mode) · [M5Stack PM1 updater](https://burner.m5stack.com/firmware/1897200000000001767) · [PM1 register reference](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_EN.pdf)
