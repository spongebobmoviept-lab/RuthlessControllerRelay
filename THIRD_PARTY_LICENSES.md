# Third-party licenses and credit

This project would not exist without four pieces of driver/library engineering built and maintained by other people. All four are verified as of 2026-09 to carry permissive licenses that explicitly allow redistribution, including in binary form, provided the copyright notice and license text travel with them. That's exactly what this file is for. If you redistribute this project, keep this file (or at minimum, the four notices below) intact.

This project is genuinely a small addition on top of the real work below — a thin layer of input relaying and UI glued on top of driver-level engineering that took each of these authors far more effort than anything in this repository. Please give them the credit.

---

## vJoy — the virtual joystick device

**Repository:** https://github.com/shauleiz/vJoy
**Author:** Shaul Eizikovich
**License:** MIT

vJoy is the virtual joystick driver this project's HOTAS mode is built entirely on top of — it's the device a flight-sim's own axis-binding screen actually points at. It's a long-standing, foundational tool across the whole flight-sim and sim-racing community, predating this project by many years.

```
MIT License

Copyright (c) 2017 Shaul Eizikovich

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## HidHide — the device-hiding filter driver

**Repository:** https://github.com/nefarius/HidHide
**Authors:** Eric Korff de Gidts, Benjamin Höglinger-Stelzer (Nefarius Software Solutions)
**License:** MIT

HidHide is what makes "the game only ever sees one controller at a time" actually possible — a kernel-mode filter driver that hides a real HID device from every application except the ones explicitly allow-listed. Everything in this project's architecture that depends on the real controller being invisible to a game depends directly on this driver.

```
MIT License

Copyright (c) 2020 Eric Korff de Gidts
Copyright (c) 2021-2024 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## ViGEmBus — the virtual gamepad bus driver

**Repository:** https://github.com/nefarius/ViGEmBus
**Author:** Benjamin Höglinger-Stelzer / Nefarius Software Solutions e.U.
**License:** BSD 3-Clause

ViGEmBus is the kernel-mode bus driver that lets a normal user-mode process create a genuine virtual Xbox 360 or DualShock 4 gamepad that Windows and games treat exactly like a real one. This project's "Normal mode" (as opposed to HOTAS mode) is built entirely on top of it.

```
BSD 3-Clause License

Copyright (c) 2016-2020, Nefarius Software Solutions e.U.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## ViGEmClient — the user-mode client library for ViGEmBus

**Repository:** https://github.com/nefarius/ViGEmClient
**Author:** Benjamin Höglinger-Stelzer
**License:** MIT

The C/C++ client library this project links against to talk to ViGEmBus — compiled from real, unmodified upstream source (`third_party/vigem_client/src/ViGEmClient.cpp`) into `ViGEmClient.dll`.

```
MIT License

Copyright (c) 2018 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Other sources consulted (no code redistributed, credit due anyway)

Two parts of this project's own code were only made correct by reading real, working reference implementations rather than guessing at undocumented hardware protocols:

- **[DS4Windows](https://github.com/Ryochan7/DS4Windows)** (and its predecessors) — the confirmed rumble/LED output-report byte layouts for both DualSense and DualShock 4, over both USB and Bluetooth (including the CRC32 scheme Bluetooth output reports require), were cross-checked against DS4Windows's real C# source rather than reverse-engineered from scratch. DS4Windows's own source and issue tracker were also the confirmation that Bluetooth PS-controller rumble/LED output is a genuinely unsolved, still-open problem community-wide — not something unique to this project. See [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
- **The Linux kernel's [`hid-playstation.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)** driver — an independent, cross-checked source for the same DualSense output-report field layout.
- **[psdevwiki.com](https://www.psdevwiki.com/ps4/DS4-BT)**'s community-maintained DS4 Bluetooth protocol documentation — the source for the Feature Report 0x05 "wake up the full report" step this project's Bluetooth DS4 support depends on.

No code from any of the above was copied into this project — they were used purely as reference material to confirm real hardware behavior instead of guessing at it, consistent with this project's own "get real confirmed data, don't guess a protocol" rule (see [docs/DEVELOPMENT_JOURNEY.md](docs/DEVELOPMENT_JOURNEY.md)).
