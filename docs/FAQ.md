# FAQ

## My sticks do nothing in HOTAS mode, but some buttons work

**The relay is probably listening to the wrong device.** The game only gets its sticks from the relay, so if the relay isn't reading your controller, the sticks do nothing. Any buttons that still work are coming straight from your real controller, because the relay hasn't hidden it.

Look at the relay window and move a stick:

| Check | Working | Wrong |
|---|---|---|
| `Controller:` at the top | Your real controller, like `Xbox-style controller (XInput)` | `generic DirectInput`, when you have an Xbox-style pad |
| `INPUT` box at the bottom | `XBOX` or `PS` | `HOTAS`, when you don't own a real joystick |
| `Updates/sec` while you move a stick | Jumps up | Stays at `0.0` |
| Stick bars | Follow your thumbs | Stay still |

**To fix it:**

1. **Get the [latest version](../../../releases/latest).** If the top line of your window has no version number, you're on v1.0.0, which has this problem and a sleep bug too. Since v1.4.1 the relay won't settle on a device that isn't sending anything.
2. **Close the relay and turn your controller on.** Wait until it's fully connected, then start the relay.
3. **Still wrong? Run `JoystickDoctor.exe`** from the zip, with the relay closed. It lists every joystick Windows knows about, shows which ones are really sending data, and offers to remove old leftovers. If you see something you don't use, like old pedals or a joystick from other software, unplug it or remove it there.
4. **Bind your sticks again in the game,** with the relay in HOTAS mode. Binds made while the relay was reading the wrong device won't be tied to its virtual joystick.

Still stuck? Run `JoystickDoctor.exe --list` and paste what it prints into an [issue](../../../issues), with a picture of the relay window.

### Doing it by hand

- **See what's connected:** press `Win+R` and run `joy.cpl`. While the relay runs you'll normally see your controller (unless it's hidden), `vJoy Device` and one virtual pad.
- **Remove old leftovers:** open Device Manager, choose **View > Show hidden devices**, and open **Human Interface Devices**. Grey entries aren't connected. Right-click a grey **HID-compliant game controller** and choose **Uninstall device**.
- **Leave everything else alone.** Don't remove anything that isn't grey, anything that looks like a keyboard or mouse, or any driver.

A leftover comes back by itself if that device is ever plugged in again, so removing one does no harm.

## My controller turned off in the middle of a game

Turn it back on. Since v1.0.1 the relay picks it up by itself and the game keeps its virtual pad, so you don't need to restart either one. On v1.0.0 this could freeze the sticks and leave your real controller visible to the game, so update.

## The game sees two controllers

Your real controller isn't hidden. Check two things:

- **Steam Input** has to be off for the game. See [Known Limitations](KNOWN_LIMITATIONS.md#steam-input-must-be-disabled).
- **Start the relay before the game.** Hiding can't take a controller away from a game that already has it open.

Some connections can't be hidden at all. An Xbox-style pad on a USB cable is the usual one, and its dongle usually works. See [What works and what doesn't](../README.md#what-works-and-what-doesnt).
