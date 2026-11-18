# Stickwalker Desktop

Connect an M5Stack StickS3 over USB to install or update Stickwalker, edit its palettes, or move a save between the Stick and an emulator. [Downloads](https://github.com/lumirth/stickwalker/releases/latest)

The device name, such as **Stick 1A20**, matches the ID in Stick Settings. With several Sticks connected, use the name to switch between them. Selecting a managed Stick wakes its screen.

Choose **Edit** beside the active palette to change its name and colors. Drag a shade or enter its hex value to see the result on the Stick. **Blend midtones** fills in the two middle shades. **Manual** restores the previous midtones during the same editing session.

Copy or paste a palette in Hachistep's `Name|BG|INK` or `Name|BG|LIGHT|DARK|INK` format, or paste four hex colors. Use the File menu to import or export a set. A partial set replaces the first palettes in order.

**File → Export EEPROM** writes a standard 64 KiB `.eeprom` file. **File → Import EEPROM** replaces the current save with the file you choose.

## Linux USB permissions

If Desktop reports a port permission error, run these commands from the extracted download and reconnect the Stick.

```sh
sudo install -m 644 60-stickwalker.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```
