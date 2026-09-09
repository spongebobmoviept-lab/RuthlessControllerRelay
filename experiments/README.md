# Experiments — real, failed attempts, kept for reference

Everything in this folder is **known broken. Do not use it.** These are not toy examples — they're real, unmodified snapshots of this project's own source tree from the exact moment a specific experiment was tried and failed on real hardware, kept here so nobody has to re-derive or re-attempt the same dead end from scratch.

Each file has its own header explaining exactly what it tried, what happened when tested on real hardware, and why it was reverted. None of them build cleanly against the current `third_party/` headers without adjustment — they're historical snapshots, not maintained alternate versions.

| File | What it tried | What happened |
|---|---|---|
| [`bluetooth-ps-output-attempt-2026-09-09.c`](bluetooth-ps-output-attempt-2026-09-09.c) | Re-enabling Bluetooth PlayStation-controller rumble/LED output via `WriteFile()` on the same handle already used for continuous input reads | Destabilized the Bluetooth connection within ~10 seconds on two different Bluetooth adapters from two different chipset vendors — required physically power-cycling the controller to recover, since a plain code revert didn't undo the bad link state |

For the full story — including two research passes and why Bluetooth PS-controller output is a genuine, still-unsolved, community-wide problem rather than something specific to this project — see [docs/KNOWN_LIMITATIONS.md § Bluetooth PlayStation-controller rumble and LED output](../docs/KNOWN_LIMITATIONS.md#bluetooth-playstation-controller-rumble-and-led-output) and [docs/DEVELOPMENT_JOURNEY.md](../docs/DEVELOPMENT_JOURNEY.md).
