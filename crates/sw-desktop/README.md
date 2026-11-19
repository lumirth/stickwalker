# Stickwalker Desktop

Connect an M5Stack StickS3 over USB to install or update Stickwalker, edit its palettes, or move a save between the Stick and an emulator. [Downloads](https://github.com/lumirth/stickwalker/releases/latest)

The device name, such as **Stick 1A20**, matches the ID in Stick Settings. With several Sticks connected, use the name to switch between them. Selecting a managed Stick wakes its screen.

Choose **Edit** beside the active palette to change its name and colors. Drag a shade or enter its hex value to see the result on the Stick. **Blend midtones** fills in the two middle shades. **Manual** restores the previous midtones during the same editing session.

Copy or paste a palette in Hachistep's `Name|BG|INK` or `Name|BG|LIGHT|DARK|INK` format, or paste four hex colors. Use the File menu to import or export a set. A partial set replaces the first palettes in order.

**File → Export EEPROM** writes a standard 64 KiB `.eeprom` file. **File → Import EEPROM** replaces the current save with the file you choose.

## Windows

Run the downloaded `.exe` directly. The Windows build is unsigned, so SmartScreen may ask you to confirm the publisher. Check that your download came from the [Stickwalker release page](https://github.com/lumirth/stickwalker/releases/latest).

## Linux

The x86-64 tarball targets Ubuntu 24.04 (glibc 2.39) and newer compatible distributions. The display needs the Vulkan loader (`libvulkan1` on Ubuntu) and a Vulkan driver for your GPU. Extract the entire `Stickwalker` folder, keep it together, and run `./stickwalker` from that folder. For an application menu entry, run `python3 install-launcher.py` there. Run it again if you move the folder.

## Linux USB permissions

If Desktop reports a port permission error, run these commands from the extracted download and reconnect the Stick.

```sh
sudo install -m 644 60-stickwalker.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```
