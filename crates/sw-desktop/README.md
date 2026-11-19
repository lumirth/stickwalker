# Stickwalker Desktop

Connect an M5Stack StickS3 over USB to install Stickwalker, update its firmware, edit its palettes, or move a save between your Stick and an emulator. [Download Stickwalker Desktop](https://github.com/lumirth/stickwalker/releases/latest)

The device name, such as **Stick 1A20**, matches the ID in Stick Settings. If several Sticks are connected, click the name to switch between them. Selecting a Stick with Stickwalker installed wakes its screen.

Choose **Edit** beside a palette to rename it or change its colors. Drag a shade or enter a hex value to see it on your Stick. **Blend midtones** fills in the two shades between Background and Ink.

Paste four hex colors, or use a Hachistep palette in `Name|BG|INK` or `Name|BG|LIGHT|DARK|INK` format. Use the File menu to import or export a palette set. If you import fewer than six palettes, they replace the first palettes in the list.

**File → Export EEPROM…** saves the 64 KiB image as `eeprom.bin`. **File → Import EEPROM…** replaces the save on your Stick. To install firmware from a file, choose **Device → Open Firmware…** and select `firmware.json`.

## Windows

Run the downloaded `.exe`. If SmartScreen displays **Windows protected your PC**, check that you downloaded the app from the Stickwalker release page. Choose **More info**, then **Run anyway**. [Stickwalker releases](https://github.com/lumirth/stickwalker/releases/latest)

## Linux

Extract the `Stickwalker` folder and run `./stickwalker` inside it. Keep saves and other personal files outside that folder so Desktop can update. To add an application menu entry, run `python3 install-launcher.py` from the folder. Run it again if you move the app.

The x86-64 build requires glibc 2.39 or later, a Vulkan driver for your GPU, and the Vulkan loader (`libvulkan1` on Ubuntu).

## Linux USB permissions

If Desktop reports a port permission error, run these commands from the extracted folder and reconnect the Stick.

```sh
sudo install -m 644 60-stickwalker.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```
