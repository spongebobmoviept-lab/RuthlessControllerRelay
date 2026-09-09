# Ruthless Controller Relay

Use a PlayStation or Xbox controller as a real HOTAS-style joystick for any game that expects one — or just as a clean, reliable regular gamepad — with full rumble, lightbar/LED color, and battery reporting where the hardware and connection type allow it.

Native C, no managed runtime, built directly against Win32 and three well-established community drivers. Requires Windows 10/11 and administrator rights (see [Why admin rights?](#why-admin-rights)).

## What this actually does

A flight-sim/HOTAS game binds its axes to a real joystick device. A gamepad isn't one. This tool sits in between:

1. Reads your real controller (Xbox, PlayStation 4/5, wired or wireless).
2. Feeds that data into **either** a virtual joystick ([vJoy](https://github.com/shauleiz/vJoy)) for HOTAS-style binding, **or** a virtual Xbox/PlayStation gamepad ([ViGEmBus](https://github.com/nefarius/ViGEmBus)) for normal games — switchable live, no restart.
3. Hides the real controller from every other app on the system ([HidHide](https://github.com/nefarius/HidHide)) so the game only ever sees the one virtual device it's supposed to, never both at once.

Rumble, LED color, and battery level are read back from the virtual device and forwarded to the real controller where the connection type allows it — see [Compatibility Cheat Sheet](docs/Controller_Compatibility_Cheat_Sheet.pdf) for the full breakdown of what works on which connection.

## Quick start

1. Download the latest build from [Releases](../../releases), or build it yourself (below).
2. Run `RuthlessControllerRelay.exe` (it will self-install vJoy/HidHide/ViGEmBus on first run if they aren't already present — see [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md) for exactly what that installer does and why it's built the way it is).
3. Connect a controller. The dashboard shows what it detected and what it's presenting to games.
4. Press `Ctrl+Alt+H` (or hold Back+Start on the controller) to switch between HOTAS mode and Normal mode.

Full control reference and a plain-English "is this working right now" guide: [docs/Controller_Compatibility_Cheat_Sheet.pdf](docs/Controller_Compatibility_Cheat_Sheet.pdf).

### Controls

| Key | Does |
|---|---|
| `Ctrl+Alt+H` (or Back+Start on the controller) | Switch between HOTAS mode and Normal mode |
| `V` | Flip the virtual output between PlayStation-style and Xbox-style |
| `R` | Change the hotkey/button combo for the mode switch above |
| `M` | Remap a button |
| `G` | Pick/launch a game |

## Building from source

```
zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra \
    -I third_party -I third_party/vigem_client \
    -o RuthlessControllerRelay.exe \
    src/hotas_relay.c resources/hotas_relay_manifest.res \
    -ldinput8 -ldxguid -lole32 -lcomdlg32 -ladvapi32 -lshell32 -lsetupapi -lnewdev -lhid
```

Compile the resource file first (embeds the UAC manifest + icon):
```
zig rc resources/hotas_relay.rc resources/hotas_relay_manifest.res
```

`third_party/vigem_client/ViGEmClient.dll` and the contents of `third_party/drivers/` need to sit next to the built exe at runtime — see [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md) for exactly why the driver-only payload is structured this way instead of shelling out to each vendor's own GUI installer.

Add `-DSAFE_MODE_NO_EXTRAS` to build a stripped-down fallback variant with no rumble/LED/battery code compiled in at all — useful as a minimal, maximally conservative build if the full one ever misbehaves on a specific machine.

Built and tested with [zig cc](https://ziglang.org/) as a self-contained cross-compiling C toolchain — no Visual Studio install required.

## Why admin rights?

The exe manages the lifecycle of a real kernel-mode virtual device (vJoy) that's created fresh on launch and fully torn down on exit, and it registers/hides devices through HidHide. Both need administrator rights on Windows, in both directions. It requests elevation on every launch, with no way around it — see [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md#session-scoped-vjoy) for the actual trade-off that decision was weighed against (a persistent idle device vs. one UAC prompt per launch) and why it was decided this way.

## What works and what doesn't

Full plain-English matrix, connection by connection: **[Controller Compatibility Cheat Sheet (PDF)](docs/Controller_Compatibility_Cheat_Sheet.pdf)**.

Short version:
- Wired PlayStation and Xbox controllers: everything works (input, rumble, LED color, battery).
- Bluetooth PlayStation controllers: input works perfectly; rumble/LED do not, and can't currently be made to — see [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) for the full, real investigation behind that conclusion.
- Wired Xbox controllers: fine for normal play, but HOTAS mode / the disguise-as-PlayStation toggle / button remapping are unreliable — this is a genuine Windows driver-architecture limitation, also detailed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md). Use a wireless dongle instead of a cable for full functionality.

## Before using this with any specific game

This tool installs and drives kernel-mode drivers that some anti-cheat systems actively look for and refuse to run alongside. Real, checked findings (not speculation) are in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md#anti-cheat) — read that before pointing this at any competitive/anti-cheat-protected game. A full `--uninstall` is built in for exactly this reason.

## The development story

This project went through real, sometimes serious, failures on the way here — including one that briefly bricked a keyboard and mouse system-wide. Every one of them is written up in detail, including the wrong turns, in **[docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md)** — not as a changelog, but as a guide so nobody building on this has to rediscover the same holes the hard way.

## Credits

This project is a thin layer of glue on top of the real engineering: three drivers, built and maintained by other people, doing all of the actual heavy lifting at the OS/kernel level. None of this works without them.

- **[vJoy](https://github.com/shauleiz/vJoy)** by Shaul Eizikovich — the virtual joystick device this entire HOTAS-mode feature is built on.
- **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** and **[ViGEmClient](https://github.com/nefarius/ViGEmClient)** by Benjamin Höglinger-Stelzer ([Nefarius Software Solutions](https://nefarius.at/)) — the virtual Xbox/DualShock gamepad emulation layer.
- **[HidHide](https://github.com/nefarius/HidHide)** by Eric Korff de Gidts and Benjamin Höglinger-Stelzer (Nefarius) — the device-hiding filter driver that makes "the game only ever sees one controller" actually work.

Full license texts and copyright notices for all four are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) — please keep them intact if you redistribute this. This project is a small addition on top of a mountain of real driver-development work by the people above; they deserve the credit for what actually makes the hard parts possible.

This project's own code was developed with [Claude Code](https://claude.com/claude-code) doing much of the implementation, debugging, and research, directed and hardware-tested throughout by the project owner.

## License

This project's own source code ([src/hotas_relay.c](src/hotas_relay.c) and everything else outside `third_party/`) is licensed under the [MIT License](LICENSE). The bundled third-party drivers and libraries under `third_party/` keep their own original licenses — see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
