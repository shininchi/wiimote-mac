# wiimote-mac

Use a real Wiimote and its add-ons on macOS, over Bluetooth, with no kernel driver
and without turning off System Integrity Protection.

The first supported setup is a **Wii Guitar Hero guitar in
[YARG](https://github.com/YARC-Official/YARG)**, played wirelessly. WiitarThing and
similar tools only run on Windows, and this project is an attempt to fill that gap
on the Mac.

> **Early prototype.** There's no download yet, so you build the app yourself. It has
> only been tested on one Mac (Apple Silicon, macOS 15) with one original Wiimote and
> one Guitar Hero III Les Paul.

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
Guitar -> Wiimote -> Bluetooth -> Wiimote Mac -> local connection -> add-on in YARG
```

- **Wiimote Mac** is a menu bar app that pairs with the Wiimote, reads the guitar and
  sends its state to YARG. `wiimote-test` does the same from a terminal, for testing.
- **The add-on** is a small .NET library loaded by YARG. It shows up in YARG as a
  regular five-fret guitar, so YARG's own default bindings apply.

The add-on is needed because macOS doesn't let an ordinary app create a virtual
controller, which is how the Windows tools work. That has a consequence: **each game
needs its own add-on, and so far only YARG has one.**

## Requirements

- macOS 12 or later is expected to work. Only macOS 15 on Apple Silicon has been tested.
- Xcode command line tools and the [.NET SDK](https://dotnet.microsoft.com/download)
- An original Wiimote, a Guitar Hero guitar and YARG. This has been tested with YARG v0.15.0.

## Build

Compiling the add-on needs YARG's files, so point `YARG_APP` at your `YARG.app`. With
the YARC Launcher, it's in the launcher's `YARG Installs` folder.

```bash
make app YARG_APP="/path/to/YARG.app"
```

This creates `build/WiimoteMac.app`. Move it wherever you like and open it. It adds an
icon to the menu bar, with no window or Dock icon.

## Play

The first time, from the menu bar icon:

1. Choose **Add Guitar to YARG**, with YARG closed. This adds the add-on file to your
   YARG and edits two of its startup lists, after backing them up. No game code changes,
   and **Remove from YARG** restores the originals.
2. Choose **Pair a New Wiimote**, then press the red SYNC button under the battery
   cover, not 1 + 2.
3. Plug in the guitar and open YARG however you normally do. In your profile, choose
   **Add New Device** and pick **Wii Guitar**. It only appears once the guitar is sending
   data.

After that, open YARG, press a Wiimote button and play. Turn on **Start at Login** to
keep the app ready. A YARG update can remove the add-on, in which case the menu asks you
to add it again.

macOS may list the Wiimote as a "Magic Trackpad". That's just a wrong label.

## Checking your setup

To see live input without YARG, build and run the terminal tool. Quit the app first,
since both would connect to the Wiimote. Pressing Ctrl-C prints which controls were
seen and how steady the connection was. `--yarg` and `--pair` do what the app does.

```bash
make
./wiimote-test
```

These options can help:

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

- A signed download, so people don't have to build the app
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
