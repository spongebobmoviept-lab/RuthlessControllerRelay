# Known limitations

Both of these are real, deliberately investigated conclusions, not gaps left from lack of effort. Each is written up in enough technical detail here that nobody needs to re-derive it from scratch.

## Wired Xbox controllers can't be hidden from other apps

**Symptom:** with a wired Xbox controller, HOTAS mode may look like it's working on this tool's own dashboard while the actual game keeps reading the real controller directly and behaves like an ordinary gamepad. The PlayStation-disguise toggle (`V`) and button remapping (`M`) can be bypassed the same way.

**Root cause:** [HidHide](https://github.com/nefarius/HidHide) can only hide devices that enumerate under the generic Windows `HIDClass`. A directly wired Xbox-compatible controller does not — it enumerates under Microsoft's own `XnaComposite` device class, a different driver stack entirely, which HidHide has no ability to intercept. This was confirmed directly: with a wired controller and HidHide's cloak active, the controller's raw XInput identity kept reaching a test game the entire time, even though this tool's own virtual-device mute/live switching was working exactly as intended underneath. This is a Windows/driver-architecture fact about how that specific device class is implemented, not a missing configuration step or a bug in this project's own HidHide integration.

**What does work:** the identical controller, connected through a wireless dongle (the official Xbox Wireless Adapter, or the controller's own dongle if it has one) enumerates as a genuine `HIDClass` device instead, and HidHide can hide it exactly like any other controller — confirmed working. Plain, ordinary gamepad use (no HOTAS mode, no disguise toggle, no remapping) is completely unaffected by any of this either way, wired or not, since both the real and virtual device carry identical data regardless of whether hiding succeeds.

**If you're extending this code:** don't spend time trying to make HidHide hide an `XnaComposite` device — it's not a matter of finding the right device path or registration call, the driver class itself is the obstacle. The only known way around it is a different connection type (dongle instead of cable), which is a hardware-level workaround, not a software one.

## Bluetooth PlayStation-controller rumble and LED output

**Symptom:** a DualShock 4 or DualSense connected over Bluetooth reports input perfectly, but cannot be made to rumble or change lightbar color from this tool. USB/wired connections for the identical controllers have both working correctly.

This was investigated more thoroughly than almost anything else in this project — real hardware testing across two entirely different Bluetooth radio chipsets, plus a source-level review of how other real, mature tools handle (or fail to handle) the exact same problem — and the conclusion was arrived at deliberately, not from giving up early.

### What was tried

**Two Win32 output APIs**, both tested against **two different Bluetooth adapters** (the machine's original internal radio, and a separately-added USB adapter using an entirely different chipset vendor, added specifically to rule out "this is one specific radio's quirk"):

| API | Original radio | Second, different-vendor radio |
|---|---|---|
| `HidD_SetOutputReport` | No crash, but the real controller never visibly reacts — Windows reports success on every call regardless | Identical: no crash, no visible effect, Windows still reports success |
| `WriteFile` | Destabilizes the Bluetooth link within seconds (rapid disconnect/reconnect cycling logged) | Identical destabilization — ruling out "this is specific to one radio chipset" |

Both APIs were tested with the output handle fully separate from the continuous input-reading handle, per this project's own confirmed rule that a second handle to the same physical wired device needs care (see [DEVELOPMENT_JOURNEY.md](DEVELOPMENT_JOURNEY.md)) — the Bluetooth-specific failure here is a distinct mechanism from that wired-handle finding, not the same bug appearing twice.

### What the research found

A review of [DS4Windows](https://github.com/Ryochan7/DS4Windows)'s actual current source (credited in full in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md)) found that project has **itself flip-flopped between these same two APIs for this exact case**, with an explicit source comment that DualSense "seems to only accept output data via the Interrupt endpoint" — i.e. `WriteFile`, not the control-transfer path `HidD_SetOutputReport` uses. DS4Windows's own issue tracker has real, still-open bug reports of Bluetooth rumble destabilizing connections, and the project's own approach is closer to *detecting and recovering from* an occasional freeze with a watchdog thread than reliably preventing one. Separately, `hidapi`'s own project wiki documents real, adapter-specific packet-drop behavior for exactly this class of Bluetooth HID output write, on Realtek/Intel-lineage radios specifically — which is why a second, different-chipset adapter was specifically sourced and tested here, rather than assuming the first result generalized.

A further review focused specifically on implementation pitfalls (event/`OVERLAPPED` object lifecycle bugs, device-removal races, and similar Win32-level mistakes that could produce exactly this symptom even with otherwise-correct code) turned up real evidence that the underlying stall may be a Bluetooth **control-channel handshake or power-management issue at the protocol level**, below where any Win32 API choice can reach — meaning a more careful or more "correct" implementation of either API might not fix this at all.

### Why this stays disabled rather than shipped as "usually works"

Every documented combination — both APIs, both radios, output handle kept fully separate from input as required — was tried on real hardware, and every single one either silently no-opped, destabilized the connection, or (per DS4Windows's own history) would require a watchdog-and-recover pattern rather than a real fix. Bluetooth input for both DS4 and DualSense is completely unaffected by any of this and remains fully reliable — this limitation is scoped specifically to the output direction.

**Current state, stated precisely:** the code path that would enable Bluetooth PS-controller output is never reached in the shipped build — the flag that marks an output handle as Bluetooth-backed is never set to true anywhere in the source, and the output handle itself stays uninitialized for any Bluetooth-connected controller. This is a deliberate design decision, not a disabled-but-almost-working feature.

**If you want to pick this up:** the most promising untried direction, based on the research above, is switching the Bluetooth input read loop to asynchronous (`OVERLAPPED`) I/O so a write can be issued without contending with a blocking read on the same handle — this specific combination was identified as worth trying but was not actually attempted or tested. Given the protocol-level evidence above, treat it as a real experiment with a real chance of still not working, not a known fix waiting to be typed in.

## Anti-cheat

Real, checked findings — not speculation, and not exhaustive. Read this before pointing this tool at any specific competitive or anti-cheat-protected game.

- Kernel-level anti-cheat systems that actively scan for driver-level input tools have been confirmed, in community reports, to refuse to launch or to flag the exact class of driver this project depends on (ViGEmBus-based virtual controllers, and HidHide specifically). If a game you want to use this with has kernel-level anti-cheat, check that specific game's own community/support channels for known compatibility before assuming this is safe.
- A vJoy-only setup (no ViGEmBus virtual gamepad, no HidHide device hiding — i.e. HOTAS mode is used, but Normal mode/device-hiding never is) appears to carry meaningfully lower risk than the hiding/virtual-gamepad pieces — vJoy is a much older, foundational tool across the flight-sim and sim-racing community with no confirmed blocking reports found anywhere. The confirmed risk is specifically in the hiding (HidHide) and fake-gamepad (ViGEmBus) pieces.
- This is exactly why a full `--uninstall` is built in: an anti-cheat driver-enumeration scan can in principle see an installed-but-unused driver even if this tool simply isn't running, so "just don't run it" isn't a sufficient mitigation before playing an anti-cheat-protected game you're concerned about.
