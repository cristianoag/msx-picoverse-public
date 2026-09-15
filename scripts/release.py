#!/usr/bin/env python3
"""Generate system UF2s and release archives; never download or bundle games."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from build import ROOT, MANIFEST, executable, write_if_changed

RECIPES = json.loads((ROOT / "scripts/firmware-manifest.json").read_text())
OUTPUT = ROOT / "build/release"
FAMILIES = {"2040": 0xE48BFF56, "2350": 0xE48BFF59}


def validate_uf2(data, family):
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    count = len(data) // 512
    previous_end = 0x10000000
    for index in range(count):
        block = data[index * 512:(index + 1) * 512]
        magic0, magic1, flags, address, size, number, total, chip = struct.unpack_from("<8I", block)
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError(f"Bad UF2 magic in block {index}")
        if not flags & 0x2000 or chip != FAMILIES[family]:
            raise ValueError(f"Wrong chip family in block {index}")
        if number != index or total != count or not 0 < size <= 476:
            raise ValueError(f"Invalid block numbering or size in block {index}")
        if not previous_end <= address < 0x11000000 or address + size > 0x11000000:
            raise ValueError(f"Invalid or overlapping flash address in block {index}")
        previous_end = address + size
    return count


def revision():
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def tool_inputs():
    return [json.loads((ROOT / spec["directory"] / "build/inputs.json").read_text())
            for spec in MANIFEST.values()]


def write_json(path, value):
    write_if_changed(path, (json.dumps(value, indent=2) + "\n").encode())


def checksums(directory):
    files = sorted(p for p in directory.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    contents = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(directory).as_posix()}\n"
                       for p in files)
    write_if_changed(directory / "SHA256SUMS", contents.encode())


def generate_firmware():
    destination = OUTPUT / "firmware"
    # Stage in a fresh directory so a previous build cannot leak ROMs or images
    # that were removed from the manifest into this release.
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="firmware-", dir=OUTPUT) as stage_name:
        stage = Path(stage_name)
        records = []
        seen = set()
        for recipe in RECIPES:
            family = recipe["tool"].split("-")[0]
            name = recipe["name"]
            if not re.fullmatch(r"[a-z0-9_.-]+", name) or (family, name) in seen:
                raise ValueError(f"Invalid or duplicate image name: {name}")
            seen.add((family, name))
            output = stage / family / (name + ".uf2")
            output.parent.mkdir(parents=True, exist_ok=True)
            command = [str(executable(recipe["tool"])), *recipe["args"], "-o", str(output)]
            # MultiROM and Explorer scan the working directory for ROMs. Keep
            # it empty and separate from the output and the user's ROM library.
            with tempfile.TemporaryDirectory(prefix="picoverse-rom-scan-") as empty:
                result = subprocess.run(command, cwd=empty, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"{name} failed:\n{result.stdout}\n{result.stderr}")
            data = output.read_bytes()
            blocks = validate_uf2(data, family)
            print(f"Validated {family}/{name}.uf2 ({blocks} blocks)", flush=True)
            records.append({**recipe, "file": f"{family}/{name}.uf2", "size": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})
        write_json(stage / "build-info.json", {
            "commit": revision(), "images": records, "tools": tool_inputs(),
            "payload_policy": "Bundled binaries/headers, or local rebuilds when present. See payload hashes.",
        })
        shutil.copy2(ROOT / "LICENSE.txt", stage / "LICENSE.txt")
        checksums(stage)
        if destination.exists():
            shutil.rmtree(destination)
        stage.rename(destination)
    return destination


def native_platform():
    system = {"Darwin": "macos", "Linux": "linux"}.get(platform.system(), platform.system().lower())
    arch = {"aarch64": "arm64", "AMD64": "x86_64"}.get(platform.machine(), platform.machine())
    return f"{system}-{arch}"


def package_tools():
    name = f"picoverse-tools-{native_platform()}"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    archive = OUTPUT / (name + ".tar.gz")
    with tempfile.TemporaryDirectory(prefix="tools-", dir=OUTPUT) as stage_name:
        stage = Path(stage_name)
        for project in MANIFEST:
            family = project.split("-")[0]
            target = stage / family / executable(project).name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(executable(project), target)
            target.chmod(0o755)
        shutil.copy2(ROOT / "LICENSE.txt", stage / "LICENSE.txt")
        shutil.copy2(ROOT / "docs/BUILDING.md", stage / "BUILDING.md")
        write_json(stage / "build-info.json", {"commit": revision(), "platform": native_platform(), "tools": tool_inputs()})
        checksums(stage)
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage, arcname=name)
    return archive


def package_firmware():
    source = generate_firmware()
    archive = OUTPUT / "picoverse-firmware.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zip_file:
        for path in sorted(source.rglob("*")):
            if path.is_file():
                zip_file.write(path, path.relative_to(source))
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["firmware", "package"])
    parser.add_argument("--only", choices=["all", "tools", "firmware"], default="all")
    args = parser.parse_args()
    if args.command == "firmware":
        generate_firmware()
        return
    archives = []
    if args.only in ("all", "tools"):
        archives.append(package_tools())
    if args.only in ("all", "firmware"):
        archives.append(package_firmware())
    for archive in archives:
        write_if_changed(Path(str(archive) + ".sha256"),
                         f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n".encode())
        print(archive.relative_to(ROOT))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"Packaging failed: {error}")
