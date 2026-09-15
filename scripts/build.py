#!/usr/bin/env python3
"""Build native UF2 tools using only Python 3 and a C compiler.

Prefer a component's binary when available (including locally rebuilt binaries).
Otherwise use its checked-in C array. All generated headers stay under build/;
the vendored headers in src/ are never overwritten or removed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "scripts/build-manifest.json").read_text())
EXE = ".exe" if os.name == "nt" else ""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def array_bytes(header):
    """Read the single byte array used by xxd-generated firmware headers."""
    match = re.search(r"unsigned char\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}", header, re.S)
    if not match:
        raise ValueError("Not a firmware byte-array header")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match[2], flags=re.S)
    tokens = [token.strip() for token in body.split(",") if token.strip()]
    return match[1], bytes(int(token, 0) for token in tokens)


def embedded_header(symbol, payload):
    lines = [f"static const unsigned char {symbol}[] = {{"]
    for start in range(0, len(payload), 16):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in payload[start:start + 16]) + ",")
    lines += ["};", f"static const unsigned int {symbol}_len = {len(payload)};", ""]
    return "\n".join(lines).encode()


def executable(project):
    spec = MANIFEST[project]
    return ROOT / spec["directory"] / (spec["program"] + EXE)


def build_tool(project, version=None):
    spec = MANIFEST[project]
    directory = ROOT / spec["directory"]
    generated = directory / "build/generated"
    inputs = []
    for name, relative_input in spec["assets"].items():
        fallback = directory / "src" / name
        header = fallback.read_text()
        symbol = re.search(r"unsigned char\s+(\w+)\s*\[", header)[1]
        binary = (directory / relative_input).resolve()
        # Optional Pico source builds stay in build/ and take precedence over
        # the distributed binary, without modifying tracked dist/ files.
        if binary.suffix.lower() in (".bin", ".rom"):
            component = binary.parent.parent if binary.parent.name == "dist" else binary.parent
            rebuilt = component / "build" / binary.name
            if rebuilt.is_file():
                binary = rebuilt
        if binary.is_file():
            source = binary
            payload = binary.read_bytes()
            content = embedded_header(symbol, payload)
            kind = "binary"
        else:
            source = fallback
            _, payload = array_bytes(header)
            content = header.encode()
            kind = "bundled-header"
        if not payload:
            raise ValueError(f"Empty firmware payload: {source}")
        write_if_changed(generated / name, content)
        inputs.append({"header": name, "source": source.relative_to(ROOT).as_posix(),
                       "kind": kind, "size": len(payload), "sha256": sha256(payload)})

    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-O2"))
    cppflags = shlex.split(os.environ.get("CPPFLAGS", ""))
    ldflags = shlex.split(os.environ.get("LDFLAGS", ""))
    effective_version = version or spec["version"]
    output = executable(project)
    command = [*compiler, *cppflags, *flags, "-std=gnu11",
               f'-DAPP_VERSION="{effective_version}"', "-I", str(generated),
               "-I", str(directory / "src"), str(directory / "src" / (spec["program"] + ".c")),
               *ldflags, "-o", str(output)]
    dependencies = sorted((directory / "src").glob("*.h")) + sorted(generated.glob("*.h"))
    dependencies += [directory / "src" / (spec["program"] + ".c")]
    digest = hashlib.sha256(json.dumps(command).encode())
    digest.update(subprocess.check_output([*compiler, "--version"]))
    for path in dependencies:
        digest.update(path.read_bytes())
    fingerprint = digest.hexdigest().encode()
    stamp = directory / "build/native.sha256"
    if not output.exists() or not stamp.exists() or stamp.read_bytes() != fingerprint:
        print(f"Compiling {project} -> {output.relative_to(ROOT)}", flush=True)
        subprocess.run(command, check=True)
        write_if_changed(stamp, fingerprint)
    else:
        print(f"{project}: up to date", flush=True)
    compiler_version = subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0]
    metadata = {"project": project, "version": effective_version,
                "compiler": compiler_version, "payloads": inputs}
    write_if_changed(directory / "build/inputs.json", (json.dumps(metadata, indent=2) + "\n").encode())
    return output


def clean_tool(project):
    """Only remove outputs owned by this build, never ROMs or vendored headers."""
    directory = ROOT / MANIFEST[project]["directory"]
    for path in [executable(project), directory / "build/native.sha256", directory / "build/inputs.json"]:
        path.unlink(missing_ok=True)
    generated = directory / "build/generated"
    if generated.is_dir():
        shutil.rmtree(generated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["tools", "clean"])
    parser.add_argument("projects", nargs="*", help="Project IDs from scripts/build-manifest.json; default: all")
    parser.add_argument("--version", help="Override the native tool version, not the embedded firmware version")
    args = parser.parse_args()
    projects = args.projects or list(MANIFEST)
    unknown = set(projects) - MANIFEST.keys()
    if unknown:
        parser.error(f"Unknown projects: {', '.join(sorted(unknown))}")
    for project in projects:
        if args.command == "clean":
            clean_tool(project)
        else:
            build_tool(project, args.version)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"Build failed: {error}")
