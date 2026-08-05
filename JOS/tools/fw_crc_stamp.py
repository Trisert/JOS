#!/usr/bin/env python3
"""Stamp the firmware image CRC32 into a built RedPill/JOS binary.

The firmware computes a software CRC-32 (IEEE 802.3, identical to
``zlib.crc32``) over ``[__fw_image_start, __fw_crc_start)`` at boot and
compares it with the 32-bit word stored in the ``.fw_crc`` linker section
(see ``App/obsw/boot_crc.c`` and ``STM32L496VGTX_FLASH.ld``).

``.fw_crc`` is the last loaded section of the image, so the stored word is
always the final 4 bytes of the ``.bin``. This tool recomputes the CRC over
everything before it and writes it back, little-endian. It is idempotent:
re-running on an already stamped binary produces the same result.

Usage (from the JOS/ directory):

    make all
    make crc-stamp          # runs this script with the linker-derived address

    python3 tools/fw_crc_stamp.py build/JOS.bin \\
        --hex build/JOS.hex --crc-addr 0x0801ABCD --verify

Requires only Python 3 (no toolchain, no third-party modules).
"""

from __future__ import annotations

import argparse
import sys
import zlib

CRC_WORD_SIZE = 4
UNSTAMPED = 0xFFFFFFFF
DEFAULT_BASE = 0x08000000


def build_intel_hex(image: bytes, base_addr: int) -> str:
    """Serialise *image* (loaded at *base_addr*) as Intel HEX."""
    lines = []
    upper = None
    for offset in range(0, len(image), 16):
        addr = base_addr + offset
        hi = (addr >> 16) & 0xFFFF
        if hi != upper:
            payload = bytes((0x02, 0x00, 0x00, 0x04, (hi >> 8) & 0xFF, hi & 0xFF))
            lines.append(_hex_record(payload))
            upper = hi
        chunk = image[offset:offset + 16]
        payload = bytes((len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0x00)) + chunk
        lines.append(_hex_record(payload))
    lines.append(":00000001FF")
    return "\n".join(lines) + "\n"


def _hex_record(payload: bytes) -> str:
    checksum = (-sum(payload)) & 0xFF
    return ":" + payload.hex().upper() + f"{checksum:02X}"


def stamp(path: str, base: int, crc_addr: int | None, hex_path: str | None,
          verify: bool) -> int:
    with open(path, "rb") as handle:
        image = bytearray(handle.read())

    if len(image) <= CRC_WORD_SIZE:
        print(f"error: {path} is too small to contain a CRC word", file=sys.stderr)
        return 1
    if len(image) % CRC_WORD_SIZE != 0:
        print(f"error: {path} length {len(image)} is not 4-byte aligned; "
              "the .fw_crc section is not word-aligned at the end of the image",
              file=sys.stderr)
        return 1

    crc_offset = len(image) - CRC_WORD_SIZE
    if crc_addr is not None and base + crc_offset != crc_addr:
        print(f"error: linker places __fw_crc_start at 0x{crc_addr:08X} but the "
              f"last word of {path} is at 0x{base + crc_offset:08X}. The .fw_crc "
              "section must remain the last loaded section of the image.",
              file=sys.stderr)
        return 1

    previous = int.from_bytes(image[crc_offset:], "little")
    crc = zlib.crc32(bytes(image[:crc_offset])) & 0xFFFFFFFF
    image[crc_offset:] = crc.to_bytes(CRC_WORD_SIZE, "little")

    with open(path, "wb") as handle:
        handle.write(image)

    state = "unstamped" if previous == UNSTAMPED else f"0x{previous:08X}"
    print(f"fw_crc_stamp: {path}")
    print(f"  region     : 0x{base:08X}..0x{base + crc_offset:08X} "
          f"({crc_offset} bytes)")
    print(f"  stored CRC : 0x{crc:08X} (was {state})")

    if hex_path:
        with open(hex_path, "w", encoding="ascii") as handle:
            handle.write(build_intel_hex(bytes(image), base))
        print(f"  hex        : {hex_path} regenerated from the stamped image")

    if verify:
        with open(path, "rb") as handle:
            check = handle.read()
        recomputed = zlib.crc32(check[:crc_offset]) & 0xFFFFFFFF
        stored = int.from_bytes(check[crc_offset:], "little")
        if recomputed != stored:
            print("error: verification failed after stamping", file=sys.stderr)
            return 1
        print("  verify     : OK")

    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="firmware .bin produced by objcopy -O binary")
    parser.add_argument("--base", type=lambda v: int(v, 0), default=DEFAULT_BASE,
                        help="load address of the image (default 0x08000000)")
    parser.add_argument("--crc-addr", type=lambda v: int(v, 0), default=None,
                        help="expected __fw_crc_start address; checked against the "
                             "image layout to catch linker-script regressions")
    parser.add_argument("--hex", dest="hex_path", default=None,
                        help="also regenerate this Intel HEX file from the stamped image")
    parser.add_argument("--verify", action="store_true",
                        help="re-read and re-check the stamped image")
    args = parser.parse_args(argv)

    return stamp(args.binary, args.base, args.crc_addr, args.hex_path, args.verify)


if __name__ == "__main__":
    sys.exit(main())
