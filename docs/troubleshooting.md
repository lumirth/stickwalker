# Troubleshooting

## The cartridge cannot find the Stick

Connect the BPV10NF anode to GND and its cathode to G5. Aim both the photodiode and the Stick's built-in emitter at the cartridge's infrared window, starting a few centimeters away.

Choose **Connect** and look for the diode icon. An outline with an **×** means the socket looks empty. Check that both leads make contact. Two arrows pointing in opposite directions mean the leads are reversed. A filled diode means the wiring is ready. Waves beside it mean the Stick heard its partner.

## Stickwalker Desktop won’t open

For a SmartScreen prompt, see the [Windows download instructions](../crates/sw-desktop/README.md#windows). If the Linux app exits at startup, check the [Linux graphics and system requirements](../crates/sw-desktop/README.md#linux).

## The computer cannot see the Stick

Use a USB-C cable that carries data. On Windows, close any serial monitor using the Stick's COM port. On Linux, follow the [USB permissions instructions](../crates/sw-desktop/README.md#linux-usb-permissions) and reconnect the Stick.

If the Stick is frozen or a firmware installation was interrupted, follow [Recover over USB](recovery.md).

## The Stick cannot open its save

Connect it to Stickwalker Desktop. Choose **Import** if you have an exported save. **Reset Walker** starts a fresh save and clears the Stick's DS pairing.
