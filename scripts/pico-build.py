#!/usr/bin/env python3
"""Optional source build of one Pico component, followed by its native tool."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

from build import ROOT, build_tool

TARGETS = {
    **{f"2040-{name}": (f"2040/software/loadrom.pio/pico/{name}", "2040-loadrom")
       for name in ("loadrom", "keyboard", "midi", "midipac", "joystick")},
    "2040-multirom": ("2040/software/multirom.pio/pico/multirom", "2040-multirom"),
    "2350-loadrom": ("2350/software/loadrom.pio/pico/loadrom", "2350-loadrom"),
    "2350-multirom": ("2350/software/multirom.pio/pico/multirom", "2350-multirom"),
    "2350-explorer": ("2350/software/explorer.pio/pico/explorer", "2350-explorer"),
    "2350-yamanooto": ("2350/software/yamanooto/pico/yamanooto", "2350-yamanooto"),
}


def dependency(variable, name, repository, tag, submodule=None):
    override = os.environ.get(variable)
    path = Path(override).expanduser().resolve() if override else ROOT / ".deps" / name
    if override and not (path / "CMakeLists.txt").is_file():
        raise ValueError(f"{variable} does not point to a valid checkout: {path}")
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", tag, repository, str(path)], check=True)
    if submodule and not (path / submodule / "src").is_dir():
        subprocess.run(["git", "-C", str(path), "submodule", "update", "--init", "--depth", "1", submodule], check=True)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=sorted(TARGETS))
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    relative, tool = TARGETS[args.target]
    source = ROOT / relative
    configure = []
    if args.target in ("2350-loadrom", "2350-multirom", "2350-explorer"):
        for variable, subdir in [("PICO_FATFS_PATH", "no-OS-FatFS-SD-SDIO-SPI-RPi-Pico")]:
            path = Path(os.environ.get(variable, str(source / "lib" / subdir))).resolve()
            if not (path / "src/CMakeLists.txt").is_file():
                raise ValueError(f"Missing {subdir}. Set {variable} to its checkout; see docs/BUILDING.md. "
                                 "Use 'make tools' to build with the bundled payloads instead.")
            configure.append(f"-D{variable}={path}")
    if args.target == "2350-explorer":
        path = Path(os.environ.get("PICOMP3LIB_PATH", str(source / "lib/picomp3lib"))).resolve()
        if not (path / "src/CMakeLists.txt").is_file():
            raise ValueError("Missing picomp3lib; set PICOMP3LIB_PATH. See docs/BUILDING.md.")
        configure.append(f"-DPICOMP3LIB_PATH={path}")

    sdk = dependency("PICO_SDK_PATH", "pico-sdk", "https://github.com/raspberrypi/pico-sdk.git", "2.2.0", "lib/tinyusb")
    configure += [f"-DPICO_SDK_PATH={sdk}", "-DCMAKE_BUILD_TYPE=Release", "-DPICOVERSE_COPY_TO_DIST=OFF"]
    if (source / "pico_extras_import.cmake").exists():
        extras = dependency("PICO_EXTRAS_PATH", "pico-extras", "https://github.com/raspberrypi/pico-extras.git", "sdk-2.1.1")
        configure.append(f"-DPICO_EXTRAS_PATH={extras}")
    if "PICO_TOOLCHAIN_PATH" in os.environ:
        configure.append(f"-DPICO_TOOLCHAIN_PATH={os.environ['PICO_TOOLCHAIN_PATH']}")
    if args.target == "2350-explorer":
        from build import MANIFEST
        configure.append(f"-DEXPLORER_VERSION={MANIFEST[tool]['version']}")
    configure.extend(shlex.split(os.environ.get("PICO_CMAKE_ARGS", "")))
    cmake = os.environ.get("CMAKE", "cmake")
    build = source / "build"
    subprocess.run([cmake, "-S", str(source), "-B", str(build), *configure], check=True)
    subprocess.run([cmake, "--build", str(build), "--parallel", str(args.jobs)], check=True)
    build_tool(tool)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"Pico build failed: {error}")
