#!/usr/bin/env python3
"""Frame generator and loss measurement for the OptiLink data path.

Stands in for a hardware traffic generator. It builds real Ethernet frames,
optionally damages a controlled fraction of them, pushes them through the
firmware's parser, and reports what came back: how many were accepted, how
many were rejected and for which reason, and whether the firmware's own
counters agree with what was sent.

That last check is the point. Any generator can count what it transmitted;
what catches firmware bugs is reconciling the transmitted count against the
counters the device reports, because a parser that silently drops a frame
class shows up as a discrepancy and nothing else.
"""

from __future__ import annotations

import argparse
import os
import random
import sys
import time
import zlib
from pathlib import Path

BROADCAST = b"\xff" * 6
DEFAULT_DST = bytes.fromhex("001122334455")
DEFAULT_SRC = bytes.fromhex("aabbccddeeff")

ETH_MIN_FRAME = 64
TPID_8021Q = 0x8100
TPID_8021AD = 0x88A8
TYPE_IPV4 = 0x0800


def fcs(payload: bytes) -> bytes:
    """Ethernet FCS: reflected CRC-32, transmitted least significant byte
    first. zlib's crc32 is bit-for-bit the same function."""
    return zlib.crc32(payload).to_bytes(4, "little")


def build_frame(
    dst: bytes = DEFAULT_DST,
    src: bytes = DEFAULT_SRC,
    ethertype: int = TYPE_IPV4,
    payload: bytes = b"",
    vlan: tuple[int, int] | None = None,
    inner_vlan: tuple[int, int] | None = None,
) -> bytes:
    """Builds a complete frame including FCS.

    ``vlan`` and ``inner_vlan`` are (pcp, vid) pairs. Supplying both produces
    an 802.1ad double tagged frame. The payload is padded so the result meets
    the 64 byte minimum, exactly as a real MAC would pad it.
    """
    body = bytearray(dst + src)
    if vlan is not None:
        pcp, vid = vlan
        outer_tpid = TPID_8021AD if inner_vlan is not None else TPID_8021Q
        body += outer_tpid.to_bytes(2, "big")
        body += (((pcp & 0x7) << 13) | (vid & 0xFFF)).to_bytes(2, "big")
        if inner_vlan is not None:
            ipcp, ivid = inner_vlan
            body += TPID_8021Q.to_bytes(2, "big")
            body += (((ipcp & 0x7) << 13) | (ivid & 0xFFF)).to_bytes(2, "big")
    body += ethertype.to_bytes(2, "big")
    body += payload

    pad = ETH_MIN_FRAME - 4 - len(body)
    if pad > 0:
        body += bytes(pad)
    return bytes(body) + fcs(bytes(body))


def corrupt(frame: bytes, rng: random.Random) -> bytes:
    """Flips one bit somewhere in the payload, leaving the FCS untouched so
    the frame arrives with a checksum that no longer matches."""
    out = bytearray(frame)
    index = rng.randrange(14, len(out) - 4)
    out[index] ^= 1 << rng.randrange(8)
    return bytes(out)


def random_frame(rng: random.Random, max_payload: int = 200) -> bytes:
    """One frame from a spread of the shapes the parser has to handle."""
    shape = rng.random()
    payload = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, max_payload)))
    dst = BROADCAST if shape > 0.9 else DEFAULT_DST
    if shape < 0.25:
        return build_frame(dst=dst, payload=payload, vlan=(rng.randrange(8), rng.randrange(1, 4095)))
    if shape < 0.35:
        return build_frame(
            dst=dst,
            payload=payload,
            vlan=(rng.randrange(8), rng.randrange(1, 4095)),
            inner_vlan=(rng.randrange(8), rng.randrange(1, 4095)),
        )
    return build_frame(dst=dst, payload=payload)


def run(binary: Path, frames: int, corrupt_rate: float, seed: int) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "integration"))
    from optilink_harness import Optilink  # noqa: E402

    rng = random.Random(seed)
    sent_clean = 0
    sent_dirty = 0
    accepted = 0
    rejected: dict[str, int] = {}

    with Optilink(binary) as fw:
        fw.tick(300)  # bring the link up before offering it traffic
        link = fw.link()
        if not link["up"]:
            print(f"link did not come up: {link['module']}", file=sys.stderr)
            return 2

        started = time.perf_counter()
        for _ in range(frames):
            frame = random_frame(rng)
            if rng.random() < corrupt_rate:
                frame = corrupt(frame, rng)
                sent_dirty += 1
            else:
                sent_clean += 1
            result = fw.send_frame(frame)["result"]
            if result == "OK":
                accepted += 1
            else:
                rejected[result] = rejected.get(result, 0) + 1
        elapsed = time.perf_counter() - started

        counters = fw.stats()["eth"]

    print(f"sent          {frames}  ({sent_clean} clean, {sent_dirty} corrupted)")
    print(f"accepted      {accepted}")
    for reason, count in sorted(rejected.items()):
        print(f"  rejected {reason:<10} {count}")
    print(f"elapsed       {elapsed:.2f}s  ({frames / elapsed:.0f} frames/s)")
    print("device counters:")
    for key in ("frames", "fcs_err", "vlan", "qinq", "uc", "mc", "bc"):
        print(f"  {key:<10} {counters.get(key, 0)}")

    # The device must have seen every frame, and its FCS error count must
    # equal the number deliberately damaged. Either mismatch is a real bug.
    problems = []
    if counters.get("frames") != frames:
        problems.append(f"device counted {counters.get('frames')} frames, sent {frames}")
    if counters.get("fcs_err") != sent_dirty:
        problems.append(
            f"device counted {counters.get('fcs_err')} FCS errors, corrupted {sent_dirty}"
        )
    if accepted != sent_clean:
        problems.append(f"accepted {accepted}, sent {sent_clean} clean")

    if problems:
        for problem in problems:
            print(f"MISMATCH: {problem}", file=sys.stderr)
        return 1
    print("reconciled: device counters match what was sent")
    return 0


def main() -> int:
    default = "optilink.exe" if os.name == "nt" else "optilink"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "build" / "host" / default,
    )
    parser.add_argument("--frames", type=int, default=1000)
    parser.add_argument(
        "--corrupt-rate",
        type=float,
        default=0.1,
        help="fraction of frames to damage after the FCS is computed",
    )
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    return run(args.binary, args.frames, args.corrupt_rate, args.seed)


if __name__ == "__main__":
    raise SystemExit(main())
