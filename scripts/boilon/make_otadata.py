#!/usr/bin/env python3
"""Write the 8 KiB otadata image that boots ota_1 on the BOILON S3 Gen2.

The first sector keeps the factory entry (sequence 1: ota_0, valid); the
second selects ota_1 (sequence 2, state unset). Each is laid out as ESP-IDF
writes it: label unset, CRC-32 over the sequence number. The bootloader picks
the higher sequence, so the device boots ota_1. Write the app into ota_1
first, then this image at 0x11000 (docs/boilon/FLASHING.md).

  python3 scripts/boilon/make_otadata.py boilon-otadata-ota1.bin
"""

import argparse
import zlib
from pathlib import Path

SECTOR = 0x1000


def otadata_sector(seq: int, state: int) -> bytes:
    number = seq.to_bytes(4, "little")
    entry = (number + b"\xff" * 20 + state.to_bytes(4, "little") +
             zlib.crc32(number, 0xFFFFFFFF).to_bytes(4, "little"))
    return entry + b"\xff" * (SECTOR - len(entry))


OTA_1 = otadata_sector(1, 2) + otadata_sector(2, 0xFFFFFFFF)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="file to write (8192 bytes)")
    args = parser.parse_args()
    args.output.write_bytes(OTA_1)


if __name__ == "__main__":
    main()
