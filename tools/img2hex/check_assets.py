#!/usr/bin/env python3
"""Check current images, packed ROM files, C descriptors and the built firmware."""
import argparse
import contextlib
import io
import re
import struct
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

import pic2pram

ROOT = pic2pram.ROOT
PROJECT = ROOT / "project/GamingPlayUnit"
SOFTWARE = PROJECT / "embedded_sw/SoC_RISCV32/software/standalone/GamingPlayUnit"


def elf_asset_table(path):
    """Read g_assets from a RISC-V ELF32 file without third-party ELF packages."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32: " + str(path))
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    sections = [struct.unpack_from("<IIIIIIIIII", data, header[6] + i*header[11])
                for i in range(header[12])]

    def at_address(address, size):
        for section in sections:
            if section[1] != 8 and section[3] <= address and address+size <= section[3]+section[5]:
                offset = section[4]+address-section[3]
                return data[offset:offset+size]
        raise ValueError("ELF address has no file-backed section: " + hex(address))

    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4]+strings[5]]
        for offset in range(section[4], section[4]+section[5], section[9]):
            name, address, size, _, _, _ = struct.unpack_from("<IIIBBH", data, offset)
            symbol = names[name:].split(b"\0", 1)[0]
            if symbol != b"g_assets":
                continue
            if size % 16:
                raise ValueError("unexpected g_assets entry size")
            result = []
            for offset in range(0, size, 16):
                result.append(struct.unpack("<IIII", at_address(address+offset, 16)))
            return result
    raise ValueError("g_assets not found in " + str(path))


def check(elf, bitstream):
    stale = []
    # Repack the live source images into an isolated directory. Comparing these
    # bytes also catches replaced images that keep the same name and dimensions.
    with tempfile.TemporaryDirectory(prefix="gpu-assets-") as folder:
        temp = Path(folder)
        hex_dir, sw_dir, out_dir = temp/"hex", temp/"sw", temp/"output"
        sw_dir.mkdir()
        with patch.object(pic2pram, "OUT_DIR", out_dir), \
             patch.object(pic2pram, "EFX_HEX_DIRS", [temp/"unused", hex_dir]), \
             patch.object(pic2pram, "SW_G2D_DIRS", [temp/"unused_sw", sw_dir]), \
             patch.object(sys, "argv", ["pic2pram.py", "--target", "project"]), \
             contextlib.redirect_stdout(io.StringIO()):
            pic2pram.main()
        comparisons = [
            (hex_dir/"assets.hex", PROJECT/"hex/assets.hex"),
            (hex_dir/"palette.hex", PROJECT/"hex/palette.hex"),
            (out_dir/"assets_desc.h", SOFTWARE/"peripheral/APB0/gpu2d/assets_desc.h"),
            (out_dir/"assets_desc.h", ROOT/"tools/img2hex/output/assets_desc.h"),
        ]
        for expected, actual in comparisons:
            if not actual.is_file() or expected.read_bytes() != actual.read_bytes():
                stale.append(str(actual.relative_to(ROOT)))
        pattern = r'\{\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\s*\}'
        descriptors = [tuple(int(v) for v in values)
                       for values in re.findall(pattern, (out_dir/"assets_desc.h").read_text())]
    if elf is not None and elf_asset_table(elf) != descriptors:
        stale.append(str(elf) + " (compiled g_assets differs from current images)")
    if bitstream is not None:
        # This is an age check, not proof of which bitstream is on the board.
        if not bitstream.is_file() or bitstream.stat().st_mtime < max(
                (PROJECT/"hex/assets.hex").stat().st_mtime,
                (PROJECT/"hex/palette.hex").stat().st_mtime):
            stale.append(str(bitstream) + " (older than ROM initializers)")
    if stale:
        raise ValueError("asset outputs out of sync:\n  " + "\n  ".join(stale))
    print("PASS: %d assets; source images, ROM/palette and C headers agree%s%s" %
          (len(descriptors), "; ELF agrees" if elf else "",
           "; bitstream age OK" if bitstream else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=SOFTWARE/"build/GamingPlayUnit.elf")
    parser.add_argument("--bitstream", type=Path, default=PROJECT/"outflow/GamingPlayUnit.bit")
    parser.add_argument("--skip-bitstream", action="store_true")
    parser.add_argument("--sources-only", action="store_true",
                        help="check generated sources only; no firmware or bitstream required")
    args = parser.parse_args()
    try:
        check(None if args.sources_only else args.elf,
              None if args.skip_bitstream or args.sources_only else args.bitstream)
    except (OSError, ValueError) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
