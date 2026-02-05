#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable, Iterator


def _iter_ihex_data(path: Path) -> Iterator[tuple[int, bytes]]:
    """Yield (address, data_bytes) from an Intel HEX file."""
    base = 0
    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"{path}:{lineno}: expected ':'")
        if len(line) < 11:
            raise ValueError(f"{path}:{lineno}: short record")

        count = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        rectype = int(line[7:9], 16)
        data_hex = line[9 : 9 + count * 2]
        if len(data_hex) != count * 2:
            raise ValueError(f"{path}:{lineno}: truncated data")

        if rectype == 0x00:  # data
            yield base + addr, (bytes.fromhex(data_hex) if count else b"")
            continue
        if rectype == 0x01:  # EOF
            break
        if rectype == 0x04:  # extended linear address
            if count != 2:
                raise ValueError(f"{path}:{lineno}: type 04 expects 2 data bytes")
            base = int(data_hex, 16) << 16
            continue
        if rectype == 0x02:  # extended segment address
            if count != 2:
                raise ValueError(f"{path}:{lineno}: type 02 expects 2 data bytes")
            base = int(data_hex, 16) << 4
            continue


def _write_memh(records: Iterable[tuple[int, bytes]], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        for addr, data in records:
            f.write(f"@{addr:08x}\n")
            for b in data:
                f.write(f"{b:02x}\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert Intel HEX to tb-compatible memh (@ADDR + byte tokens).")
    ap.add_argument("input_hex", type=Path)
    ap.add_argument("output_memh", type=Path)
    args = ap.parse_args()

    _write_memh(_iter_ihex_data(args.input_hex), args.output_memh)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

