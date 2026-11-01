# Head tracking

Turn your head and the camera follows, with both hands still on the sticks. All you need is a webcam, and it's set up from inside the relay window. New in v1.3.0, and off until you turn it on.

> **⚠️ Anti-cheat notice.** Head tracking sends your head movement to the game as **mouse movement from software**, constantly, while it's on. Some anti-cheat systems study how players use the mouse and keyboard, and could treat that as suspicious. No bans from this are known, but nobody outside the game's developers can promise that. It's your account, so it's your call. [More about anti-cheat](KNOWN_LIMITATIONS.md#anti-cheat).

## What you need

- **A webcam** that can see your face. A phone or an HDMI camera works too, if Windows sees it as a camera.
- **opentrack**, which does the actual face tracking. Its installer is in the zip, and setup offers to run it.
- **A game where holding Left Alt and moving the mouse is free look.** It's the same requirement as [free look](../README.md#free-look-hotas-mode).

## Setup

1. Start the relay and connect your controller.
2. Press **`H`** in the relay window.
3. If opentrack isn't installed, press **`Y`** to install it.
4. Press the number of your camera.
5. Pick your game's `.exe` in the window that opens. If the relay already knows one, it shows you, and you can press **`G`** to pick a different one or any other key to keep it.

<p align="center"><img src="images/head-tracking-setup.png" alt="The head tracking setup screen: an anti-cheat notice, a list of cameras, the program it runs in, and the controls" width="600"></p>

That's it. Head tracking is now on. Tracking starts when the game starts, so you don't have to open anything else.

To try it, start your game, switch to **HOTAS mode** and turn your head. The **HEAD** box next to the stick bars shows a dot where you're pointing:

<p align="center"><img src="images/dashboard-head-tracking.png" alt="The relay window in HOTAS mode with head tracking on. The HEAD box shows a dot up and to the right of center" width="640"></p>

**Playing something else?** Head tracking only runs in the one program you picked, and it works in anything where Left Alt + mouse is free look. Press `H`, pick your camera, then press `G` and pick the other game's `.exe`.

## Controls

| Control | Does |
|---|---|
| `F` | Free look: **off**, **normal** or **head**. The webcam is only on in head |
| Tap `RB` | Recenter. Look straight ahead first, because wherever your head is becomes straight ahead |
| Hold `RB` | Head tracking pauses and the right stick's left/right turns the camera, so you can look behind you. Up/down keeps flying |
| Let go of `RB` | The camera goes back to where your head has it, with the center unchanged |
| Hold `RB` + click `R3` | Freeze the camera where it is. Tap `RB` to release and recenter |
| `+` / `-` | Head speed (saved automatically) |
| `H` | Run setup again, to switch cameras or games |

Head tracking only runs in **HOTAS mode**. In Normal mode it stops by itself, and comes back when you switch to HOTAS mode.

## What the HEAD box says

| Word | Meaning |
|---|---|
| **OFF** | Head tracking is off, or you're in Normal mode. Press `F` |
| **NO TRACKER** | Nothing is arriving from opentrack. Normal until the game is running. If the game is running, see below |
| **TRACKING** | Your head is moving the camera |
| **PAUSED** | The game you picked doesn't have the mouse right now: you're in a menu, in another program, or your cursor is on another monitor. It comes back by itself |
| **RB: STICK** | You're holding RB, so the stick has the camera |
| **LOCKED** | The camera is frozen with RB + R3 |

## Your mouse is safe

Head tracking only moves the mouse while **all** of these are true:
- you're in HOTAS mode,
- the program you picked in setup has focus,
- your cursor is over it,
- the game has hidden the cursor, which it does while you're flying but not in menus.

So it stays out of your way on your desktop, on another monitor and in menus like an inventory.

## If something's off

| Problem | Try |
|---|---|
| Stuck on **NO TRACKER** with the game running | Press `H` and pick your camera again. Make sure no other program is using the camera. A camera can usually only be used by one program at a time |
| Stuck on **PAUSED** while you're playing | Setup may have the wrong `.exe`. Some games start from a launcher and then run as a different program, which Task Manager's Details tab shows while you play. Press `H`, then `G` and pick that one. In Unreal Engine games it usually ends in `-Win64-Shipping.exe`. A game that keeps the mouse cursor showing while you play stays on PAUSED too |
| **TRACKING**, but the camera doesn't move | Check your game's free look setting. It was tested with vehicle free look set to on by default. Also check that Left Alt + mouse is free look in your game |
| Up/down is backwards | Set `head_pitch=-1` in `ruthless_controller_relay.ini` (close the relay first) |
| Left/right is backwards | Set `head_yaw=-1` the same way |
| Too fast or too slow | `+` and `-` in the relay window |
| Small shakes | Raise `head_deadzone`, or lower the speed with `-` |
| Feels delayed or stutters | Close anything heavy that's running while you play. Compiling code during a test made tracking noticeably worse. Better light and a higher frame rate camera also help |
| Keyboard shortcuts act strange in the game | While the game has the mouse, the relay holds Left Alt, so shortcuts see Alt held. Stream Deck keys and the like are unaffected if they don't send keystrokes |
| In a chase view, the camera stays pointing one way while the vehicle turns | Some games lock free look to a compass direction in outside views. Tap `RB` to recenter, or use head tracking in the cockpit view, where it stays with the vehicle |

## Settings

These live in `ruthless_controller_relay.ini`, next to the relay. Close the relay before editing.

| Setting | Does | Default |
|---|---|---|
| `freelook` | `off`, `on` or `head` | `on` |
| `head_speed` | Mouse movement per degree of head turn | `50` |
| `head_yaw` | Multiplies left/right. `0` turns it off, negative flips it | `1.00` |
| `head_pitch` | Multiplies up/down. `0` turns it off, negative flips it | `1.00` |
| `head_deadzone` | Head wobble smaller than this many degrees is ignored | `0.15` |
| `game_path` | The program head tracking runs in. Change it with `H` rather than by hand, so opentrack learns about it too | none |

## Fine tuning in opentrack

Setup makes an opentrack profile called `relay-head.ini`, and opentrack sits in your system tray while head mode is on. Open it from there to see your camera, change smoothing and curves, or pick a different tracker such as TrackIR-style hardware or a Tobii. Whatever you use, its **output must stay on "UDP over network", 127.0.0.1, port 4242**, because that's how it reaches the relay. Running setup again only changes the camera and keeps the rest of what you tuned.

The tracker uses real CPU while it runs: 1.5 to 3 cores in testing on a Ryzen 7 5800X3D. In `off` or `normal`, opentrack is closed and the webcam is off.

## How it works

Your webcam goes to opentrack, which works out where your head is pointing and sends that to the relay on your own PC only. The relay is the only thing that talks to the game, and it turns your head movement into mouse movement while Left Alt is held, the same route as [free look](../README.md#free-look-hotas-mode). It runs on its own thread, so the path from your controller to the game isn't touched. If you close the relay, opentrack closes with it and Left Alt is released.

## Removing it

Set free look to off or normal with `F`, and nothing runs. To remove opentrack itself, uninstall it from Windows like any other program. The relay's `--uninstall` doesn't touch it, since you may use it for other things.
