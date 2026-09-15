# Building PicoVerse on macOS and Linux

The default build compiles the six **native UF2 builder tools** from their C
sources. They embed the firmware payloads already shipped with this repository.
It does not cross-compile every Pico firmware or assemble every Z80 BIOS.
This is intentional: the public tree is missing the OPL4/MSX-AUDIO sources and
some external libraries used by the original author's build.

## Quick start

Requirements: Python **3.9 or newer**, a C compiler (Clang or GCC), and GNU Make.
There are no Python packages to install, no Windows executables to run, and no
`xxd` dependency. The native build works offline from a clean checkout.

macOS:

```sh
xcode-select --install
brew install python
```

Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install build-essential python3
```

From the repository root:

```sh
make tools
make firmware
```

The native tools are placed alongside each tool's Makefile:

| Family | Executables |
| --- | --- |
| 2040 | `2040/software/loadrom.pio/tool/loadrom`, `2040/software/multirom.pio/tool/multirom` |
| 2350 | `2350/software/loadrom.pio/tool/loadrom`, `2350/software/multirom.pio/tool/multirom`, `2350/software/explorer.pio/tool/explorer`, `2350/software/yamanooto/tool/yamanooto` |

On Windows with Python, GNU Make and a GCC-compatible compiler on `PATH`, native
tools use `.exe`. The release workflow currently tests and packages macOS and Linux.
Existing `.exe` binaries in `tool/dist` are legacy upstream builds; use the new
executables beside the tool Makefiles.

To build only selected tools, use the IDs from `scripts/build-manifest.json`:

```sh
make tools PROJECTS='2040-loadrom 2350-explorer'
make -C 2040/software/loadrom.pio/tool
```

`CC`, `CPPFLAGS`, `CFLAGS` and `LDFLAGS` can be supplied as environment variables.
For example: `CC=clang CFLAGS='-O0 -g' make tools`. Python can be selected with
`make PYTHON=python3.12`. Running `make` inside a LoadROM/MultiROM/Explorer/Yamanooto
project also builds its native tool using the bundled payloads.

## Generated firmware images

`make firmware` writes the system image variants listed in
`scripts/firmware-manifest.json` to `build/release/firmware/2040` and `2350`.
It includes Nextor, Nextor + WiFi, mapper/MegaRAM variants, Explorer with Nextor,
MIDI, MIDI-PAC, keyboard, joystick, OPL4 and MSX-AUDIO.

Images that previously included collections of commercial game ROMs are not
generated: those ROM inputs are not in this checkout. Yamanooto's native tool is
included, but its UF2 needs a user-supplied game image. Use the tools locally with
your own ROMs to produce those images. Explorer/MultiROM generation runs in an
empty temporary directory, so it cannot accidentally package ROMs from your
working directory.

The old top-level `firmware/` directory has been removed from the current tree;
its previous contents remain in Git history. Generated release outputs belong in
`build/release/`, which is ignored by Git.

## How payloads are selected

For each tool, `scripts/build-manifest.json` maps a generated C header to its
binary input. When the input is present, the build embeds it. An optional local
`build/<component>.bin` or `build/<component>.rom` takes precedence. When
neither binary is available, the build uses the checked-in firmware byte array
from `tool/src`. This fallback is recorded explicitly as `bundled-header`.

Generated headers go in `tool/build/generated/`, and the tool includes those
before `tool/src`. This avoids silently compiling an old checked-in header after
rebuilding a firmware binary. Every build records each selected payload's source,
size and SHA-256 in `tool/build/inputs.json`. Native builds never overwrite the
checked-in headers or firmware binaries. `make clean` removes only native build
outputs; it preserves payloads and user ROMs/UF2s.

The tool version and the embedded firmware version are separate. Overriding
`VERSION` changes the native tool's version string; it does not upgrade a bundled
firmware. A release tag identifies the repository commit and the included input
hashes, rather than claiming all components share the same version.

## Rebuilding a Pico component from source

This optional path requires CMake, Git and a complete `arm-none-eabi` compiler
installation, including Newlib (`nosys.specs`). On macOS:

```sh
brew install cmake
brew install --cask gcc-arm-embedded
```

On Debian/Ubuntu:

```sh
sudo apt-get install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

To rebuild MIDI-PAC after editing `midipac.h`:

```sh
make pico PICO_TARGET=2040-midipac
./2040/software/loadrom.pio/tool/loadrom -p -o midipac-test.uf2
```

The first command configures and builds MIDI-PAC, then recompiles LoadROM with
the new payload. No separate header-generation step or `clean` is needed.
The equivalent per-project command is:

```sh
make -C 2040/software/loadrom.pio midipac
```

The helper downloads Pico SDK **2.2.0** (and Pico Extras **sdk-2.1.1**, where used)
into `.deps/` on first use. To reuse existing installations, set `PICO_SDK_PATH`
and `PICO_EXTRAS_PATH`. Use `PICO_TOOLCHAIN_PATH` if your Arm compiler is not on
`PATH`, `CMAKE` to select CMake, `JOBS` to change parallelism, and `PICO_CMAKE_ARGS`
for extra configuration arguments. For example:

```sh
PICO_SDK_PATH=/path/to/pico-sdk make pico PICO_TARGET=2040-midipac JOBS=8
```

Available IDs are listed by `python3 scripts/pico-build.py --help`. The 2040
components and 2350 Yamanooto can build with the SDK/Extras. Source builds of
2350 LoadROM, MultiROM and Explorer additionally need the
`no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` library (`PICO_FATFS_PATH`); Explorer also needs
`picomp3lib` (`PICOMP3LIB_PATH`). Those external source trees are absent from the
public repository. Their versions/customizations are not specified upstream, so
the helper requires explicit local paths instead of silently substituting an
arbitrary library revision. OPL4 and MSX-AUDIO are bundled-payload-only here.

Pico builds stay in the component's `build/` directory. Direct legacy CMake builds
can still copy to `dist/`; the helper disables that via `PICOVERSE_COPY_TO_DIST`.

## Z80 source builds

The default native build reuses the distributed menu and WiFi ROMs. Optional
WiFi assembly uses `sjasmplus` from `PATH`, overridable with `SJASMPLUS`.
Optional MSX menu builds require SDCC and a compatible Fusion-C checkout supplied
through `FUSION_DIR`. They use the repository's Intel HEX converter instead of
a platform-specific `hex2bin` executable. See the component Makefiles for targets.
These source builds write to the component's `build/` directory; the next native
tool build picks up the new ROM. For example:

```sh
make -C 2350/software/wifi/bios
make tools PROJECTS='2350-loadrom 2350-multirom 2350-explorer'
```

## Releases and GitHub Actions

`make release` builds the native tools and produces:

- `picoverse-tools-<os>-<architecture>.tar.gz`, containing all six tools;
- `picoverse-firmware.zip`, containing the system UF2s, separated by chip family;
- SHA-256 checksum files, input manifests and the project license.

The GitHub Actions workflow runs for PRs, pushes to `main`/`codex/**`, manual runs,
and pushed tags. It builds/tests on Linux x86-64, macOS Apple Silicon and macOS
Intel. Every platform generates and structurally validates all UF2 recipes;
the Linux job packages the shared firmware download. Artifacts from ordinary
runs are retained for 14 days.

After these changes have been pushed to the fork, publish a version with:

```sh
git tag v0.1.0
git push origin v0.1.0
```

Use your own tag name. Only a **tag push** publishes a GitHub Release, and only
after every matrix build passes. The release job uses the repository's automatic
`GITHUB_TOKEN` with `contents: write`; no personal token is required. If the fork's
Actions are disabled, enable them in its Actions tab first. Merely creating a tag
locally does not trigger GitHub Actions. Re-running a tag workflow updates that
release's assets; PRs and manual runs never publish a release.

These checks verify builds, payload inclusion and UF2 structure. They do not
replace testing the firmware on real MSX/PicoVerse hardware.
