"""TGPIO Control Center — a full TUI for the tgpio-ptp-input driver."""

from __future__ import annotations

import os
import sys

from textual.app import App, ComposeResult
from textual.containers import Horizontal
from textual.widgets import (Button, Footer, Header, Input, Label, Select,
                             Switch, TabbedContent, TabPane)

from .driver import TgpioDriver
from .panels import (ControlPanel, DashboardPanel, LogsPanel, OutputPanel,
                     SetupPanel, StatusPanel)

NSEC = 1_000_000_000


class TgpioApp(App):
    TITLE = "TGPIO Control Center"
    SUB_TITLE = "Intel Time-Aware GPIO / tgpio-ptp-input"

    CSS = """
    #dash-grid {
        grid-size: 2; grid-gutter: 1; padding: 1;
        grid-rows: auto auto auto;
    }
    .card {
        border: round $primary; padding: 0 1; height: auto;
        min-height: 7; background: $surface;
    }
    .card-active { border: round $success; }
    .trend { min-height: 6; }
    .trend-value { color: $text-muted; }
    Sparkline { height: 3; margin: 0 1; }
    .section-title {
        padding: 1 1 0 1; color: $secondary; text-style: bold;
    }
    .section-title.warn { color: $warning; }
    #switch-grid { grid-size: 2; grid-gutter: 0 2; padding: 0 1;
                   height: auto; }
    .switch-row { height: 3; align-vertical: middle; }
    .switch-label { padding: 1 0 0 1; }
    #value-grid, #setup-grid { grid-size: 3; grid-gutter: 1; padding: 0 1;
                               height: auto; }
    .value-row { height: auto; }
    .value-label { color: $text-muted; }
    .button-row { height: 3; padding: 0 1; margin-bottom: 1; }
    .button-row Button { margin-right: 2; }
    .row-title { padding: 1 2 0 0; text-style: bold; width: 8; }
    .inline-input { width: 34; margin-right: 2; }
    .button-row Select { width: 18; }
    .filter-button { min-width: 10; }
    #journal { border: round $primary; margin: 0 1 1 1; }
    .mono { padding: 1; }
    #banner {
        dock: top; height: 1; background: $error; color: $text;
        text-align: center; display: none;
    }
    #banner.visible { display: block; }
    """

    BINDINGS = [
        ("q", "quit", "Quit"),
        ("d", "tab('tab-dash')", "Dashboard"),
        ("c", "tab('tab-control')", "Control"),
        ("o", "tab('tab-io')", "I/O"),
        ("s", "tab('tab-setup')", "Setup"),
        ("l", "tab('tab-logs')", "Logs"),
        ("t", "tab('tab-status')", "Raw"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.driver = TgpioDriver()
        self.journal_proc = None

    # ---------------------------------------------------------- layout
    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Label("", id="banner")
        with TabbedContent(initial="tab-dash"):
            with TabPane("Dashboard", id="tab-dash"):
                yield DashboardPanel()
            with TabPane("Control", id="tab-control"):
                yield ControlPanel()
            with TabPane("I/O", id="tab-io"):
                yield OutputPanel()
            with TabPane("Setup", id="tab-setup"):
                yield SetupPanel()
            with TabPane("Logs", id="tab-logs"):
                yield LogsPanel()
            with TabPane("Raw", id="tab-status"):
                yield StatusPanel()
        yield Footer()

    def on_mount(self) -> None:
        self.refresh_all()
        self.set_interval(1.0, self.refresh_all)
        self.run_worker(self.follow_journal, thread=True,
                        exclusive=True, name="journal")

    # ------------------------------------------------------- refreshing
    def refresh_all(self) -> None:
        status = self.driver.status()
        params = self.driver.params()
        banner = self.query_one("#banner", Label)
        if not status.loaded:
            banner.update(" module not loaded — configure and load it from "
                          "the Setup tab ")
            banner.set_class(True, "visible")
        elif "unavailable" in status.raw:
            banner.update(f" {status.raw.strip()} ")
            banner.set_class(True, "visible")
        else:
            banner.set_class(False, "visible")
        self.sub_title = (f"{status.ptp_dev or 'no PTP device'}  ·  "
                          f"clock {status.globals.get('clock_mode', '—')}")
        for panel in (DashboardPanel, ControlPanel, StatusPanel):
            self.query_one(panel).refresh_data(status, params)

    def follow_journal(self) -> None:
        self.journal_proc = self.driver.journal_process()
        logs = self.query_one(LogsPanel)
        for line in self.journal_proc.stdout:
            self.call_from_thread(logs.add_line, line.rstrip())

    # ---------------------------------------------------------- events
    def on_switch_changed(self, event: Switch.Changed) -> None:
        name = (event.switch.id or "").removeprefix("sw-")
        if not name:
            return
        err = self.driver.set_param(name, "1" if event.value else "0")
        self._report(err, f"{name} → {'on' if event.value else 'off'}")

    def on_input_submitted(self, event: Input.Submitted) -> None:
        wid = event.input.id or ""
        if wid.startswith("in-"):
            name = wid.removeprefix("in-")
            err = self.driver.set_param(name, event.value.strip())
            self._report(err, f"{name} = {event.value.strip()}")
            event.input.value = ""

    def on_button_pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id or ""
        if bid.startswith("filter-"):
            self._logs_filter(bid.removeprefix("filter-"))
        elif bid == "act-tdc-reset":
            self._report(self.driver.tdc_reset(), "TDC statistics cleared")
        elif bid.startswith("act-flip-"):
            block = int(bid[-1])
            self._report(self.driver.flip_polarity(block),
                         f"polarity flip requested on TGPIO{block}")
        elif bid == "act-oneshot":
            self._fire_oneshot()
        elif bid == "act-save":
            ok, out = self.driver.save_config()
            self._report("" if ok else out or "save-config failed",
                         "configuration persisted")
        elif bid == "act-reload":
            self._reload()
        elif bid == "act-unload":
            ok, out = self.driver.unload_module()
            self._report("" if ok else out or "unload failed",
                         "module unloaded")
        elif bid.startswith(("out-start-", "out-stop-",
                             "in-start-", "in-stop-")):
            self._io_action(bid)

    # ---------------------------------------------------------- actions
    def _logs_filter(self, name: str) -> None:
        logs = self.query_one(LogsPanel)
        if name == "pause":
            logs.paused = not logs.paused
            self.notify("journal paused" if logs.paused
                        else "journal resumed")
            return
        logs.active_filter = name
        self.notify(f"journal filter: {name}")

    def _fire_oneshot(self) -> None:
        panel = self.query_one(ControlPanel)
        width_text = panel.query_one("#oneshot-width", Input).value.strip()
        block = panel.query_one("#oneshot-block", Select).value
        try:
            width_ns = int(float(width_text) * 1_000_000)
        except ValueError:
            self.notify("one-shot width must be a number of ms",
                        severity="error")
            return
        err = self.driver.oneshot(int(block), 0, width_ns)
        self._report(err, f"one-shot fired on TGPIO{block}: "
                          f"{width_text} ms, 200 ms from now")

    def _io_action(self, bid: str) -> None:
        block = int(bid[-1])
        params = self.driver.params()
        channel = self.driver.channel_for_block(block, params)
        panel = self.query_one(OutputPanel)
        if bid.startswith("out-start-"):
            period_text = panel.query_one(f"#period-{block}",
                                          Input).value.strip()
            duty_text = panel.query_one(f"#duty-{block}", Input).value.strip()
            try:
                period = int(period_text or NSEC)
                duty = int(duty_text) if duty_text else 0
            except ValueError:
                self.notify("period/duty must be integer ns",
                            severity="error")
                return
            err = self.driver.output_start(channel, period, duty)
            self._report(err, f"TGPIO{block} output armed: period {period} ns"
                              + (f", duty {duty} ns" if duty else ""))
        elif bid.startswith("out-stop-"):
            self._report(self.driver.output_stop(channel),
                         f"TGPIO{block} output stopped")
        elif bid.startswith("in-start-"):
            edge = str(panel.query_one(f"#edge-{block}", Select).value)
            self._report(self.driver.extts(channel, True, edge),
                         f"TGPIO{block} capture enabled ({edge})")
        else:
            self._report(self.driver.extts(channel, False),
                         f"TGPIO{block} capture disabled")

    def _reload(self) -> None:
        env = self.query_one(SetupPanel).collect_env()
        self.notify("reloading module…", timeout=2)
        ok, out = self.driver.reload_module(env)
        if ok:
            summary = " ".join(f"{k}={v}" for k, v in sorted(env.items()))
            self.notify(f"module reloaded: {summary}", timeout=8)
        else:
            self.notify(f"reload failed: {out[-300:]}", severity="error",
                        timeout=12)

    def _report(self, err: str, success_msg: str) -> None:
        if err:
            self.notify(err, severity="error", timeout=8)
        else:
            self.notify(success_msg, timeout=4)
        self.refresh_all()

    def action_tab(self, tab_id: str) -> None:
        self.query_one(TabbedContent).active = tab_id

    def on_unmount(self) -> None:
        if self.journal_proc:
            self.journal_proc.terminate()


def main() -> None:
    if os.geteuid() != 0:
        sys.exit("tgpio-tui reads debugfs and writes module parameters: "
                 "run it with sudo.")
    TgpioApp().run()


if __name__ == "__main__":
    main()
