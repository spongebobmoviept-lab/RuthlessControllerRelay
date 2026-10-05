# Stream Deck and macro-pad control

New in v1.2.0. **Optional and off by default.**

Turn it on and a Stream Deck, any other macro pad, or a script can control the relay and read its status:

- switch between **HOTAS mode and Normal mode**,
- switch the virtual pad between **Xbox and PlayStation**,
- turn **free look** on or off,
- read the status: mode, controller, battery, free look, and the live sticks and buttons,
- **close** the relay cleanly.

## Why it works this way

The relay runs as administrator, and so do many games and launchers (Steam often does). Windows blocks normal programs from sending keystrokes into administrator programs. So a Stream Deck button that "presses Ctrl+Alt+H" does nothing while an admin game has focus. That is exactly when you want it to work.

Instead, the relay can listen on a small **local control pipe** (`\\.\pipe\RuthlessControllerRelay`). A Stream Deck plugin or the included `RelayControl.exe` sends it a short text command, and it answers with one line.

Safety:
- **Local only.** Nothing on the network can connect to it.
- It only understands the handful of commands listed below. It can't run programs, read files or do anything else.
- The controller path isn't touched. Commands are applied by the relay between controller reads, the same way the dashboard's own keys are. It doesn't slow down or change your input.
- The pipe is created only when `control_pipe=on` is set. The `_SAFE` build leaves this feature out completely.

## Turn it on

1. Open `ruthless_controller_relay.ini` (next to the exe; it's created on first run).
2. Set `control_pipe=on`.
3. Restart the relay. The window shows **Stream Deck / macro-pad control: ON**.

## Use it from a Stream Deck (no plugin needed)

The release includes **`RelayControl.exe`**. It sends one command to the relay and exits. It runs as a normal user, so it never shows an admin prompt, and it has no window, so buttons don't flash anything.

1. Right-click `RelayControl.exe` and choose **Create shortcut**.
2. Right-click the shortcut, choose **Properties**, and add the command after the path in **Target**. For example:
   `"C:\...\RelayControl.exe" mode toggle`
3. In the Stream Deck app, drag a **System > Open** action onto a key and pick that shortcut.
4. Repeat for each button you want. Useful ones:

| Button | Command |
|---|---|
| Switch HOTAS / Normal | `mode toggle` |
| HOTAS mode | `mode hotas` |
| Normal mode | `mode normal` |
| Xbox / PlayStation virtual pad | `pad toggle` |
| Free look on / off | `freelook toggle` |
| Close the relay | `quit` |

This works the same with any macro pad or launcher that can open a shortcut or run a program with arguments.

**Showing status on the key** (battery, current mode and so on) needs a plugin that reads the reply. See [For plugin and script authors](#for-plugin-and-script-authors) below.

## Use it from a script

```bat
RelayControl.exe status
```
prints, for example:
```
ok v=1.2.0 state=connected mode=hotas pad=x360 padlive=1 freelook=on look=idle batt=80 chg=0 ctrl=Xbox-style controller (XInput)
```

Exit codes:

| Code | Meaning |
|---|---|
| `0` | The relay answered `ok ...` |
| `1` | The relay answered `err ...` (for example, a pad switch with no controller connected) |
| `2` | The relay isn't running, `control_pipe=on` isn't set, or there was no answer |
| `3` | Bad usage |

`RelayControl.exe` is a windowless program, so PowerShell doesn't wait for it on its own. Pipe its output (`RelayControl.exe status | Out-String`) or use `Start-Process -Wait` if you need the exit code.

## For plugin and script authors

The protocol: **one command per connection.**
1. Connect to `\\.\pipe\RuthlessControllerRelay`. It serves one client at a time. If you get `ERROR_PIPE_BUSY`, wait and retry.
2. Write one line of text ending in `\n`.
3. Read one line back.
4. Close the connection.

The relay drops a connection that sends nothing within about 1 second.

Commands (case-insensitive):

| Command | Does | Reply |
|---|---|---|
| `status` | Nothing; reports the state | `ok v=… state=… mode=… pad=… padlive=… freelook=… look=… batt=… chg=… ctrl=…` |
| `mode hotas` / `mode normal` / `mode toggle` | Switch mode | status line |
| `pad x360` / `pad ds4` / `pad toggle` | Switch the virtual pad type (needs a connected controller) | status line, or `err no controller` |
| `freelook on` / `freelook off` / `freelook toggle` | Free look on/off for this session (not saved to the ini) | status line |
| `input` | Live controller sample | `ok lx= ly= rx= ry= lt= rt= btn= ps=` |
| `quit` | Close the relay cleanly, exactly like closing its window | `ok quitting` |

Status fields:

| Field | Values |
|---|---|
| `v` | relay version |
| `state` | `starting`, `waiting` (no controller), `connected`, `menu` (a dashboard menu is open) |
| `mode` | `hotas`, `normal`, `unset` (not picked yet since launch) |
| `pad` | `x360` or `ds4`: the virtual pad type |
| `padlive` | `1` if the virtual pad is live |
| `freelook` | `on` / `off` |
| `look` | `idle`, `looking` (RB held), `locked` (camera locked with R3) |
| `batt` | controller battery in %, or `-` if unknown / wired |
| `chg` | `1` while charging |
| `ctrl` | controller name. Always the **last** field and runs to the end of the line, since names can contain spaces |

`input` fields: `lx ly rx ry` are the sticks (-32768..32767), `lt rt` the triggers (0..255), `btn` the XInput button bits, `ps` the PlayStation button (0/1). The relay refreshes this sample about 15 times a second, so polling faster gains nothing. Polling `status` several times a second is fine.

Pad and free-look changes are applied a moment after the reply (between controller reads), so the status line in that reply may still show the old value. Send `status` again to see the change.

Errors: `err unknown command`, `err no controller`, and similar. Treat any line that doesn't start with `ok` as "didn't happen".

---

# Discord button in Normal (Xbox) mode

New in v1.2.0. **Optional and off by default.** Set `discord_button=` in the ini.

## What it's for

You want one controller button to **toggle your Discord mute while you play in Normal (Xbox) mode**.

The catch: **Discord's keybinds can't see the virtual Xbox pad** the relay gives your game, but they *can* see the relay's virtual joystick (vJoy). In HOTAS mode every button already reaches vJoy, so Discord can bind them. In Normal mode, vJoy is deliberately kept silent so the game never sees your input twice. That left nothing for Discord to bind.

## What it does

With `discord_button=dpad_down`, pressing **D-pad down in Normal mode** also presses **vJoy button 18**.
- Button 18 is otherwise only used by a PlayStation controller's touchpad click, so an Xbox controller never presses it. Typical game joystick bindings don't use it either.
- **HOTAS mode is unchanged.** There, D-pad down does exactly what it always did and never presses button 18. So a HOTAS-mode binding on D-pad down (in the author's case, a supply-crate drop) can't trigger your Discord mute.
- In Normal mode the game still gets the button on the virtual Xbox pad as usual. Only button 18 is added on the joystick side.
- A press counts only if it **starts** in Normal mode. Holding the button through a mode switch won't toggle Discord.

## Set it up

1. In `ruthless_controller_relay.ini`, set `discord_button=dpad_down` (or another button, see below). Restart the relay.
2. Switch the relay to **Normal mode**. Do this first: right after launch the mode is "unset" and both outputs are live, so Discord would record the wrong button.
3. In Discord: **User Settings → Keybinds → Add a Keybind**, pick **Toggle Mute**, click **Record Keybind**, press the button on your controller, then stop recording.

Tapping the button now flips your Discord mute on and off in Normal mode.

## Options and notes

- Allowed buttons: `a b x y lb rb back start l3 r3 dpad_up dpad_down dpad_left dpad_right`. Triggers, PS, touchpad and Guide can't be used.
- Several buttons joined with `+` all press the same button 18.
- **Don't use your mode-switch button(s).** The relay warns about this at startup.
- **PlayStation controllers:** button 18 is the touchpad click by default, so in HOTAS mode the touchpad would also toggle Discord. That doesn't apply if the touchpad is your mode button, or if you move it with `M`.
- `discord_button=none` turns it off. With it off, nothing about vJoy changes in either mode.
