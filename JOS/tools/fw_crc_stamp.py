#!/usr/bin/env python3
"""Stamp (or check) the firmware image CRC32 of a built RedPill/JOS binary.

The firmware computes a software CRC-32 (IEEE 802.3, identical to
``zlib.crc32``) over ``[__fw_image_start, __fw_crc_start)`` at boot and
compares it with the 32-bit word stored in the ``.fw_crc`` linker section
(see ``App/obsw/boot_crc.c`` and ``STM32L496VGTX_FLASH.ld``).

``.fw_crc`` is the last loaded section of the image, so the stored word is
always the final 4 bytes of the ``.bin``. This tool recomputes the CRC over
everything before it and writes it back, little-endian. It is idempotent:
re-running on an already stamped binary produces the same result.

Sentinels (must match ``App/obsw/boot_crc.h``):

* ``0x00000000`` — deliberate "not stamped yet" placeholder emitted by the
  compiler. Deliberately *not* the erased-Flash pattern, so a CRC word that
  has been erased/corrupted cannot downgrade a bad image to "nothing to
  check".
* ``0xFFFFFFFF`` — erased Flash. Never a valid stamp; the firmware treats it
  as an integrity fault.

Usage (from the JOS/ directory):

    make all                # builds and stamps (all depends on crc-stamp)
    make crc-stamp          # runs this script with the linker-derived address
    make crc-check          # read-only CI gate, no write

    python3 tools/fw_crc_stamp.py build/JOS.bin \\
        --hex build/JOS.hex --crc-addr 0x0801ABCD --verify
    python3 tools/fw_crc_stamp.py --check build/JOS.bin

Requires only Python 3 (no toolchain, no third-party modules).
"""

from __future__ import annotations

import argparse
import sys
import zlib

CRC_WORD_SIZE = 4
UNSTAMPED = 0x00000000     # BOOT_CRC_UNSTAMPED_VALUE
ERASED = 0xFFFFFFFF        # BOOT_CRC_ERASED_VALUE
DEFAULT_BASE = 0x08000000

# A stamp equal to a sentinel would be indistinguishable from "unstamped" or
# "erased" on the flight side. The odds are 2^-31, but the failure mode is a
# silently disabled integrity check, so it is a hard error, not a warning.
RESERVED_CRCS = (UNSTAMPED, ERASED)


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


def _load(path: str) -> bytearray | None:
    with open(path, "rb") as handle:
        image = bytearray(handle.read())

    if len(image) <= CRC_WORD_SIZE:
        print(f"error: {path} is too small to contain a CRC word", file=sys.stderr)
        return None
    if len(image) % CRC_WORD_SIZE != 0:
        print(f"error: {path} length {len(image)} is not 4-byte aligned; "
              "the .fw_crc section is not word-aligned at the end of the image",
              file=sys.stderr)
        return None
    return image


def _state(value: int) -> str:
    if value == UNSTAMPED:
        return "unstamped (0x00000000)"
    if value == ERASED:
        return "erased (0xFFFFFFFF)"
    return f"0x{value:08X}"


def check(path: str, base: int) -> int:
    """Read-only gate: the artefact must carry a real, matching stamp."""
    image = _load(path)
    if image is None:
        return 1

    crc_offset = len(image) - CRC_WORD_SIZE
    stored = int.from_bytes(image[crc_offset:], "little")
    expected = zlib.crc32(bytes(image[:crc_offset])) & 0xFFFFFFFF

    print(f"fw_crc_stamp: checking {path}")
    print(f"  region     : 0x{base:08X}..0x{base + crc_offset:08X} "
          f"({crc_offset} bytes)")
    print(f"  stored CRC : {_state(stored)}")
    print(f"  computed   : 0x{expected:08X}")

    if stored in RESERVED_CRCS:
        print(f"error: {path} is not stamped ({_state(stored)}). The boot-time "
              "integrity check would be a no-op on this artefact. Run "
              "`make crc-stamp`.", file=sys.stderr)
        return 1
    if stored != expected:
        print(f"error: {path} stored CRC 0x{stored:08X} != computed "
              f"0x{expected:08X}; the image is corrupted or was stamped before "
              "its last modification.", file=sys.stderr)
        return 1

    print("  check      : OK (stamped and consistent)")
    return 0


def stamp(path: str, base: int, crc_addr: int | None, hex_path: str | None,
          verify: bool) -> int:
    image = _load(path)
    if image is None:
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
    if crc in RESERVED_CRCS:
        print(f"error: computed CRC 0x{crc:08X} collides with a reserved "
              "sentinel value; the flight check could not distinguish it from "
              "an unstamped/erased image. Rebuild (e.g. touch a source file) "
              "to change the image.", file=sys.stderr)
        return 1

    image[crc_offset:] = crc.to_bytes(CRC_WORD_SIZE, "little")

    with open(path, "wb") as handle:
        handle.write(image)

    print(f"fw_crc_stamp: {path}")
    print(f"  region     : 0x{base:08X}..0x{base + crc_offset:08X} "
          f"({crc_offset} bytes)")
    print(f"  stored CRC : 0x{crc:08X} (was {_state(previous)})")

    if hex_path:
        with open(hex_path, "w", encoding="ascii") as handle:
            handle.write(build_intel_hex(bytes(image), base))
        print(f"  hex        : {hex_path} regenerated from the stamped image")

    if verify:
        with open(path, "rb") as handle:
            recheck = handle.read()
        recomputed = zlib.crc32(recheck[:crc_offset]) & 0xFFFFFFFF
        stored = int.from_bytes(recheck[crc_offset:], "little")
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
    parser.add_argument("--check", action="store_true",
                        help="read-only: fail if the image is unstamped, erased or "
                             "inconsistent; never writes")
    args = parser.parse_args(argv)

    if args.check:
        return check(args.binary, args.base)
    return stamp(args.binary, args.base, args.crc_addr, args.hex_path, args.verify)


if __name__ == "__main__":
    sys.exit(main())
