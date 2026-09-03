#!/usr/bin/env python3
"""Footprint gate for the target image.

Reports flash and RAM against the budget for the part and exits non-zero if
either is exceeded. Wired into CI so a change that quietly costs three
kilobytes of flash fails on the commit that caused it, rather than being
discovered months later when the image no longer fits.

Usage: size_check.py <size-tool> <elf> <flash-limit> <ram-limit>
"""

from __future__ import annotations

import subprocess
import sys


def bar(used: int, limit: int, width: int = 32) -> str:
    filled = min(width, round(width * used / limit)) if limit else 0
    return "[" + "#" * filled + "." * (width - filled) + "]"


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2

    size_tool, elf, flash_limit, ram_limit = argv[1], argv[2], int(argv[3]), int(argv[4])

    try:
        output = subprocess.run(
            [size_tool, elf], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"could not run {size_tool}: {exc}", file=sys.stderr)
        return 2

    rows = output.strip().splitlines()
    if len(rows) < 2:
        print(f"unexpected size output:\n{output}", file=sys.stderr)
        return 2
    text, data, bss = (int(field) for field in rows[1].split()[:3])

    # .data lands in both: it is stored in flash and copied into RAM at reset.
    flash = text + data
    ram = data + bss

    print(f"flash {flash:>7} / {flash_limit:<7} {bar(flash, flash_limit)} "
          f"{100 * flash / flash_limit:5.1f}%")
    print(f"ram   {ram:>7} / {ram_limit:<7} {bar(ram, ram_limit)} "
          f"{100 * ram / ram_limit:5.1f}%")

    failures = []
    if flash > flash_limit:
        failures.append(f"flash over budget by {flash - flash_limit} bytes")
    if ram > ram_limit:
        failures.append(f"ram over budget by {ram - ram_limit} bytes")

    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
