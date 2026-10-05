# Changelog

## v1.2.0 — 2026-10-05

### Added
- **Stream Deck and macro-pad control (optional, off by default).** Set `control_pipe=on` in the ini, and a Stream Deck, any macro pad or a script can:
  - switch HOTAS/Normal mode,
  - flip the virtual pad between Xbox and PlayStation,
  - turn free look on or off,
  - read the mode, controller, battery and live input,
  - close the relay cleanly.

  It works while an administrator game has focus, where keyboard shortcuts sent by other apps are blocked. It runs over a local-only pipe that understands just these commands. Commands are applied between controller reads, so input is untouched. The `_SAFE` build leaves it out. Guide: [docs/STREAM_DECK.md](docs/STREAM_DECK.md).
- **`RelayControl.exe`**: sends one of those commands from a Stream Deck button, a shortcut or a script (`RelayControl.exe mode toggle`). No admin prompt, no window.
- **Discord button in Normal mode (optional, off by default).** Discord's keybinds can't see the virtual Xbox pad, so Normal mode had no controller button Discord could bind. With `discord_button=dpad_down`, D-pad down in Normal mode also presses virtual-joystick button 18. Nothing else uses it on an Xbox controller, and typical game joystick bindings don't either. Bind Discord's Toggle Mute to it. HOTAS mode is unchanged. A press only counts if it starts in Normal mode. Details: [docs/STREAM_DECK.md § Discord button](docs/STREAM_DECK.md#discord-button-in-normal-xbox-mode).

### Changed
- Nothing changes unless you turn these on. With `control_pipe` and `discord_button` left at their defaults, the relay behaves exactly like v1.1.0, and the virtual joystick is identical: same 20 buttons, same axes.

## v1.1.0 — 2026-10-02

### Added
- **Free look in HOTAS mode.** Hold RB and the right stick's left/right turns the camera, while up/down keeps flying pitch. Let go and the camera snaps back; the stick flies again instantly.
  - **Lock:** RB + click R3 keeps the camera where it is after you let go. Tap RB or click R3 to unlock.
  - **Speed:** change it live with `+` / `-` on the dashboard. It's saved as `freelook_speed=` in the ini.
  - **Off switch:** works through the game's own Left Alt + mouse free look. Turn it off with `freelook=off` in the ini for games where that isn't free look.
  - Details: [README](README.md#free-look-hotas-mode).
- **A tiny dead zone on the virtual gamepad's sticks (Normal mode only, about 3%).** Some hall-effect sticks wobble a hair off center, and games with no dead-zone setting of their own show that as drift. It's scaled, so the stick still starts smoothly from zero and still reaches full deflection. HOTAS mode is unchanged and still passes the stick through untouched.
- **The dashboard shows a loud banner while you're in free look** (yellow) or have the camera locked (magenta).

### Changed
- **Steadier virtual-joystick output.** Every update now goes to the virtual joystick as one complete report, re-sent at a steady 1,000 per second like a real joystick, even when nothing moves. Before, it only sent anything when the stick moved, and each change arrived as about 25 half-finished pieces. Some games act on each report as it arrives, and those saw stutter while moving and stalls when the stick was held still.
- **The virtual joystick now declares two extra slider axes and 20 buttons.** Existing bindings keep working, because the axis and button numbers that were already there didn't move. Some games remember joystick bindings in odd ways, so it's worth a quick check after updating.

How free look ended up working this way, including the approaches that didn't: [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md#free-look-in-hotas-mode-three-dead-ends-and-what-worked-v110).

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
