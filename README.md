# Pad Test

This plugin shows on the TV what the cart sees of a USB game pad. Use it when a pad doesn't work right in a game. It doesn't open any files.

## Setup

Copy `PADTEST.CVP` into the `/PLUGINS` folder on the card. Start Pad Test from the Plugins page in the menu.

The cart can't power a pad. Plug the pad into a USB-C hub with its charger plugged in, or into a powered USB-A hub on an adapter. Then plug the hub into the cart. A pad with its own battery still needs the hub.

Pad Test 0.18 is the same test built with an older version of the USB code, TinyUSB 0.18. It's there to compare the two.

## Controls

There are no controls on the ColecoVision controller. Plug a pad in and press its buttons. Press RESET on the console to go back to the menu.

## What the screen shows

- The top line counts the seconds since the test started.
- The next line counts plugs and unplugs.
- Below that are the two newest USB devices and their IDs.
- P1 is pad 1. It says SWITCH or HID, and turns green when the pad works. A Switch pad says READY once it's set up.
- RPT shows how many reports a second come from the pad.
- Next is the pad's last report, in hex.
- BTN shows the buttons you're pressing, or the sticks.
- P2 shows the same for pad 2.
- The last four lines are a log of what happened, with times.

## Pads that work

- The Nintendo Switch Pro Controller, and 8BitDo pads in Switch mode.
- Plain HID pads, such as 8BitDo pads in D-input mode, the DualShock 4, and Logitech pads in D mode.

## Credits

- TinyUSB by Ha Thach, under the MIT license. The source is at https://github.com/hathach/tinyusb
- The plugin itself is by Parrot Cart.

## Build it yourself

You need the Raspberry Pi Pico SDK 2.1.1, an Arm GNU toolchain, CMake,
Ninja, Python 3, sjasmplus and picotool. Releases are built with the Arm
GNU Toolchain 9-2019-q4 (GCC 9.2.1) and picotool 2.3.1. With those you get
the same file, byte for byte.

```sh
cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
ninja -C build
```

This makes `build/PADTEST.CVP`. Copy it into the `/PLUGINS` folder on the card.

On Ubuntu, `tools/ci/setup.sh` installs the same tools that GitHub uses to
build each release.

`CART-README.md` is the text the cart shows on the plugin's page. It is
the start of this README.

## License

The plugin is under the MIT license (`LICENSE`). `THIRD-PARTY.md` lists
the parts that came from elsewhere, and their licenses.
