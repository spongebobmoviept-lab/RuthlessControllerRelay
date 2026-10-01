# Changelog

## v1.0.1 — 2026-10-01

**Bug-fix release. Everyone on v1.0.0 should update.**

### Fixed
- **The relay could lock onto its own virtual controller after your real controller turned off or fell asleep.** The dashboard said READY but showed `Updates/sec: 0.0`, a "wired" battery and frozen stick values. Turning the controller back on did nothing, and the game had to be restarted. Most noticeable with wireless controllers that put themselves to sleep after a few minutes of no input.
- **The game lost its virtual controller whenever the real one disconnected.** The virtual gamepad is now created once and stays for the whole session, as the design always intended. It sits centered while nothing is connected, so the game keeps the same controller across any number of disconnects and reconnects.
- **Your real controller could become visible to the game after it reconnected.** When the relay briefly couldn't find the controller (for example while it was asleep), it removed it from HidHide's hidden list. It now stays hidden.
- **A stick or button held at the moment the controller dropped could stay stuck.** Both the virtual joystick (vJoy) and the virtual gamepad now get one centered, released frame on disconnect.
- **Rumble is forwarded to the reconnected controller even if it comes back on a different slot.** Previously the slot was fixed at startup.

### Added
- The version number now shows at the top of the relay window.

### Build
- Now also links `cfgmgr32` (`-lcfgmgr32`). See [Building from source](README.md#building-from-source).

Full write-up of what was missed, how, and why: [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md#a-sleeping-controller-made-the-relay-read-its-own-virtual-pad-fixed-in-v101).

## v1.0.0 — 2026-09-09

Initial public release.
