#!/usr/bin/env python3
"""Convert SDCC Intel HEX to a fixed-size, 0xff-padded cartridge ROM."""
import argparse
from pathlib import Path


def convert(text, start=0x4000, size=0x8000):
    output = bytearray([0xFF]) * size
    base = 0
    eof = False
    written = set()
    for line in text.splitlines():
        if not line.strip():
            continue
        if eof or not line.startswith(":"):
            raise ValueError("Invalid Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) % 256:
            raise ValueError("Bad Intel HEX length or checksum")
        address = int.from_bytes(record[1:3], "big")
        kind = record[3]
        data = record[4:-1]
        if kind == 0:
            offset = base + address - start
            if offset < 0 or offset + len(data) > size:
                raise ValueError("HEX data exceeds the cartridge ROM address range")
            for index, value in enumerate(data, offset):
                if index in written and output[index] != value:
                    raise ValueError("Conflicting HEX data")
                output[index] = value
                written.add(index)
        elif kind == 1 and not data:
            eof = True
        elif kind in (2, 4) and len(data) == 2:
            base = int.from_bytes(data, "big") << (4 if kind == 2 else 16)
        elif kind not in (3, 5):
            raise ValueError(f"Unsupported Intel HEX record: {kind}")
    if not eof or not written:
        raise ValueError("Missing HEX end record or ROM data")
    return bytes(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(convert(args.input.read_text()))
