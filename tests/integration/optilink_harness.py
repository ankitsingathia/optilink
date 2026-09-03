"""Host-side driver for the OptiLink firmware.

This is the piece that stands in for a bench: it launches the firmware built
for the host target, talks to its console over a pipe, and exposes the module
as an object instead of a string protocol.

The single most important property here is that nothing sleeps. The firmware
clock is virtual and only advances when ``tick`` says so, so a test that wants
to know what happens 200 ms after a laser fault asks for exactly 200 ms and
gets a deterministic answer. Wall-clock waits are what make hardware test
suites flaky, and there is no reason to inherit that when the timebase is
ours to control.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

SENTINEL_OK = "ok"
SENTINEL_ERR = "err "


class OptilinkError(RuntimeError):
    """The firmware refused a command."""


def default_binary() -> Path:
    name = "optilink.exe" if os.name == "nt" else "optilink"
    return Path(__file__).resolve().parents[2] / "build" / "host" / name


def parse_kv(line: str) -> dict[str, str]:
    """Splits ``a=1 b=2`` into a dict, ignoring any bare leading token.

    Several responses lead with a row label such as ``lane0``; callers that
    care about it read it separately, so it is dropped here rather than
    turned into a key with no value.
    """
    out: dict[str, str] = {}
    for token in line.split():
        if "=" in token:
            key, _, value = token.partition("=")
            out[key] = value
    return out


def _as_int(value: str) -> int:
    return int(value, 16) if value.startswith("0x") else int(value)


class Optilink:
    """A running firmware instance."""

    def __init__(self, binary: Path | None = None) -> None:
        self.binary = Path(binary) if binary else default_binary()
        if not self.binary.exists():
            raise FileNotFoundError(
                f"{self.binary} not built. Run `make host` first."
            )
        self.proc = subprocess.Popen(
            [str(self.binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,
            text=True,
            bufsize=1,
            encoding="ascii",
        )
        banner = self._read_until_sentinel()
        if not banner or "fw=optilink" not in banner[0]:
            raise OptilinkError(f"unexpected banner: {banner!r}")
        self.version = parse_kv(banner[0]).get("version", "?")

    # -- plumbing ---------------------------------------------------------

    def _read_until_sentinel(self) -> list[str]:
        lines: list[str] = []
        while True:
            raw = self.proc.stdout.readline()
            if raw == "":
                raise OptilinkError("firmware exited mid-command")
            line = raw.rstrip("\r\n")
            if line == SENTINEL_OK:
                return lines
            if line.startswith(SENTINEL_ERR):
                raise OptilinkError(line[len(SENTINEL_ERR):])
            lines.append(line)

    def command(self, line: str) -> list[str]:
        """Sends one command and returns its response lines."""
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self._read_until_sentinel()

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self) -> "Optilink":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- time and stimulus ------------------------------------------------

    def tick(self, milliseconds: int) -> None:
        """Advances the firmware clock. One tick is one millisecond."""
        self.command(f"tick {milliseconds}")

    def inject(self, signal: str, *args: int) -> None:
        self.command(" ".join(["inject", signal, *(str(a) for a in args)]))

    def set_lane(self, lane: int, enabled: bool) -> None:
        self.command(f"lane {lane} {'on' if enabled else 'off'}")

    # -- observation ------------------------------------------------------

    def identity(self) -> dict[str, str]:
        return parse_kv(self.command("id")[0])

    def monitors(self) -> dict[str, int]:
        """Flattened monitor readings in CMIS units.

        Keys are ``temp``, ``vcc`` and ``lane<N>_<bias|txpower|rxpower>``.
        """
        out: dict[str, int] = {}
        for line in self.command("mon"):
            fields = parse_kv(line)
            label = line.split()[0]
            prefix = f"{label}_" if label.startswith("lane") else ""
            for key, value in fields.items():
                out[prefix + key] = int(value)
        return out

    def flags(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for line in self.command("flags"):
            fields = parse_kv(line)
            label = line.split()[0]
            prefix = f"{label}_" if label.startswith("lane") else ""
            for key, value in fields.items():
                out[prefix + key] = _as_int(value)
        return out

    def link(self) -> dict[str, object]:
        """Module state plus a list of per-lane dicts, index-aligned."""
        lines = self.command("link")
        module = parse_kv(lines[0])
        lanes = []
        for line in lines[1:]:
            fields = parse_kv(line)
            lanes.append(
                {
                    "state": fields["state"],
                    "enabled": fields["enabled"] == "1",
                    "tx_on": fields["tx_on"] == "1",
                    "transitions": int(fields["transitions"]),
                    "faults": int(fields["faults"]),
                    "retries": int(fields["retries"]),
                    "last_fault": fields["last_fault"],
                }
            )
        return {
            "module": module["module"],
            "faults": int(module["faults"]),
            "active_mask": _as_int(module["active_mask"]),
            "up": module["up"] == "1",
            "lanes": lanes,
        }

    def stats(self) -> dict[str, object]:
        eth: dict[str, int] = {}
        i2c: dict[str, int] = {}
        tasks: dict[str, dict[str, int]] = {}
        uptime = 0
        for line in self.command("stats"):
            head = line.split()[0]
            fields = parse_kv(line)
            if head == "eth":
                eth = {k: int(v) for k, v in fields.items()}
            elif head == "i2c":
                i2c = {k: int(v) for k, v in fields.items()}
            elif head == "task":
                name = line.split()[1]
                tasks[name] = {k: int(v) for k, v in fields.items()}
            elif head.startswith("uptime_ms"):
                uptime = int(fields["uptime_ms"])
        return {"eth": eth, "i2c": i2c, "tasks": tasks, "uptime_ms": uptime}

    # -- management interface ---------------------------------------------

    def i2c_read(self, offset: int, length: int) -> bytes:
        return bytes.fromhex(self.command(f"i2cr {offset} {length}")[0])

    def i2c_write(self, offset: int, *values: int) -> None:
        self.command(" ".join(["i2cw", str(offset), *(str(v) for v in values)]))

    def select_page(self, page: int) -> None:
        self.i2c_write(0x7F, page)

    def read_u16(self, offset: int, signed: bool = False) -> int:
        return int.from_bytes(self.i2c_read(offset, 2), "big", signed=signed)

    def write_u16(self, offset: int, value: int) -> None:
        self.i2c_write(offset, (value >> 8) & 0xFF, value & 0xFF)

    # -- data path ---------------------------------------------------------

    def send_frame(self, data: bytes) -> dict[str, object]:
        """Feeds one L2 frame to the parser and returns what it made of it."""
        lines = self.command("frame " + data.hex())
        merged: dict[str, str] = {}
        for line in lines:
            merged.update(parse_kv(line))
        out: dict[str, object] = {
            "result": merged["result"],
            "len": int(merged["len"]),
        }
        if "ethertype" in merged:
            out.update(
                {
                    "dst": merged["dst"],
                    "src": merged["src"],
                    "ethertype": _as_int(merged["ethertype"]),
                    "vlan": merged["vlan"] == "1",
                    "qinq": merged["qinq"] == "1",
                    "outer_vid": int(merged["outer_vid"]),
                    "outer_pcp": int(merged["outer_pcp"]),
                    "inner_vid": int(merged["inner_vid"]),
                    "payload_len": int(merged["payload_len"]),
                    "fcs_ok": merged["fcs_frame"] == merged["fcs_calc"],
                }
            )
        return out
