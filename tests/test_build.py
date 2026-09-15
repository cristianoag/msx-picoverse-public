import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import build
import ihx2bin
import release


class NativeBuildTests(unittest.TestCase):
    def test_binary_and_rebuilt_payloads_override_bundled_header(self):
        with tempfile.TemporaryDirectory(prefix="picoverse-build-test-") as temp:
            root = Path(temp).resolve()
            source = root / "project/tool/src"
            source.mkdir(parents=True)
            original = build.embedded_header("payload", b"\x11")
            (source / "payload.h").write_bytes(original)
            (source / "probe.c").write_text('#include <stdio.h>\n#include <payload.h>\nint main(void) { printf("%u", payload[0]); }\n')
            binary = root / "project/pico/probe/dist/probe.bin"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"\x22")
            spec = {"probe": {"directory": "project/tool", "program": "probe", "version": "test",
                              "assets": {"payload.h": "../pico/probe/dist/probe.bin"}}}
            with patch.object(build, "ROOT", root), patch.object(build, "MANIFEST", spec), contextlib.redirect_stdout(io.StringIO()):
                exe = build.build_tool("probe")
                self.assertEqual(subprocess.check_output([exe]), b"34")
                self.assertEqual((source / "payload.h").read_bytes(), original)
                timestamp = exe.stat().st_mtime_ns
                build.build_tool("probe")
                self.assertEqual(timestamp, exe.stat().st_mtime_ns)
                binary.write_bytes(b"\x33")
                build.build_tool("probe")
                self.assertEqual(subprocess.check_output([exe]), b"51")
                rebuilt = root / "project/pico/probe/build/probe.bin"
                rebuilt.parent.mkdir()
                rebuilt.write_bytes(b"\x44")
                build.build_tool("probe")
                self.assertEqual(subprocess.check_output([exe]), b"68")
                rebuilt.unlink()
                binary.unlink()
                build.build_tool("probe")
                self.assertEqual(subprocess.check_output([exe]), b"17")
                info = json.loads((source.parent / "build/inputs.json").read_text())
                self.assertEqual(info["payloads"][0]["kind"], "bundled-header")
                user_rom = source.parent / "my-game.rom"
                user_uf2 = source.parent / "my-game.uf2"
                user_rom.write_bytes(b"keep")
                user_uf2.write_bytes(b"keep")
                build.clean_tool("probe")
                self.assertFalse(exe.exists())
                self.assertEqual(user_rom.read_bytes(), b"keep")
                self.assertEqual(user_uf2.read_bytes(), b"keep")
                self.assertEqual((source / "payload.h").read_bytes(), original)


def uf2_block(number=0, count=1, address=0x10000000, family=0xE48BFF56):
    block = bytearray(512)
    struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                     address, 256, number, count, family)
    struct.pack_into("<I", block, 508, 0x0AB16F30)
    return bytes(block)


class UF2Tests(unittest.TestCase):
    def test_valid_and_truncated_image(self):
        data = uf2_block()
        self.assertEqual(release.validate_uf2(data, "2040"), 1)
        for broken in (b"", data[:-1], b"xxxx" + data[4:]):
            with self.assertRaises(ValueError):
                release.validate_uf2(broken, "2040")

    def test_wrong_family_and_block_count(self):
        with self.assertRaises(ValueError):
            release.validate_uf2(uf2_block(), "2350")
        with self.assertRaises(ValueError):
            release.validate_uf2(uf2_block(count=2), "2040")

    def test_overlapping_flash_blocks(self):
        with self.assertRaises(ValueError):
            release.validate_uf2(uf2_block(count=2) + uf2_block(number=1, count=2), "2040")

    def test_actual_midipac_uf2_contains_the_selected_payload(self):
        exe = build.executable("2040-loadrom")
        if not exe.exists():
            self.skipTest("Run make tools first for the integration test")
        header = exe.parent / "build/generated/midipac_fw.h"
        _, expected = build.array_bytes(header.read_text())
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "midipac.uf2"
            subprocess.run([exe, "-p", "-o", output], cwd=temp, check=True, capture_output=True)
            data = output.read_bytes()
        release.validate_uf2(data, "2040")
        actual = b"".join(data[offset + 32:offset + 32 + struct.unpack_from("<I", data, offset + 16)[0]]
                          for offset in range(0, len(data), 512))
        self.assertEqual(actual[:len(expected)], expected)


def hex_record(address, kind, data=b""):
    record = bytes([len(data)]) + address.to_bytes(2, "big") + bytes([kind]) + data
    return ":" + (record + bytes([-sum(record) & 0xFF])).hex().upper()


class IntelHexTests(unittest.TestCase):
    def test_padding_and_rom_address(self):
        text = hex_record(0x4000, 0, b"AB") + "\n" + hex_record(0, 1)
        rom = ihx2bin.convert(text)
        self.assertEqual(len(rom), 32768)
        self.assertEqual(rom[:3], b"AB\xff")
        self.assertEqual(rom[-1], 0xFF)

    def test_invalid_checksum_and_out_of_range(self):
        with self.assertRaises(ValueError):
            ihx2bin.convert(":02400000414200\n:00000001FF")
        with self.assertRaises(ValueError):
            ihx2bin.convert(hex_record(0xC000, 0, b"AB") + "\n" + hex_record(0, 1))


if __name__ == "__main__":
    unittest.main()
