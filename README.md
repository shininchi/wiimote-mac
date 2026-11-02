# wiimote-mac

Use a real Wiimote and its add-ons on macOS, over Bluetooth, with no kernel driver
and without turning off System Integrity Protection.

The first supported setup is a **Wii Guitar Hero guitar in
[YARG](https://github.com/YARC-Official/YARG)**, played wirelessly. WiitarThing and
similar tools only run on Windows, and this project is an attempt to fill that gap
on the Mac.

> **Early prototype.** It works, but it's a terminal tool plus a manually patched copy
> of YARG, not yet an app. It has only been tested on one Mac (Apple Silicon, macOS 15)
> with one original Wiimote and one Guitar Hero III Les Paul.

## What works

| Guitar control | In YARG |
|---|---|
| All five frets | Yes |
| Strum up and down | Yes |
| Whammy, analog | Yes |
| Star power with Select | Yes |
| Star power by tilting | Yes |
| Start | Read, but not tried in YARG |
| Touch bar and stick | Read, but not sent to YARG |

## How it works

```text
Guitar -> Wiimote -> Bluetooth -> wiimote-test -> local connection -> add-on in YARG
```

- **`wiimote-test`** pairs with the Wiimote, reads the guitar and sends its state to YARG.
- **The add-on** is a small .NET library loaded by YARG. It shows up in YARG as a
  regular five-fret guitar, so YARG's own default bindings apply.

The add-on is needed because macOS doesn't let an ordinary app create a virtual
controller, which is how the Windows tools work. That has a consequence: **each game
needs its own add-on, and so far only YARG has one.**

## Requirements

- macOS 12 or later is expected to work. Only macOS 15 on Apple Silicon has been tested.
- Xcode command line tools and the [.NET SDK](https://dotnet.microsoft.com/download)
- An original Wiimote, a Guitar Hero guitar and YARG. This has been tested with YARG v0.15.0.

## Setup

Build the tool:

```bash
make
```

The add-on goes into a separate copy of YARG, so your real install stays untouched.
The copy keeps its own settings, profiles and scores. Set `YARG_APP` to wherever your
`YARG.app` is. With the YARC Launcher, it's in the launcher's `YARG Installs` folder.

```bash
mkdir yarg-copy
cp -R "$YARG_APP" yarg-copy/
(cd yarg-addon && dotnet build -c Release)
python3 scripts/install_addon.py yarg-copy/YARG.app yarg-addon/bin/Release/netstandard2.1/WiimoteGuitarBridge.dll
codesign --force --deep -s - yarg-copy/YARG.app
```

## Play

1. Pair the Wiimote. You only need to do this once. Run the command below, then press
   the red SYNC button under the battery cover, not 1 + 2.

   ```bash
   ./wiimote-test --pair
   ```

2. Plug in the guitar, open `yarg-copy/YARG.app`, and start the bridge:

   ```bash
   ./wiimote-test --yarg
   ```

3. Press a Wiimote button to connect, and press Ctrl-C when you're done. The first time,
   add a profile in YARG and choose **Wii Guitar** as its device. The guitar only
   appears in YARG once the bridge is sending data.

macOS may list the Wiimote as a "Magic Trackpad". That's just a wrong label.

## Checking your setup

Run `./wiimote-test` on its own to see live input without YARG. Pressing Ctrl-C prints
which controls were seen and how steady the connection was. These options can help:

- `--debug` shows the messages sent to and from the Wiimote.
- `--no-display` hides the live status line.
- `--seconds N` stops after N seconds.

To run the tests, which use a simulated Wiimote and need no hardware:

```bash
make test
```

## Known limits

- **Tilt and whammy are tuned for one guitar.** The values come from one Les Paul, and
  other guitars may need different ones.
- **YARG updates can replace the add-on.** It's also tied to how YARG loads code, so a
  new version could break it.
- **It relies on private macOS features.** Pairing and its permission workaround use
  them, because the public API can't send a Wiimote's PIN. A macOS update could break them.
- **Connection timing varies.** In a 12-minute session, reports averaged 84 per second,
  with no gap over 79 ms but about two gaps a second over 50 ms. It felt fine to play,
  but one earlier session was much slower and the cause is unknown.

## Future add-ons

The connection code is shared, so every add-on below is already identified when it's
plugged in. Each one still needs its data decoded, testing on real hardware, and
something to control in a game.

| Add-on | Status |
|---|---|
| Guitar Hero guitar | Works in YARG |
| Guitar Hero drums | Identified. The natural next step, since YARG supports drums |
| Classic Controller and Pro | Identified |
| Nunchuk | Identified |
| DJ Hero turntable | Identified |
| MotionPlus | Identified when active. Needs its own activation sequence |

Wii Remote Plus is expected to connect, but hasn't been tested.

Other planned work:

- A proper Mac app with built-in pairing and add-on installation
- More games, starting with checking whether Clone Hero is possible

## Credits

- [YARG](https://github.com/YARC-Official/YARG) and
  [PlasticBand-Unity](https://github.com/TheNathannator/PlasticBand-Unity) provide the
  guitar device this project plugs into.
- [Dolphin's WiimotePair](https://github.com/dolphin-emu/WiimotePair) showed how to pair a
  Wiimote on modern macOS.
- [WiimoteUserlandDriver](https://github.com/HakkinDavid/WiimoteUserlandDriver) showed that
  a Wiimote can be read without a driver.
- The guitar's data layout follows Linux's `hid-wiimote` driver.

## License

Not chosen yet. The license needs to take into account the projects above, in particular
Dolphin's GPL-licensed pairing tool and YARG's LGPL license.

This project isn't affiliated with or endorsed by Nintendo, Apple or the YARG project.
It doesn't include YARG or any game files.
