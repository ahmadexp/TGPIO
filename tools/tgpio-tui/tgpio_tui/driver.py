"""Interface between the TUI and the tgpio-ptp-input driver.

Everything the TUI shows or changes flows through this module: module
parameters under /sys/module, the debugfs status file, the debugfs action
files (tdc_reset, oneshotN, outputN_invert), PTP character-device ioctls
for output generation and input capture, the kernel journal, and the
repository load/unload/save-config scripts.
"""

from __future__ import annotations

import fcntl
import os
import re
import struct
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

MODULE = "tgpio_ptp_input"
PARAMS_DIR = Path("/sys/module") / MODULE / "parameters"
DEBUGFS_DIR = Path("/sys/kernel/debug/tgpio")
PTP_SYS = Path("/sys/class/ptp")
CLOCK_NAME = "Intel TGPIO"

# Parameters safely writable at runtime (mirrors the 0644 params).
RUNTIME_BOOL_PARAMS = [
    ("verbose", "Verbose (every feature logs details)"),
    ("activity_log", "Activity log (events, arms, stops)"),
    ("verbose_rounding", "Rounding log (every output program)"),
    ("tdc", "TDC mode (pair inputs into durations)"),
    ("auto_polarity", "Auto-polarity (loopback wiring)"),
    ("hardware_timestamps", "Hardware capture timestamps"),
]
RUNTIME_VALUE_PARAMS = [
    ("output_phase_offset_ns", "Output phase offset (ns)"),
    ("output_phase_tolerance_ns", "Phase nudge dead-band (ns)"),
    ("tdc_start", "TDC start block (0 or 1)"),
    ("poll_ms", "Input poll interval (ms)"),
    ("rate_trim_ppb", "ART-mode rate trim (ppb)"),
]

# Load-time options fed to scripts/load.sh through the environment.
RELOAD_FIELDS = [
    ("TGPIO0", ["input", "output", "off"]),
    ("TGPIO1", ["input", "output", "off"]),
    ("EDGE0", ["rising", "falling", "both"]),
    ("EDGE1", ["rising", "falling", "both"]),
    ("CLOCK_MODE", ["phc", "art", "realtime"]),
]
RELOAD_VALUES = [
    "OUTPUT0_PERIOD_NS", "OUTPUT1_PERIOD_NS",
    "OUTPUT0_DUTY_NS", "OUTPUT1_DUTY_NS",
    "OUTPUT_PHASE_OFFSET_NS", "POLL_MS",
]
RELOAD_FLAGS = [
    "INPUT0_ENABLE", "INPUT1_ENABLE", "TDC", "TDC_START",
    "AUTO_POLARITY", "VERBOSE", "ACTIVITY_LOG",
]

# PTP ioctls (include/uapi/linux/ptp_clock.h, magic '=').
_PTP_MAGIC = ord("=")


def _iow(nr: int, size: int) -> int:
    return (1 << 30) | (size << 16) | (_PTP_MAGIC << 8) | nr


PTP_EXTTS_REQUEST2 = _iow(11, 16)
PTP_PEROUT_REQUEST2 = _iow(12, 56)
PTP_ENABLE_FEATURE = 1 << 0
PTP_RISING_EDGE = 1 << 1
PTP_FALLING_EDGE = 1 << 2
PTP_PEROUT_DUTY_CYCLE = 1 << 1


@dataclass
class BlockStatus:
    """One block's parsed line from the debugfs status file."""

    index: int = 0
    role: str = "off"          # input | output | off
    summary: str = ""          # raw remainder of the line
    fields: dict = field(default_factory=dict)


@dataclass
class DriverStatus:
    loaded: bool = False
    ptp_dev: str = ""
    globals: dict = field(default_factory=dict)
    blocks: list = field(default_factory=list)
    tdc: dict = field(default_factory=dict)
    raw: str = ""


class TgpioDriver:
    """Stateless helpers; every call reads the live kernel state."""

    def __init__(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[3]

    # ---------------------------------------------------------- state
    def is_loaded(self) -> bool:
        return PARAMS_DIR.is_dir()

    def ptp_device(self) -> str:
        try:
            for clock in sorted(PTP_SYS.glob("ptp*/clock_name")):
                if clock.read_text().strip() == CLOCK_NAME:
                    return "/dev/" + clock.parent.name
        except OSError:
            pass
        return ""

    def params(self) -> dict:
        out = {}
        if not self.is_loaded():
            return out
        for p in sorted(PARAMS_DIR.iterdir()):
            try:
                out[p.name] = p.read_text().strip()
            except OSError as exc:
                out[p.name] = f"<{exc.strerror}>"
        return out

    def set_param(self, name: str, value: str) -> str:
        """Returns '' on success, an error message otherwise."""
        try:
            (PARAMS_DIR / name).write_text(str(value))
            return ""
        except OSError as exc:
            return f"{name}: {exc.strerror}"

    # --------------------------------------------------------- status
    def status(self) -> DriverStatus:
        st = DriverStatus(loaded=self.is_loaded(), ptp_dev=self.ptp_device())
        if not st.loaded:
            return st
        try:
            st.raw = (DEBUGFS_DIR / "status").read_text()
        except OSError as exc:
            st.raw = f"status unavailable: {exc.strerror} (run as root?)"
            return st
        for line in st.raw.splitlines():
            m = re.match(r"block(\d): (\w+)\s*(.*)", line)
            if m:
                blk = BlockStatus(index=int(m.group(1)), role=m.group(2),
                                  summary=m.group(3).strip())
                blk.fields = self._kv(blk.summary)
                st.blocks.append(blk)
                continue
            if line.startswith("tdc:"):
                st.tdc = self._kv(line[4:].strip())
                continue
            m = re.match(r"([\w ]+): (.*)", line)
            if m:
                st.globals[m.group(1)] = m.group(2)
        return st

    @staticmethod
    def _kv(text: str) -> dict:
        out = {}
        for tok in text.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
            else:
                out.setdefault("_flags", []).append(tok)
        return out

    # -------------------------------------------------------- actions
    def _debugfs_write(self, name: str, payload: str) -> str:
        try:
            (DEBUGFS_DIR / name).write_text(payload)
            return ""
        except OSError as exc:
            return f"{name}: {exc.strerror}"

    def tdc_reset(self) -> str:
        return self._debugfs_write("tdc_reset", "1")

    def flip_polarity(self, block: int) -> str:
        return self._debugfs_write(f"output{block}_invert", "1")

    def oneshot(self, block: int, start_ns: int, width_ns: int) -> str:
        return self._debugfs_write(f"oneshot{block}",
                                   f"{start_ns} {width_ns}")

    # ----------------------------------------------------- ptp ioctls
    def _ptp_ioctl(self, request: int, payload: bytes) -> str:
        dev = self.ptp_device()
        if not dev:
            return "no TGPIO PTP device found"
        try:
            fd = os.open(dev, os.O_RDWR)
        except OSError as exc:
            return f"{dev}: {exc.strerror}"
        try:
            fcntl.ioctl(fd, request, payload)
            return ""
        except OSError as exc:
            return f"ioctl: {exc.strerror}"
        finally:
            os.close(fd)

    def output_start(self, channel: int, period_ns: int,
                     duty_ns: int = 0) -> str:
        """Arm periodic output on a PTP channel; the driver grid-aligns."""
        flags = PTP_PEROUT_DUTY_CYCLE if duty_ns else 0
        on_s, on_n = divmod(duty_ns, 1_000_000_000)
        per_s, per_n = divmod(period_ns, 1_000_000_000)
        req = struct.pack("qII qII II qII",
                          0, 0, 0,           # start: 0 = driver picks grid
                          per_s, per_n, 0,   # period
                          channel, flags,
                          on_s, on_n, 0)     # duty on-time
        return self._ptp_ioctl(PTP_PEROUT_REQUEST2, req)

    def output_stop(self, channel: int) -> str:
        req = struct.pack("qII qII II qII", 0, 0, 0, 0, 0, 0,
                          channel, 0, 0, 0, 0)
        return self._ptp_ioctl(PTP_PEROUT_REQUEST2, req)

    def extts(self, channel: int, enable: bool, edge: str = "rising") -> str:
        flags = 0
        if enable:
            flags = PTP_ENABLE_FEATURE
            flags |= {"rising": PTP_RISING_EDGE,
                      "falling": PTP_FALLING_EDGE,
                      "both": PTP_RISING_EDGE | PTP_FALLING_EDGE}[edge]
        req = struct.pack("II8x", channel, flags)
        return self._ptp_ioctl(PTP_EXTTS_REQUEST2, req)

    # -------------------------------------------------------- scripts
    def _run_script(self, script: str, env_extra: dict | None = None):
        path = self.repo_root / "scripts" / script
        env = dict(os.environ)
        env.update({k: str(v) for k, v in (env_extra or {}).items()})
        try:
            proc = subprocess.run([str(path)], env=env, cwd=self.repo_root,
                                  capture_output=True, text=True, timeout=60)
            out = (proc.stdout + proc.stderr).strip()
            return proc.returncode == 0, out
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, str(exc)

    def reload_module(self, env: dict):
        ok, out = self._run_script("unload.sh")
        if not ok and self.is_loaded():
            return False, out
        return self._run_script("load.sh", env)

    def unload_module(self):
        return self._run_script("unload.sh")

    def save_config(self):
        return self._run_script("save-config.sh")

    # --------------------------------------------------------- journal
    def journal_process(self) -> subprocess.Popen:
        """Follow the kernel journal; the TUI filters per view."""
        return subprocess.Popen(
            ["journalctl", "-k", "-f", "-n", "200", "-o", "short-precise",
             "--no-pager"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def channel_for_block(self, block: int, params: dict) -> int:
        st = self.status()
        role = ""
        for blk in st.blocks:
            if blk.index == block:
                role = blk.role
        key = (f"output{block}_channel" if role == "output"
               else f"input{block}_channel")
        try:
            return int(params.get(key, block))
        except ValueError:
            return block
