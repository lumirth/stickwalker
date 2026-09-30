# Third-party material and tools

The [references](references.md) describe the research and tools that inform this project.

The compiler archive reader in `tools/installshield.py` uses the InstallShield format and
block-decoding approach documented by [ISx](https://github.com/lifenjoiner/ISx)
and [Unshield](https://github.com/twogood/unshield). Their MIT notices follow.

[Wibo](https://github.com/decompals/wibo) is downloaded separately from its
upstream release, or supplied locally. uv, Ninja, Pillow, Ruff, clang-format and Cppcheck retain their own licenses.
The Python dependencies are recorded in `uv.lock`.

The NCG/NCL converter follows public format descriptions in
[NitroPaint](https://github.com/Garhoogin/NitroPaint) and
[Tinke](https://github.com/pleonex/tinke). The [artwork guide](../assets/README.md)
links the consulted revisions and explains the reconstructed editor metadata.

## Renesas HEW device header

[`include/startup/iodefine.h`](../include/startup/iodefine.h) is the retained
Renesas HEW device-register template. It identifies itself as **H8/38602 Series
Include File, Ver 2.1**, with the marker **HEW_2006.10.05**. It provides the
memory-mapped register declarations used by the firmware.

This vendor template is outside the project's MIT license. Its existing
identification is retained in the file, and automated formatting and line-ending
conversion exclude it. The retained file contains no standalone license grant;
the project adds no license grant over Renesas's material.

## ISx

MIT License

Copyright (c) 2017 lifenjoiner

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

## Unshield

Copyright (c) 2003 David Eriksson <twogood@users.sourceforge.net>

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Stickwalker runtime

The Stick build uses M5Stack's ESP32 board package **3.3.9**,
[M5Unified 0.2.21](https://github.com/m5stack/M5Unified/tree/0.2.21) and
[M5GFX 0.2.29](https://github.com/m5stack/M5GFX/tree/0.2.29).
The two M5 libraries use MIT; their full notices are retained in
[`stick/licenses/`](../stick/licenses/). The Arduino ESP32 core contains
LGPL-2.1-or-later material, and its underlying ESP-IDF, precompiled libraries
and components retain their own terms. These components retain their own licenses alongside Stickwalker's MIT license.

Each firmware release includes a runtime archive with the exact application
objects, Arduino core archive, linked SDK libraries, linker scripts, dependency
sources and their original notices. Its relinking recipe reproduces the release
ELF and supports replacing the Arduino core. See the [relinking guide](../stick/docs/relink.md).

The runtime uses ESP-IDF **v5.5.4** at
`735507283d5b2f9fb363a1901172dbd9e847945d`, including its pinned submodules, plus
LittleFS **1.22.1** and ESP Diagnostics **1.2.1**. The source inventory in
[`stick/runtime-sources.json`](../stick/runtime-sources.json) records the archive
origins, hashes and submodule revisions. The SDK's linked Espressif radio
libraries retain their included binary-library licenses. GCC runtime and Newlib
notices are included with the firmware and runtime archives.

Earlier versions of the reconstruction remain under their original CC0 terms.
The [placeholder artwork](../assets/placeholders/LICENSE.md) also retains its
explicit CC0 dedication.
