# Head tracking

Turn your head and the camera follows, with both hands still on the sticks. All you need is a webcam, and it's set up from inside the relay. Off until you turn it on.

> **⚠️ Anti-cheat notice.** Since 2.0, head tracking sends **no mouse movement and no keys**. It moves two axes on the virtual joystick, and you map those on the game's own HOTAS screen. One player whose head tracker moved the mouse has reported a ban for hacking, and the game studio's CEO said its detection may have seen it as inhuman mouse movement. [Why we don't think 2.0 gets flagged](../README.md#no-mouse-no-keys). That's our reasoning, not a ruling: the game hasn't said head tracking is allowed, so nobody can promise it's safe. It's your account, so it's your call. [More about anti-cheat](KNOWN_LIMITATIONS.md#anti-cheat).

## What you need

- **A webcam** that can see your face. A phone or an HDMI camera works too, if Windows sees it as a camera.
- **Nothing else.** The tracker is built into the relay. Its engine and head model are in the `head_tracker` folder in the zip, so keep that folder next to the relay. There's nothing to install.
- **A game that lets you bind look axes to a joystick,** usually rows for looking left/right and up/down on its HOTAS or joystick screen. It's the same requirement as [free look](../README.md#free-look-hotas-mode).

## Setup

1. Start the relay and connect your controller.
2. Click the gear at the bottom right of the window, then **Head tracking**, and pick your camera.
3. Go back, click **Game**, and pick your game's `.exe`.
4. Go back, click **Look**, and choose **Head**.
5. **Map the look axes once:** see [Mapping the look axes](../README.md#map-the-look-axes-once). If you've mapped them for free look, you're done.

<p align="center"><img src="images/2.0-head.png" alt="The Head tracking page: an anti-cheat notice, then sliders for head speed, left/right speed, up/down speed and the still zone" width="420"></p>

Tracking starts when the game starts, so you don't have to open anything else.

To try it, start your game, switch to **HOTAS mode** and turn your head. The ring in the window shows a dot where you're pointing.

**Prefer the console?** `H` runs the same setup there.

**Playing something else?** Head tracking only runs while the one program you picked is running. Change it on the **Game** page, and map the look axes in that game too.

## Which style to use

Joystick axes aren't a mouse, so there are a few ways to turn a head into an axis. Pick one under **How your head moves the view** on the Head tracking page. If your game has a setting like **Absolute Freelook**, it matters here.

| Style | Game's Absolute Freelook | What it does |
|---|---|---|
| **Position** | On | Where your head points is where the view points. Responsive and can't drift. The game may snap to center near the middle: **Center skip** steps over that spot |
| **Speed** | Off | How fast your head turns is how fast the view turns. Slower and laggier, because it has to measure your head's speed, and it can drift off center |
| **Circle** | Try both | An experiment: your head turn goes around both axes, with forward at the top and behind at the bottom. Position or Speed will usually feel better |

## Controls

On a PlayStation controller, `RB` is `R1`.

| Control | Does |
|---|---|
| **Look** page, or `F` in the console | Free look: **off**, **stick** or **head**. The webcam is only on in head |
| Tap `RB` | Recenter. Look straight ahead first, because wherever your head is becomes straight ahead |
| Hold `RB` | Head tracking pauses and the right stick's left/right turns the camera. Up/down keeps flying |
| Let go of `RB` | The camera goes back to where your head has it, with the center unchanged if you moved the stick. If you didn't, it counts as a tap and recenters |
| Hold `RB` + click `R3` | Freeze the camera where it is. Tap `RB` to release |
| **Head speed** slider | Both directions together. `+` and `-` do the same in the console |
| **Left/right speed**, **Up/down speed** sliders | Each direction on its own. Raising up/down won't touch left/right |

Sliders work right away, even with the game running, and save by themselves. Head tracking only runs in **HOTAS mode**. In Normal mode the camera turns off, and comes back when you switch to HOTAS mode.

## What the Look box says

The console's HEAD box is the same, except that it has no **No face**, and says **RB: STICK** for **Stick**.

| Word | Meaning |
|---|---|
| **Off** | Head tracking is off, or you're in Normal mode |
| **Waiting** | Head tracking is on, but it isn't reading a head yet. Normal until your game is running. The line under it says what's missing |
| **No face** | The camera is on but can't see you |
| **Tracking** | Your head is moving the look axes |
| **Paused** | No head readings for a moment. It comes back by itself |
| **Stick** | You're holding RB, so the stick has the camera |
| **Locked** | The camera is frozen with RB + R3 |

## Your mouse is safe

The relay can't move your mouse or press your keys. It has no code that does. So there's nothing to get in your way on your desktop, on another monitor or in a menu.

## If something's off

| Problem | Try |
|---|---|
| **Tracking**, but the camera doesn't move | [Map the look axes](../README.md#map-the-look-axes-once). They have to be mapped through the game's own screen, while the relay is in HOTAS mode |
| Stuck on **Waiting** with the game running | Face the camera, because tracking starts when it first sees your face. Check that the `head_tracker` folder is next to the relay, and check the camera and the game in settings. Make sure no other program is using the camera: a camera can usually only be used by one program at a time. A virtual camera also needs its own app running |
| The view snaps to center near the middle | That's the game treating near center as letting go, with Absolute Freelook on. Raise **Center skip** a little, or use **Speed** with Absolute Freelook off |
| The view drifts off center | **Speed** can drift, because nothing tells the game where center is. Tap `RB`, or use **Position** with Absolute Freelook on |
| Up/down or left/right is backwards | **Flip up/down** or **Flip left/right** on the Head tracking page |
| Too fast or too slow | **Head speed**, or the separate speeds for each direction |
| Small shakes | Raise **Still zone** |
| Feels delayed or stutters | Close anything heavy that's running while you play. Compiling code during a test made tracking noticeably worse. Better light and a higher frame rate camera also help |
| In a chase view, the camera stays pointing one way while the vehicle turns | Some games lock free look to a compass direction in outside views. Tap `RB` to recenter, or use head tracking in the cockpit view, where it stays with the vehicle |

## Settings

These live in `ruthless_controller_relay.ini`, next to the relay. The window changes all of them. Close the relay before editing by hand.

| Setting | Does | Default |
|---|---|---|
| `freelook` | `off`, `on` or `head` | `on` |
| `head_speed` | How far your head has to turn. `50` is the full axis at 45 degrees of head turn, or at 90 degrees a second in Speed style | `50` |
| `head_yaw` | Multiplies left/right. `0` turns it off, negative flips it | `1.00` |
| `head_pitch` | Multiplies up/down. `0` turns it off, negative flips it | `1.00` |
| `head_deadzone` | Head wobble smaller than this many degrees is ignored | `0.15` |
| `look_axis_style` | `straight` (Position), `circle` or `speed`. `Ctrl+Alt+J` cycles them with 1, 2 or 3 beeps | `straight` |
| `look_center_skip` | Percent of the axis to step over around center, Position style only, `0` to `25`. `Ctrl+Alt+Up` and `Ctrl+Alt+Down` change it live | `0` |
| `head_show_camera` | Shows your picture in the head ring, in this window only | `off` |
| `game_path` | The program head tracking runs with | none |
| `head_camera` | The camera to use | none |
| `head_width`, `head_height`, `head_fps` | The video mode to ask the camera for, or the closest it has | `640`, `480`, `60` |

## Cameras

Anything Windows offers as a camera works, virtual ones like OBS Virtual Camera and NVIDIA Broadcast included. A virtual camera only delivers video while its own app is running. If a camera stops sending video, tracking stops, and the relay tries it again every 10 seconds.

The tracker shrinks a larger picture to at most 640 wide before reading it, so a higher resolution doesn't make tracking sharper.

The tracker uses real CPU while it runs: about 1.6 cores for the whole relay in testing on an 8-core desktop CPU. The camera is off with no game running, in Normal mode, and in `off` or `stick`.

## How it works

Your webcam goes to a tracker inside the relay, which works out where your head is pointing. The picture stays inside the relay and is never saved or sent anywhere. The relay puts the result on two axes of the virtual joystick, Slider0 and Slider1, and the game reads them like any other joystick axis. The tracker runs on its own threads, so the path from your controller to the game isn't touched. If you close the relay, the camera closes with it. The method is opentrack's, ported to C: see [Credits](../README.md#credits).

## Removing it

Set Look to Off or Stick, and nothing runs.

Nothing was ever installed, and nothing goes in the registry or your Documents. Delete the `head_tracker` folder and head tracking is gone.
