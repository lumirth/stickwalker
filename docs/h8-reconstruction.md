# The H8 firmware reconstruction

The application in `src/` and `include/` reconstructs the Pokéwalker's firmware.
With retail artwork and a qualified Renesas compiler suite, it builds a binary
identical to the reference image with SHA-256:

```text
f9e210a3b74afbbd12c5a66a51cc05cb9fbac986805ff0a3bfb4be6074d15607
```

This identity applies to that image; it does not establish that every regional
or production variant uses the same firmware.

## Build the original target

Provide a Renesas H8/H8S/H8SX compiler installation. Suites 6.02.01 and 6.02.02
are qualified. The compiler's 32-bit Windows executables run natively on Windows
x86_64 and through Wibo/Rosetta on macOS arm64.

Follow the [H8 build guide](build.md) to import the compiler, configure and
build. Placeholder artwork is sufficient for studying or modifying the program.
To reproduce the reference hash, [extract the resident artwork](../assets/README.md)
from your own firmware image and run `uv run ninja verify` under each qualified
suite. The repository does not distribute the firmware or compiler.

For an M5StickS3 build, use the [Stickwalker build guide](../stick/docs/build.md).
It does not use the H8 compiler or require a matching H8 binary.

## Read and contribute

The [source guide](source.md) follows boot, scheduling, motion, storage and
infrared across modules. File prefixes preserve the observed link order;
renaming a file can alter binary placement. [Build configuration](../config/README.md)
describes the associated flags and sections.

Follow [Contributing](../CONTRIBUTING.md) when changing either target. See the
[references and acknowledgments](references.md) for research sources and
community projects.
