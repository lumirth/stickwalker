# Third-party licenses

Stickwalker uses the [MIT license](../LICENSE). Included dependencies and assets keep their own licenses.

The compiler archive reader follows [ISx](https://github.com/lifenjoiner/ISx) and [Unshield](https://github.com/twogood/unshield). Their notices follow.

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

## Other material

- The [Renesas HEW register header](../include/startup/iodefine.h) retains its vendor identification and is outside the project's MIT license.
- [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX) use MIT. Their notices are in [stick/licenses](../stick/licenses/).
- [esp32sim](https://github.com/mikroverk/esp32sim) uses MIT. Its [notice](../stick/simulation/ESP32SIM-LICENSE) accompanies the simulator patch. Espressif's mask ROM uses Apache-2.0.
- The Arduino core uses LGPL-2.1-or-later. Runtime downloads include dependency sources, notices and a [relinking recipe](../stick/docs/relink.md). [runtime-sources.json](../stick/runtime-sources.json) records the dependency versions and origins.
- The [placeholder artwork](../assets/placeholders/LICENSE.md) uses CC0. Earlier reconstruction releases retain their original CC0 terms.
- Wibo, uv, Ninja, Pillow, Ruff, clang-format and Cppcheck keep their upstream licenses.
- The graphics converter follows format descriptions from [NitroPaint](https://github.com/Garhoogin/NitroPaint) and [Tinke](https://github.com/pleonex/tinke).
