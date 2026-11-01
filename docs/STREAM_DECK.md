# Stream Deck and macro-pad control

Added in v1.2.0. **Optional and off by default.**

Turn it on and a Stream Deck, any other macro pad, or a script can control the relay and read its status:

- switch between **HOTAS mode and Normal mode**,
- switch the virtual pad between **Xbox and PlayStation**,
- switch **free look** between off, normal and head tracking,
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
| Free look normal / off | `freelook toggle` |
| Head tracking on | `freelook head` |
| Close the relay | `quit` |

This works the same with any macro pad or launcher that can open a shortcut or run a program with arguments.

**Showing status on the key** (battery, current mode and so on) needs a plugin that reads the reply. See [For plugin and script authors](#for-plugin-and-script-authors) below.

## Use it from a script

```bat
RelayControl.exe status
```
prints, for example:
```
ok v=1.3.0 state=connected mode=hotas pad=x360 padlive=1 freelook=on look=idle batt=80 chg=0 ctrl=Xbox-style controller (XInput)
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
| `freelook on` / `freelook off` / `freelook toggle` / `freelook head` | `on` is normal free look, and `head` is [head tracking](HEAD_TRACKING.md). `toggle` switches between normal and off, and from head it goes to off. Not saved by itself, but a later settings change such as `+`, `-`, `F` or `C` saves it | status line |
| `input` | Live controller sample, plus head tracking | `ok lx= ly= rx= ry= lt= rt= btn= ps= hs= hx= hy=`, or `err no controller` |
| `quit` | Close the relay cleanly, exactly like closing its window | `ok quitting` |

Status fields:

| Field | Values |
|---|---|
| `v` | relay version |
| `state` | `starting`, `waiting` (no controller), `connected`, `menu` (a dashboard menu is open) |
| `mode` | `hotas`, `normal`, `unset` (not picked yet since launch) |
| `pad` | `x360` or `ds4`: the virtual pad type |
| `padlive` | `1` if the virtual pad is live |
| `freelook` | `on` / `off` / `head` |
| `look` | `idle`, `looking` (RB held), `locked` (camera locked with R3) |
| `batt` | controller battery in %; `empty`, `low`, `medium` or `full` for controllers that only report a level; `wired`; or `-` if unknown |
| `chg` | `1` while charging |
| `ctrl` | controller name. Always the **last** field and runs to the end of the line, since names can contain spaces |

`input` fields: `lx ly rx ry` are the sticks (-32768..32767), `lt rt` the triggers (0..255), `btn` the XInput button bits, `ps` the PlayStation button (0/1). `hs` is head status: 0 off, 1 waiting, 2 tracking, 3 paused, 4 RB: stick, 5 locked. `hx hy` are where the head points, -100..100, with positive being right and down on the HEAD box. Ignore them when `hs` is 0 or 1. The relay refreshes this sample about 15 times a second, so polling faster gains nothing. Polling `status` several times a second is fine.

Pad and free-look changes are applied a moment after the reply (between controller reads), so the status line in that reply may still show the old value. Send `status` again to see the change.

Errors: `err unknown command`, `err no controller`, and similar. Treat any line that doesn't start with `ok` as "didn't happen".

---

# Discord button in Normal (Xbox) mode

Added in v1.2.0. **Optional and off by default.** Set `discord_button=` in the ini.

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

---

# Discord's noise filter from a Stream Deck (Vencord)

**Optional, and separate from the relay.** The relay doesn't use this or need it. It's here because it goes well with a Stream Deck setup.

## What it's for

A key or script that switches Discord's Krisp noise filter on and off **in the background**. Discord doesn't come to the front, and whatever you're typing in keeps the keyboard. It can also tell you whether you're in a call, muted or deafened, so a key can show that.

> **⚠️ Notice.** This uses [Vencord](https://github.com/Vendicated/Vencord), a Discord client mod. Client mods are against Discord's Terms of Service. Vencord's own README says there are no known bans for using them, and also says not to use one if losing your account would be a disaster. It's your account, so it's your call. This is an unofficial add-on, and neither Vencord nor Discord supports it. It can stop working after a Discord update.

## How it works

Discord has no outside control for Krisp, but inside Discord a plugin can call the same action Discord's own switch uses. So a small Vencord plugin opens a local pipe, `\\.\pipe\DiscordBridge`, and passes what it receives to Discord's window:

| Send | Does |
|---|---|
| `status` | Nothing. Just answers |
| `krisp on`, `krisp off`, `krisp toggle` | Switches Krisp, then answers |

The answer is one line, like `ok incall=1 krisp=1 mute=0 deaf=0`. A pipe is local only, so nothing on the network can reach it. Any program running on your PC as you can, though.

## Set it up

Vencord only loads custom plugins when it's built from source, so this needs [git](https://git-scm.com/) and [Node.js](https://nodejs.org/) with pnpm (`npm i -g pnpm`).

```sh
git clone https://github.com/Vendicated/Vencord
cd Vencord
pnpm install --frozen-lockfile
```

Copy the `extras/vencord-bridge/streamDeckBridge` folder from this repo into Vencord's `src/userplugins/` folder, creating that folder if it isn't there. Then:

```sh
pnpm build
pnpm inject
```

Restart Discord. The plugin turns itself on. To test it, run this in PowerShell, with Discord open:

```powershell
$p = [IO.Pipes.NamedPipeClientStream]::new('.', 'DiscordBridge', 'InOut')
$p.Connect(2000); $w = [IO.StreamWriter]::new($p); $w.WriteLine('status'); $w.Flush()
[IO.StreamReader]::new($p).ReadLine(); $p.Dispose()
```

Change `status` to `krisp toggle`, save it as a `.ps1`, and point a Stream Deck **System > Open** key at it. To update later, run `git pull`, `pnpm install --frozen-lockfile` and `pnpm build` in the same folder. If a Discord update ever removes Vencord, run `pnpm inject` again. `pnpm uninject` removes it.
