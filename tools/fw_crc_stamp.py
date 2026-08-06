#!/usr/bin/env python3
"""Post-build stamp of the firmware-image CRC-32 (W2-1 / hardening.md §5).

The linker emits a 4-byte placeholder (BOOT_CRC_UNSTAMPED_VALUE = 0xFFFFFFFF)
in the .fw_crc section as the LAST loaded section of the image. The CRC-32
(IEEE 802.3, reflected, poly 0xEDB88320) is computed over the whole image
EXCLUDING those final 4 bytes and patched in-place into the .bin / .hex.

This makes boot_crc_verify() (App/obsw/boot_crc.c) able to detect a corrupted
Flash image (SEU / incomplete uplink) before the OBSW trusts it.

Usage:
    python3 tools/fw_crc_stamp.py build/JOS.bin
    python3 tools/fw_crc_stamp.py build/JOS.hex   # intel hex supported

Returns non-zero if the placeholder is not found (already stamped / wrong file).
"""
import sys
import zlib

UNSTAMPED = 0xFFFFFFFF
PLACEHOLDER = UNSTAMPED.to_bytes(4, "little")


def stamp_bin(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 4:
        print(f"{path}: file too small", file=sys.stderr)
        return 2
    tail = bytes(data[-4:])
    if tail != PLACEHOLDER:
        print(f"{path}: tail is {tail.hex()} not the unstamped placeholder; "
              f"already stamped or wrong image", file=sys.stderr)
        return 1
    region = bytes(data[:-4])
    crc = zlib.crc32(region) & 0xFFFFFFFF
    data[-4:] = crc.to_bytes(4, "little")
    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: stamped CRC32 = 0x{crc:08X} over {len(region)} bytes")
    return 0


def stamp_hex(path: str) -> int:
    """Intel HEX: locate the last 4 data bytes (the .fw_crc placeholder) and
    replace them with the CRC of every preceding byte in the file. A full HEX
    parser is overkill for the one section we own; we find the placeholder
    record by its content."""
    with open(path, "r") as f:
        lines = f.read().splitlines()
    # Concatenate all data payloads in file order; the final 4 payload bytes
    # are the .fw_crc placeholder (unstamped = FF FF FF FF).
    payload = bytearray()
    tail_records = []
    for ln in lines:
        if not ln.startswith(":"):
            continue
        count = int(ln[1:3], 16)
        rectype = int(ln[7:9], 16)
        body = bytes.fromhex(ln[9:9 + count * 2])
        if rectype == 0x00:
            tail_records.append((ln, body))
            payload.extend(body)
    if len(payload) < 4:
        print(f"{path}: no data records", file=sys.stderr)
        return 2
    if bytes(payload[-4:]) != PLACEHOLDER:
        print(f"{path}: last 4 data bytes are not the unstamped placeholder",
              file=sys.stderr)
        return 1
    region = bytes(payload[:-4])
    crc = zlib.crc32(region) & 0xFFFFFFFF
    crc_bytes = crc.to_bytes(4, "little")
    # Replace the last data record's trailing 4 bytes with the CRC.
    last_ln, last_body = tail_records[-1]
    new_body = bytes(last_body[:-4]) + crc_bytes
    # Recompute the checksum byte (two's complement of the sum of all bytes
    # from byte count through the data, modulo 256).
    body_hex = new_body.hex().upper()
    rec = f":{len(new_body):02X}{last_ln[3:7]}{last_ln[7:9]}{body_hex}"
    csum = (-sum(int(rec[i:i+2], 16) for i in range(1, len(rec), 2))) & 0xFF
    rec += f"{csum:02X}"
    lines[lines.index(last_ln)] = rec
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{path}: stamped CRC32 = 0x{crc:08X} over {len(region)} bytes")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    if path.lower().endswith(".hex"):
        return stamp_hex(path)
    return stamp_bin(path)


if __name__ == "__main__":
    sys.exit(main())
