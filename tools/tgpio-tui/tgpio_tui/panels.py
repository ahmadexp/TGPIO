"""Widgets for the TGPIO TUI tabs.

Each panel owns one tab's content and knows how to refresh itself from a
DriverStatus + params snapshot pushed by the app once a second.
"""

from __future__ import annotations

from textual.app import ComposeResult
from textual.containers import Grid, Horizontal, Vertical, VerticalScroll
from textual.widgets import (Button, Input, Label, RichLog, Select, Sparkline,
                             Static, Switch)

from .driver import (RELOAD_FIELDS, RUNTIME_BOOL_PARAMS,
                     RUNTIME_VALUE_PARAMS, DriverStatus)

MAX_TREND = 120  # samples kept for sparklines (~2 minutes)


def _fmt_ns(value: str | None) -> str:
    """'1000000024ns'/'113' -> readable ns with thousands separators."""
    if value in (None, ""):
        return "—"
    try:
        return f"{int(str(value).rstrip('ns')):,} ns"
    except ValueError:
        return str(value)


class BlockCard(Static):
    """Live card for one TGPIO block on the dashboard."""

    def __init__(self, index: int) -> None:
        super().__init__(id=f"block-card-{index}", classes="card")
        self.index = index
        self.border_title = f"TGPIO{index}"

    def show(self, status: DriverStatus) -> None:
        blk = next((b for b in status.blocks if b.index == self.index), None)
        if blk is None:
            self.update("[dim]block unused (off)[/dim]")
            self.set_class(False, "card-active")
            return
        f = blk.fields
        lines = []
        if blk.role == "input":
            lines.append("[b]INPUT[/b]  capture="
                         f"{f.get('capture', '?')}")
            lines.append(f"events: [b]{f.get('events', '0')}[/b]")
            lines.append(f"phase vs second: [b]{_fmt_ns(f.get('phase'))}[/b]")
            lines.append(f"interval: {_fmt_ns(f.get('interval_last'))}")
            lines.append(f"  min {_fmt_ns(f.get('min'))}   "
                         f"max {_fmt_ns(f.get('max'))}")
            lines.append(f"  mean {_fmt_ns(f.get('mean'))}   "
                         f"n={f.get('n', '0')}")
        elif blk.role == "output":
            running = "running" in blk.summary
            state = "[green]RUNNING[/green]" if running else "[red]stopped[/red]"
            lines.append(f"[b]OUTPUT[/b]  {state}  "
                         f"mode={f.get('mode', '?')}")
            lines.append(f"period: [b]{_fmt_ns(f.get('period'))}[/b]"
                         f"  (error {f.get('period_error', '—')})")
            lines.append(f"duty: high {_fmt_ns(f.get('high_time'))} / "
                         f"low {_fmt_ns(f.get('low_time'))}")
            lines.append(f"phase error: [b]{f.get('phase_error', '—')}[/b]")
            lines.append(f"level: {f.get('tracked_level', '?')}   "
                         f"piv={f.get('armed_piv', '—')}")
        else:
            lines.append(f"[dim]{blk.role}[/dim] {blk.summary}")
        self.set_class(True, "card-active")
        self.update("\n".join(lines))


class ClockCard(Static):
    def __init__(self) -> None:
        super().__init__(id="clock-card", classes="card")
        self.border_title = "Clock"

    def show(self, status: DriverStatus, params: dict) -> None:
        g = status.globals
        lines = [
            f"mode: [b]{g.get('clock_mode', '?')}[/b]   "
            f"device: [b]{status.ptp_dev or '—'}[/b]",
            f"ART: {g.get('art_frequency', '?')}   "
            f"base clock {g.get('art_base_clock', '?')}",
            f"phase offset cal: "
            f"{_fmt_ns(params.get('output_phase_offset_ns'))}",
            f"logging: activity={g.get('activity_log', '?')} "
            f"verbose={g.get('verbose', '?')}",
            f"counters: {g.get('counters', '—')}",
        ]
        self.update("\n".join(lines))


class TdcCard(Static):
    def __init__(self) -> None:
        super().__init__(id="tdc-card", classes="card")
        self.border_title = "TDC (bipolar)"

    def show(self, status: DriverStatus, params: dict) -> None:
        if params.get("tdc") != "Y":
            self.update("[dim]TDC mode off — enable it in Control[/dim]")
            return
        t = status.tdc
        if not t:
            self.update("waiting for measurements…")
            return
        self.update("\n".join([
            f"start={t.get('start', '?')}  stop={t.get('stop', '?')}  "
            f"pending={t.get('pending', '?')}",
            f"count: [b]{t.get('count', '0')}[/b]   lost: "
            f"{t.get('lost', '0')}",
            f"last: [b]{_fmt_ns(t.get('last'))}[/b]  "
            f"({t.get('last_cycles', '—')} cycles)",
            f"min {_fmt_ns(t.get('min'))}   max {_fmt_ns(t.get('max'))}",
            f"mean {_fmt_ns(t.get('mean'))}",
        ]))


class TrendCard(Static):
    """Sparkline of a nanosecond series (input phase / output phase error)."""

    def __init__(self, title: str, card_id: str) -> None:
        super().__init__(id=card_id, classes="card trend")
        self.border_title = title
        self.series: list[float] = []
        self.latest = Label("—", classes="trend-value")
        self.spark = Sparkline([], summary_function=max)

    def compose(self) -> ComposeResult:
        yield self.latest
        yield self.spark

    def push(self, value_ns: float | None) -> None:
        if value_ns is None:
            return
        self.series.append(float(value_ns))
        del self.series[:-MAX_TREND]
        # Plot magnitudes so ± jitter is visible on a zero-floor sparkline.
        self.spark.data = [abs(v) for v in self.series]
        peak = max(abs(v) for v in self.series)
        self.latest.update(f"now {value_ns:+,.0f} ns    "
                           f"peak ±{peak:,.0f} ns    n={len(self.series)}")


class DashboardPanel(VerticalScroll):
    def compose(self) -> ComposeResult:
        with Grid(id="dash-grid"):
            yield BlockCard(0)
            yield BlockCard(1)
            yield ClockCard()
            yield TdcCard()
            yield TrendCard("Input phase vs whole second", "trend-input")
            yield TrendCard("Output phase error", "trend-output")

    def refresh_data(self, status: DriverStatus, params: dict) -> None:
        self.query_one("#block-card-0", BlockCard).show(status)
        self.query_one("#block-card-1", BlockCard).show(status)
        self.query_one("#clock-card", ClockCard).show(status, params)
        self.query_one("#tdc-card", TdcCard).show(status, params)
        for blk in status.blocks:
            if blk.role == "input" and "phase" in blk.fields:
                self.query_one("#trend-input", TrendCard).push(
                    _ns_int(blk.fields["phase"]))
            if blk.role == "output" and "phase_error" in blk.fields:
                self.query_one("#trend-output", TrendCard).push(
                    _ns_int(blk.fields["phase_error"]))


def _ns_int(text: str) -> float | None:
    try:
        return float(text.rstrip("ns"))
    except ValueError:
        return None


class ControlPanel(VerticalScroll):
    """Runtime parameters (applied live) and one-click actions."""

    def compose(self) -> ComposeResult:
        yield Label("Runtime parameters — applied immediately",
                    classes="section-title")
        with Grid(id="switch-grid"):
            for name, help_text in RUNTIME_BOOL_PARAMS:
                with Horizontal(classes="switch-row"):
                    yield Switch(id=f"sw-{name}")
                    yield Label(help_text, classes="switch-label")
        with Grid(id="value-grid"):
            for name, help_text in RUNTIME_VALUE_PARAMS:
                with Vertical(classes="value-row"):
                    yield Label(help_text, classes="value-label")
                    yield Input(placeholder="value  (Enter applies)",
                                id=f"in-{name}")
        yield Label("Actions", classes="section-title")
        with Horizontal(classes="button-row"):
            yield Button("Fire one-shot", id="act-oneshot", variant="primary")
            yield Input(placeholder="width ms (≥50)", id="oneshot-width",
                        classes="inline-input")
            yield Select([("block 0", 0), ("block 1", 1)], value=1,
                         id="oneshot-block", allow_blank=False)
        with Horizontal(classes="button-row"):
            yield Button("Flip polarity 0", id="act-flip-0")
            yield Button("Flip polarity 1", id="act-flip-1")
            yield Button("Reset TDC stats", id="act-tdc-reset")
            yield Button("Save config (persist)", id="act-save",
                         variant="success")

    def refresh_data(self, status: DriverStatus, params: dict) -> None:
        for name, _ in RUNTIME_BOOL_PARAMS:
            sw = self.query_one(f"#sw-{name}", Switch)
            live = params.get(name) == "Y"
            if sw.value != live and not sw.has_focus:
                with sw.prevent(Switch.Changed):
                    sw.value = live
        for name, _ in RUNTIME_VALUE_PARAMS:
            box = self.query_one(f"#in-{name}", Input)
            if not box.has_focus and not box.value:
                box.placeholder = f"current: {params.get(name, '?')}"


class OutputPanel(VerticalScroll):
    """Arm and stop periodic outputs through the PTP interface."""

    def compose(self) -> ComposeResult:
        yield Label("Periodic output — armed through the standard PTP "
                    "interface; the driver grid-aligns the first edge",
                    classes="section-title")
        for block in (0, 1):
            with Horizontal(classes="button-row"):
                yield Label(f"TGPIO{block}", classes="row-title")
                yield Input(placeholder="period ns (1 PPS = 1000000000)",
                            id=f"period-{block}", classes="inline-input")
                yield Input(placeholder="duty on-time ns (blank = 50%)",
                            id=f"duty-{block}", classes="inline-input")
                yield Button("Start", id=f"out-start-{block}",
                             variant="primary")
                yield Button("Stop", id=f"out-stop-{block}",
                             variant="error")
        yield Label("Input capture", classes="section-title")
        for block in (0, 1):
            with Horizontal(classes="button-row"):
                yield Label(f"TGPIO{block}", classes="row-title")
                yield Select([("rising", "rising"), ("falling", "falling"),
                              ("both", "both")], value="rising",
                             id=f"edge-{block}", allow_blank=False)
                yield Button("Enable", id=f"in-start-{block}",
                             variant="primary")
                yield Button("Disable", id=f"in-stop-{block}",
                             variant="error")


class SetupPanel(VerticalScroll):
    """Load-time configuration; applying reloads the module."""

    def compose(self) -> ComposeResult:
        yield Label("Module reload — block roles and the clock mode are "
                    "load-time options. Reloading recreates the PTP device: "
                    "restart any tool holding it (ts2phc, phc2sys).",
                    classes="section-title warn")
        with Grid(id="setup-grid"):
            for name, options in RELOAD_FIELDS:
                with Vertical(classes="value-row"):
                    yield Label(name, classes="value-label")
                    yield Select([(o, o) for o in options], value=options[0],
                                 id=f"setup-{name}", allow_blank=False)
            for name, label in (("OUTPUT0_PERIOD_NS", "TGPIO0 period ns"),
                                ("OUTPUT1_PERIOD_NS", "TGPIO1 period ns"),
                                ("OUTPUT_PHASE_OFFSET_NS", "phase offset ns"),
                                ("TDC", "TDC (0/1)"),
                                ("VERBOSE", "verbose (0/1)")):
                with Vertical(classes="value-row"):
                    yield Label(label, classes="value-label")
                    yield Input(placeholder="default", id=f"setup-{name}")
        with Horizontal(classes="button-row"):
            yield Button("Reload module with these settings",
                         id="act-reload", variant="warning")
            yield Button("Unload module", id="act-unload", variant="error")

    def collect_env(self) -> dict:
        env = {}
        for name, _ in RELOAD_FIELDS:
            env[name] = str(self.query_one(f"#setup-{name}", Select).value)
        for widget in self.query(Input):
            name = (widget.id or "").removeprefix("setup-")
            if widget.value.strip():
                env[name] = widget.value.strip()
        if env.get("TGPIO0") == "input":
            env.setdefault("INPUT0_ENABLE", "1")
        if env.get("TGPIO1") == "input":
            env.setdefault("INPUT1_ENABLE", "1")
        return env


class LogsPanel(Vertical):
    """Live kernel journal, filterable by log family."""

    FILTERS = [("all", "tgpio"), ("activity", "activity="),
               ("verbose", "verbose="), ("rounding", "output rounding"),
               ("warnings", None)]

    def __init__(self) -> None:
        super().__init__()
        self.active_filter = "all"
        self.paused = False

    def compose(self) -> ComposeResult:
        with Horizontal(classes="button-row"):
            for name, _ in self.FILTERS:
                yield Button(name.title(), id=f"filter-{name}",
                             classes="filter-button")
            yield Button("Pause", id="filter-pause")
        yield RichLog(id="journal", max_lines=2000, wrap=False,
                      highlight=False)

    def add_line(self, line: str) -> None:
        if self.paused or "tgpio" not in line:
            return
        _, needle = next(f for f in self.FILTERS
                         if f[0] == self.active_filter)
        if self.active_filter == "warnings":
            if not any(w in line.lower() for w in ("warn", "error", "stall")):
                return
        elif needle and needle not in line:
            return
        self.query_one("#journal", RichLog).write(line)


class StatusPanel(VerticalScroll):
    def compose(self) -> ComposeResult:
        yield Static(id="raw-status", classes="mono")

    def refresh_data(self, status: DriverStatus, params: dict) -> None:
        param_dump = "\n".join(f"  {k} = {v}" for k, v in params.items())
        self.query_one("#raw-status", Static).update(
            f"[b]/sys/kernel/debug/tgpio/status[/b]\n{status.raw}\n"
            f"[b]/sys/module/tgpio_ptp_input/parameters[/b]\n{param_dump}")
