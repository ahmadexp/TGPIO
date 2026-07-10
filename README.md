# TGPIO

<p align="center">
  <img src="assets/tgpio-logo.svg" alt="TGPIO logo" width="760">
</p>

Author: Ahmad Byagowi

Linux kernel module that exposes Time-Aware GPIO / Timed I/O blocks on systems.
It also works where system's firmware does not enumerate the ACPI devices.

Each static TGPIO block can be selected independently as:

- `off`: leave the block unused (0).
- `input`: PTP external timestamp input (1).
- `output`: PTP periodic output (2).

The default is both blocks as inputs, matching the first working input setup.

## Feature Overview

| Capability | Summary | Measured on the validated setup |
|---|---|---|
| Periodic output | Hardware toggle engine, grid-aligned starts, deterministic polarity | 1 PPS at **+9 ± 64 ns** vs an atomic reference (phc2sys path) |
| Asymmetric duty | Per-edge interval alternation, halves >= 50 ms, every edge hardware-timed | 25% duty: 249.998 ms / 749.995 ms |
| One-shot pulse | `oneshotN` debugfs: single hardware-timed pulse at an absolute time | 100 ms request measured 99.9990 ms |
| Timestamp input | Hardware ART-domain capture, per-edge selection, live statistics | capture-to-grid `phase` readout at ns level |
| TDC mode | Start/stop duration measurement across the two pins, roles selectable | ~26 ns LSB, 64-bit range |
| PPS sources | RFC 2783 `/dev/ppsX` per input, no PTP awareness needed | 1/s hardware-timestamped asserts |
| Clock modes | `phc` (adjustable), `art` (raw, unadjustable), `realtime` (steers the system clock) | chrony locked the system clock to a captured PPS at −218 ns |
| Discipline | ts2phc in the ART domain, or phc2sys PHC-to-PHC | ~50 ns uncalibrated systematic (ART loop) |
| Precise crosststamps | `PTP_SYS_OFFSET_PRECISE` with exact device/realtime pairing | adopted automatically by phc2sys/chrony |
| Observability | status file, activity log, quantization + verbose rounding logs, stall watchdog | rounding self-check within ±13 ns |
| Calibration & persistence | `output_phase_offset_ns`, polarity knobs, auto-polarity (loopback), `make save-config` | one-shot calibration, survives reboots |

Full documentation lives in this README and the
[project wiki](https://github.com/ahmadexp/TGPIO/wiki); run `make help` for
every load-time option with examples.

## Pre Req

```
sudo apt install make gcc
```

## Build And Load

```sh
make
sudo make load
make status
```

Defaults:

```text
addr0=0xFE001210
addr1=0xFE001310
mmio_size=0x38
tgpio0=input
tgpio1=input
edge0=rising
edge1=rising
clock_mode=phc
timestamp_mode=realtime
output_polarity=normal
poll_ms=10
art_frequency=0
hardware_timestamps=Y
hardware_periodic_output=Y
activity_log=N
input0_enable=N
input1_enable=N
input0_channel=0
input1_channel=1
output0_channel=0
output1_channel=1
output0_period_ns=0
output1_period_ns=0
output0_duty_ns=0
output1_duty_ns=0
output_start_delay_ns=0
```

`art_frequency=0` means auto-detect the ART/crystal frequency from CPUID leaf
`0x15` when raw ART timestamp mode needs it. PHC mode normally captures a
calibrated ART base rate from the kernel timekeeper at load time; set
`ART_FREQUENCY=<Hz>` only when you want to override that PHC base rate manually.

CPUID leaf `0x15` also reports the TSC/ART ratio. The driver records it as
`tsc_art_numerator` and `tsc_art_denominator` and shows it in `make status`.

`clock_mode=phc` is the default and exposes an adjustable ART-backed PHC. In
PHC mode the driver implements `gettime64`, `settime64`, `adjtime`, and
`adjfine`, reports a non-zero `max_adj`, and emits hardware external timestamp
events in the adjusted PHC time domain. This is the mode to use with tools that
discipline a PHC from external timestamps, such as `ts2phc`.

`CLOCK_MODE` selects one of three PTP clock timebases:

- `phc` (default): an adjustable ART-backed PHC. `settime`, `adjtime`, and
  `adjfine` work, so tools like `ts2phc` and `phc2sys` can discipline it.
- `art`: the same ART-backed clock model, but anchored to
  `CLOCK_MONOTONIC_RAW` and deliberately **not adjustable** — `settime`,
  `adjtime`, and `adjfine` all return `EOPNOTSUPP` and `max_adj` is 0. The
  cycles-per-second rate is calibrated against the raw clock over 250 ms at
  load and refined once over a 10 s baseline shortly after (watch for the
  `ART clock base rate refined` kernel log line). Use this when you want an
  undisciplined hardware-paced timebase that nothing can steer.
- `realtime`: the PTP clock returns Linux `CLOCK_REALTIME` directly, and
  conversions go through the kernel timekeeper per call. This mode is
  **adjustable, and the adjustments steer the system clock itself**:
  `settime64` sets `CLOCK_REALTIME`, `adjtime` applies an offset
  (`ADJ_SETOFFSET`), and `adjfine` slews the kernel NTP frequency
  (`ADJ_FREQUENCY`, `max_adj` = 500 ppm). Frequency and atomic-offset
  control need `do_adjtimex`, which is not exported to modules; the driver
  resolves it via a kprobe and logs a warning (falling back to settime-only
  plus stepped adjtime) if that fails. Combined with a reference PPS wired
  into a TGPIO input, this lets the OS clock be disciplined directly from
  hardware timestamps. Note that linuxptp's `ts2phc -s generic` references
  CLOCK_TAI and will try to drag a UTC system clock 37 s away — for system
  clock discipline use chrony's PHC refclock instead:

  ```
  refclock PHC /dev/ptpX:extpps:pin=0 poll 2 refid TPPS prefer
  ```

  Validated live: chrony locked to the TGPIO-captured atomic PPS with a
  -218 ns system-clock offset and 11 ns per-sample standard deviation.
  (A free-running lab PPS whose pulse is not on the UTC second will
  eventually conflict with NTP sources; with a GNSS-aligned PPS this
  configuration holds.)

In realtime clock mode, `TIMESTAMP_MODE=realtime` reports hardware input
captures in the same `CLOCK_REALTIME` timebase returned by the PTP clock. Use
`TIMESTAMP_MODE=art` to report raw ART-cycle-derived nanoseconds instead.

```sh
sudo make reload CLOCK_MODE=art TGPIO0=input TGPIO1=output OUTPUT1_PERIOD_NS=1000000000
```

Mixed-mode examples:

```sh
sudo make reload TGPIO0=output TGPIO1=input
sudo make reload TGPIO0=input TGPIO1=output
sudo make reload TGPIO0=output TGPIO1=off
sudo make reload TGPIO0=output TGPIO1=input EDGE1=rising
sudo make reload TGPIO0=input TIMESTAMP_MODE=art
sudo make reload TGPIO0=output OUTPUT_POLARITY=inverted
sudo make reload TGPIO0=output OUTPUT0_PERIOD_NS=1000000 OUTPUT0_DUTY_NS=250000
sudo make reload TGPIO0=output HARDWARE_PERIODIC_OUTPUT=0
sudo make reload TGPIO0=input TGPIO1=off
sudo make reload CLOCK_MODE=realtime TGPIO0=input TGPIO1=off
```

`reload` unloads the add-on and reloads it with the selected modes and input
edge defaults.

Before proceeding with Input and Output, make sure testptp is installed. If not, here is a repo:
https://github.com/Time-Appliances-Project/Incubation-Projects/tree/master/Software/testptp

## Python Control With PyPTM

PyPTM is a companion Python library and CLI for TSC/PTP/TGPIO experiments:

https://github.com/ahmadexp/PyPTM

It wraps the same Linux PTP character-device ioctls used by `testptp`, so it can
list PTP clocks, map TGPIO pins, enable external timestamp input, start periodic
output, and read timestamp events from Python.

Example:

```sh
pyptm list
sudo pyptm caps -d /dev/ptpX
sudo pyptm input -d /dev/ptpX --pin 1 --channel 0 --edge rising
sudo pyptm output -d /dev/ptpX --pin 0 --channel 0 --period-ns 1000000000
```

PyPTM also exposes `RDTSC`, ordered `RDTSC`, `RDTSCP`, CPUID feature checks, and
`TPAUSE` when the CPU reports WAITPKG support.

## Input With testptp

Find the PTP device:

```sh
make status
```

Then use the device whose clock name is `Intel TGPIO`:

```sh
sudo testptp -l -d /dev/ptpX
sudo testptp -i 0 -L 0,1 -d /dev/ptpX
sudo testptp -i 0 -e 100 -d /dev/ptpX
```

For block 1 input:

```sh
sudo testptp -i 0 -L 1,1 -d /dev/ptpX
sudo testptp -i 0 -e 100 -d /dev/ptpX
```

For `testptp -L`, the `-i` value is the PTP channel. It does not need to match
the physical TGPIO block number; for example, `-i 0 -L 1,1` maps physical
TGPIO1 to external timestamp channel 0.

By default, input timestamps are captured on rising edges. Choose a different
edge mode at load time with `EDGE0` or `EDGE1`:

```sh
sudo make reload TGPIO0=output TGPIO1=input EDGE1=rising
sudo make reload TGPIO0=output TGPIO1=input EDGE1=falling
sudo make reload TGPIO0=output TGPIO1=input EDGE1=both
```

If a userspace program sends explicit PTP rising/falling edge flags, those flags
override the module default for that request.

## Adjustable PHC Mode

The default `CLOCK_MODE=phc` creates a software-adjusted PHC backed by the
platform ART counter. The ART counter itself is not disciplined; the driver
keeps an adjustable PHC offset and frequency scale on top of ART. This allows
tools such as `ts2phc` to steer the TGPIO PTP clock while the TGPIO
capture/compare registers continue to use ART-domain hardware timestamps.

Example load for a TGPIO external timestamp input:

```sh
sudo make reload TGPIO0=input TGPIO1=off EDGE0=rising
```

In PHC mode:

- `testptp -g` reads the adjusted TGPIO PHC time.
- `testptp -s` and linuxptp adjustment calls can set or adjust the PHC.
- External timestamp events are reported in the adjusted PHC time domain.
- Periodic output requests are interpreted in the adjusted PHC time domain and
  converted back to ART compare values before programming hardware.
- If a tool such as `phc2sys` steps the TGPIO PHC while periodic output is
  active, the driver re-primes and re-arms the output in the adjusted PHC
  domain so a stale compare value does not stop the waveform. The re-armed
  first edge stays on the requested period grid (`start + k * period`), so a
  step never shifts the waveform phase.
- If `phc2sys` only changes PHC frequency with `adjfine`, the driver
  hot-refreshes `PIV` to the current servo rate (verified safe on this
  hardware: a `PIV` write latches for the following periods) and, when the
  pending edge has drifted more than 200 ns off the requested grid, rewrites
  `COMPV` to pull it back (`activity=output_phase_nudge`). No waveform
  restart happens in steady state; a full grid-aligned re-arm
  (`activity=output_phase_rearm`) is only the backstop for errors beyond a
  quarter period.
- `output_phase_offset_ns` (runtime writable under
  `/sys/module/tgpio_ptp_input/parameters/`) shifts every programmed output
  edge by a constant, so a one-shot external measurement can cancel
  systematic delays such as the asymmetric PCIe read latency in
  `phc2sys` PHC-to-PHC comparison. A running output converges onto the new
  position within a couple of frequency updates via the nudge machinery.
  With this calibrated (about -2.5 us on the validated platform) and
  `phc2sys -R 8 -N 5`, the 1 PPS output measures within +/-100 ns of the
  atomic-clock reference.
- Output polarity is deterministic through software level tracking. The
  hardware output level flop is write-only state: it holds the level of the
  last generated toggle, survives disable, and cannot be loaded or read (EP
  writes and single-shot output compares do nothing on this hardware). The
  driver therefore mirrors the flop in software — it assumes the power-on
  low state at load, counts every generated toggle through the live `COMPV`
  readback, drains the flop low before stops and unloads so the assumption
  stays true across reloads, and picks the compare slot at arm time so
  rising edges always land on the requested grid. If the mirror is ever
  wrong (for example after external register pokes), writing 1 to
  `/sys/kernel/debug/tgpio/outputN_invert` flips the tracked level and
  shifts a running waveform by half a period, glitch-free. The tracked
  level appears as `tracked_level` in the status output.

`TIMESTAMP_MODE=art` is intended for explicit `CLOCK_MODE=realtime` debugging.
In the default PHC mode, hardware input events are emitted in adjusted PHC time
so that PHC tools see a consistent clock domain.

## TDC Mode (Time-to-Digital Converter)

With both blocks as inputs, `TDC=1` pairs them into a duration counter:
one block is Start, the other is Stop, and each completed pair yields the
time between the two edges. `TDC_START` selects which block starts
(default 0); it is runtime-writable under
`/sys/module/tgpio_ptp_input/parameters/tdc_start`, so the roles can be
swapped on a running measurement.

```sh
sudo make reload TDC=1 TGPIO0=input TGPIO1=input EDGE0=rising EDGE1=rising
sudo make reload TDC=1 TDC_START=1 TGPIO0=input TGPIO1=input   # block 1 starts
```

Both timestamps come from the hardware capture registers in the same ART
domain, so the measurement is immune to clock discipline, NTP slew, and
software latency. Resolution is one ART cycle per edge (about 26 ns at
38.4 MHz, roughly 11 ns RMS on the difference); range is the full 64-bit
counter, so durations from tens of nanoseconds to years are equally valid.
Both captures are armed automatically at load; edges are selected with
`EDGE0`/`EDGE1` as usual, and the normal PTP external timestamp events keep
flowing alongside.

Semantics: the TDC is bipolar. An edge on either input arms a measurement
and the next edge on the other input completes it; the result is signed
Stop − Start, so a Stop that precedes its Start reads negative. Repeated
edges on the armed input re-arm (latest wins). Because the poll loop latches
one capture per block per interval (`POLL_MS`), repeated edges faster than
the poll rate keep only the most recent; overwritten events are counted as
`lost`. A Stop and its Start may land in the same poll window — sub-poll
(even sub-microsecond) durations measure correctly since the pairing uses
the hardware timestamps, not arrival times.

Results appear in the status file and, with `ACTIVITY_LOG=1`, per
measurement in the kernel journal:

```text
tdc: start=block0 stop=block1 count=42 last=1000000013ns last_cycles=38400005 min=-999999961ns max=1000000039ns mean=12ns lost=0 pending=0
activity=tdc_measure start_art=... stop_art=... cycles=... duration_ns=...
```

Clear the statistics with `echo 1 > /sys/kernel/debug/tgpio/tdc_reset`.

## Disciplining The PHC

Two validated ways to steer the TGPIO PHC to an external reference, both
measured with a logic analyzer against the atomic-clock PPS of an OCP
TimeCard.

### Hardware-domain loop (recommended)

Wire the reference PPS into a TGPIO input block and let `ts2phc` discipline
the PHC from that block's own ART-domain external timestamps while the other
block generates output:

```sh
sudo make reload TGPIO0=input EDGE0=rising TGPIO1=output OUTPUT1_PERIOD_NS=1000000000
sudo ts2phc -m -s generic -c /dev/ptpX --ts2phc.pin_index 0
```

Capture and generation share the ART timebase, so no PCIe clock comparison
is in the loop: the uncalibrated systematic offset is only about 50 ns (the
capture-versus-generate pipeline difference, roughly two ART cycles) and no
platform tuning is required. Measured output alignment: `-52 +/- 122 ns`
against a reference distribution whose own wire-to-wire noise floor on the
instrument was 60-80 ns. This is also the complete GPS PPS use case, with
any 1 PPS reference standing in for the receiver.

### PHC-to-PHC with phc2sys

```sh
sudo phc2sys -s /dev/ptp<master> -c /dev/ptpX -O 0 -R 8 -N 10 -m
```

This path reads both clocks across PCIe, which adds a constant read
asymmetry (about 2.5 us on the validated platform; cancel it with
`output_phase_offset_ns`) and requires pinning platform latency: hold
`/dev/cpu_dma_latency` at 0 and select the performance cpufreq governor,
otherwise the asymmetry wanders with power management and the calibration
goes stale. Measured output alignment after calibration: `+9 +/- 64 ns`.

In both cases: PTP device numbering can change across reboots, so resolve
devices with `cat /sys/class/ptp/ptp*/clock_name` ("Intel TGPIO" is this
driver). After a cold boot, check output polarity once and flip it with
`echo 1 > /sys/kernel/debug/tgpio/outputN_invert` if the waveform came up
half a period off.

## Output With testptp

For block 0 output:

```sh
sudo testptp -i 0 -L 0,2 -d /dev/ptpX
sudo testptp -i 0 -p 1000000000 -d /dev/ptpX
```

For block 1 output:

```sh
sudo testptp -i 1 -L 1,2 -d /dev/ptpX
sudo testptp -i 1 -p 1000000000 -d /dev/ptpX
```

`-L pin,2` assigns the pin to periodic output. `-p 1000000000` starts a
1-second period output. By default, the driver first primes the output low,
then arms TGPIO hardware periodic mode: `COMPV` holds the first active edge,
`PIV` holds the half-period in ART cycles, and `TGPIOCTL.PM` lets hardware
generate the steady toggle edges. The periodic interval is derived through the
kernel's ART base-clock conversion so the free-running hardware cadence follows
the same calibrated timebase used for absolute realtime edge programming. This
avoids reprogramming every transition from software and supports faster periodic
transitions than the hrtimer re-arm path can reliably sustain.

Modern `testptp` can set pulse width with `-w`, which maps to the Linux
`PTP_PEROUT_DUTY_CYCLE` request. For example, this generates a 1 ms period with
250 us on-time:

```sh
sudo testptp -i 0 -p 1000000 -w 250000 -d /dev/ptpX
```

The hardware periodic engine is fixed to 50% duty. When the requested on-time
is not exactly half the period, the driver automatically uses the software
re-arm path and programs explicit rising and falling edges. A duty cycle can
also be persisted at load time with `OUTPUT0_DUTY_NS` / `OUTPUT1_DUTY_NS`.

Know the timing-quality difference between the two paths on the validated
hardware:

- **50% duty, or any duty whose halves are both at least 50 ms**: edges are
  generated by the hardware toggle engine and quantize only to the 26 ns ART
  cycle. Asymmetric duty alternates the interval register between the
  on-time and off-time cycle counts, rewritten by a per-edge service inside
  each half (in-flight `PIV` rewrites latch for the following period), so
  every edge stays hardware-timed; the intervals are recomputed each edge,
  tracking PHC servo rate changes automatically. Validated: a 25% duty 1 s
  request measured 249.998 ms / 749.995 ms on the logic analyzer.
- **Halves shorter than 50 ms with non-50% duty (software path)**: the
  waveform comes out at the requested ratio, but the edges are produced by
  the per-edge register writes themselves — single-shot output compares
  never fire on this hardware — so each edge lands roughly a lead time
  (~20 ms) early and jitters with software scheduling. Treat this case as
  millisecond-quality signaling, not precision timing.

### One-Shot Timed Pulse

A single hardware-timed pulse can be fired on an idle output block through
debugfs:

```sh
echo "<start_ns> <width_ns>" | sudo tee /sys/kernel/debug/tgpio/oneshot1
echo "0 100000000" | sudo tee /sys/kernel/debug/tgpio/oneshot1   # 100 ms pulse, 200 ms from now
```

`start_ns` is an absolute PTP-clock time (0 means 200 ms from now) and both
edges are generated by the toggle engine at ART precision; a scheduled stop
lands in the following low half, which is why the width must be at least
50 ms. Validated: a requested 100 ms pulse measured 99.9990 ms with exactly
one rise and one fall. The block must not be running a periodic output
(`EBUSY` otherwise).

Use `HARDWARE_PERIODIC_OUTPUT=0` to return to the older software re-arm path:

```sh
sudo make reload TGPIO0=output HARDWARE_PERIODIC_OUTPUT=0
```

If your board or measurement path inverts the output, reload with
`OUTPUT_POLARITY=inverted`.

## Activity Log With journalctl

The driver can optionally log GPIO activity to the kernel journal. This is off
by default because input events and software output edges can be frequent.

Enable it at load time:

```sh
sudo make reload TGPIO0=output TGPIO1=input ACTIVITY_LOG=1
```

Or toggle it on a loaded module:

```sh
echo 1 | sudo tee /sys/module/tgpio_ptp_input/parameters/activity_log
```

Follow the activity stream:

```sh
sudo journalctl -k -f -g 'tgpio_ptp_input: activity='
```

Input lines include the block, PTP channel, edge selection, event counter,
event-counter delta, raw ART capture value, and emitted timestamp.

What the output path reports, and where to see it with journalctl:

- `output quantization ...` — unconditional (needs no option): one line per
  arm in the kernel log with the requested and actual period, half-period in
  ART cycles, `period_rounding_ns`, and `first_edge_rounding_ns`. See it with
  `sudo journalctl -k -g 'output quantization'`.
- `activity=output_arm` / `activity=output_hw_periodic` — arming details:
  requested period, high/on and low/off time, ART-cycle quantization,
  calculated full period, rounding/split error fields, and the armed first
  edge. In hardware periodic mode the hardware free-runs after arming, so the
  journal records the programmed setup rather than every physical edge; in
  software mode each programmed transition can be logged.
- `activity=output_phase_nudge` — a running output's pending compare was
  pulled back onto the period grid after PHC frequency updates, with the
  phase error that was corrected.
- `activity=output_phase_rearm` — the phase error exceeded a quarter period
  and the output was restarted on the grid (backstop path).
- `activity=output_late_push` — an arm ran late and the first edge was pushed
  forward by whole periods to protect polarity.
- `activity=output_stop` — the output was stopped.

### Verbose rounding mode

For the rounding of every individual output programming action — not just
arms — enable `verbose_rounding`:

```sh
sudo make reload TGPIO0=input TGPIO1=output OUTPUT1_PERIOD_NS=1000000000 VERBOSE_ROUNDING=1
```

Or toggle it on a loaded module:

```sh
echo 1 | sudo tee /sys/module/tgpio_ptp_input/parameters/verbose_rounding
```

Every periodic-interval refresh, phase nudge, and software-programmed edge
then writes one `output rounding` line with the requested value, the value
actually programmed (read back through the clock conversion), and the
difference:

```text
output rounding block=1 channel=1 event=interval_refresh piv_cycles=19199251 requested_half_ns=500000000 actual_half_ns=500000013 half_rounding_ns=13
output rounding block=1 channel=1 event=phase_nudge requested_edge_ns=... programmed_edge_ns=... edge_rounding_ns=-9
```

Follow it live with:

```sh
sudo journalctl -k -f -g 'output rounding'
```

Note that with a tool like `phc2sys -R 8` adjusting frequency eight times per
second, interval refreshes log at the same rate — this mode is for analysis
sessions, not steady-state operation.

For a quick frequency sanity check while an output is running:

```sh
sudo cat /sys/kernel/debug/tgpio/status
```

The output status shows `art_snapshot`, `high_time`, `low_time`, and the PHC
`base_art_hz`; for hardware periodic output it also includes
`art_half_cycles`, `actual_period`, `period_error`, and — while the block is
armed — `armed_piv` and the live `phase_error` against the requested grid.

Input blocks report live statistics on their status line: `phase` is the
distance of the last capture from the PTP clock's whole-second grid (for a
PPS reference this is the discipline quality, visible without any external
tool), and `interval_last/min/max/mean` track the spacing between
consecutive captures — the period of the signal with a single-edge
selection, or the alternating high/low widths with `EDGE=both`. Clear them
together with the TDC statistics via
`/sys/kernel/debug/tgpio/tdc_reset`.

Each input block also registers a standard RFC 2783 PPS source (named
`tgpioN`, typically `/dev/pps2`+), fed with the hardware capture times
converted to the realtime domain — so `ppstest`, NTPd, or chrony's PPS
refclock can consume TGPIO captures without any PTP awareness.

The PHC implements `PTP_SYS_OFFSET_PRECISE` (precise cross-timestamps with
an exact device/realtime pairing); `phc2sys` and chrony use it automatically,
removing read-jitter from clock comparisons. A watchdog checks running
outputs every 10 seconds and logs a warning if a compare value ever stops
advancing (a silently dead waveform).

With `AUTO_POLARITY=1` and the output looped back into the other block's
input, captured rising edges observe the output level directly: three
consecutive captures on the half-period slot trigger the same glitch-free
polarity flip as the debugfs knob. Note this requires loopback wiring —
with an external reference on the input instead, output polarity is
fundamentally unobservable at 50% duty (edge times cannot distinguish rise
from fall), so the option stays safely inert.

After tuning runtime knobs (calibration offset, polarity, TDC roles), persist
the whole current configuration with:

```sh
sudo make save-config
```

Independently of the opt-in activity log, every hardware periodic arm writes
one `output quantization` line to the kernel log (visible in `dmesg` /
`journalctl -k`) recording the rounding introduced by converting the request
onto integer ART cycles: the requested and actual period, the half-period in
ART cycles, `period_rounding_ns`, and `first_edge_rounding_ns` (the
programmed compare value read back through the clock conversion). At the
38.4 MHz ART rate one cycle is about 26 ns, so both figures stay within
±13 ns of the request.
When `art_snapshot` is `absent`, PHC mode derives the current ART value from
the `CLOCK_REALTIME` inversion, which is exact for the current instant even
while NTP disciplines the realtime clock.

## Persistent Install

Install for the running kernel:

```sh
sudo make install TGPIO0=output TGPIO1=input EDGE1=rising
```

`install` persists the module and its load-time options through
`/etc/modprobe.d/tgpio-ptp-input.conf` and
`/etc/modules-load.d/tgpio-ptp-input.conf`.

To also restore pin operation after reboot, pass the operation options when
installing. For example, this brings block 0 back as a 1 Hz output and block 1
back as an enabled rising-edge input whenever the module loads:

```sh
sudo make persist TGPIO0=output TGPIO1=input EDGE1=rising \
  OUTPUT0_PERIOD_NS=1000000000 INPUT1_ENABLE=1
```

The persisted operation options are:

- `INPUT0_ENABLE=1` or `INPUT1_ENABLE=1`: enable external timestamp capture at
  module load for an input-mode block.
- `INPUT0_CHANNEL` and `INPUT1_CHANNEL`: PTP external timestamp channels for
  those inputs; defaults are `0` and `1`.
- `OUTPUT0_PERIOD_NS` and `OUTPUT1_PERIOD_NS`: start periodic output at module
  load for an output-mode block; `0` disables restored output.
- `OUTPUT0_DUTY_NS` and `OUTPUT1_DUTY_NS`: optional output on/high time in
  nanoseconds for restored output. `0` means 50% duty. Non-50% duty uses the
  software re-arm path.
- `OUTPUT0_CHANNEL` and `OUTPUT1_CHANNEL`: PTP periodic-output channels for
  those outputs; defaults are `0` and `1`.
- `OUTPUT_START_DELAY_NS`: optional delay from current PTP time to the first
  restored output edge. With the default `0`, the driver chooses a safe start
  time shortly after module load.

The output period is restored after boot, but an exact absolute waveform phase
is not preserved across a restart.

Remove the persistent install:

```sh
sudo make uninstall
```

## Known working setup

The following is a known working setup with confirmed results:

ASUS ProArt Z890-CREATOR WIFI with the BIOS version 3202

link: https://www.asus.com/us/motherboards-components/motherboards/proart/proart-z890-creator-wifi/

Using "out of the box" Ubuntu 26.04 LTS (validated on the default Linux
kernels 7.0.0-22-generic and 7.0.0-27-generic).

Measured on this setup against an atomic-clock PPS reference (OCP TimeCard,
Saleae Logic Pro 16 at 50-100 MS/s): disciplined 1 PPS output within
+/-100 ns of the reference by either discipline path, with deterministic
polarity across module reloads, PHC steps, and phc2sys restarts.

## Safety

This module maps and exposes MMIO resources. Wrong addresses can write to the
wrong hardware registers. Use only address sets confirmed for your platform.

## Important Notes

Do not load this at the same time as the separate TGPIO Platform module
(a separate repo for output only).
This add-on owns the selected static blocks itself.

Hardware input timestamps use `TIMESTAMP_MODE=realtime` by default, which
converts captured ART cycles into `CLOCK_REALTIME` through the kernel
timekeeping clocksource relationship. Use `TIMESTAMP_MODE=art` for the old raw
ART-cycle-derived nanosecond scale. In that mode, `ART_FREQUENCY=0`
auto-detects the ART/crystal frequency from CPUID leaf `0x15`. If the CPU does
not report it, load with `ART_FREQUENCY=<Hz>` manually, for example
`ART_FREQUENCY=25000000`. Set `HARDWARE_TIMESTAMPS=0` to emit poll time instead
of hardware capture time while debugging.

In realtime mode, `art_frequency` and the TSC/ART ratio are reported for
diagnostics when CPUID exposes them, but realtime timestamp conversion uses the
kernel timekeeping ART base-clock relationship instead of the manual frequency
scale.

In PHC mode, the driver converts between ART cycles and adjusted PHC
nanoseconds with a stable `base_art_hz` captured when the PHC is created. This
keeps `phc2sys`/`ts2phc` frequency discipline from feeding back through
`CLOCK_REALTIME`. Current PHC reads use ART cycles from the kernel timekeeping
snapshot, not an inverse conversion from realtime. A non-zero
`ART_FREQUENCY=<Hz>` overrides the captured base rate.

In realtime mode, hardware periodic output does not use the CPUID-reported
`art_frequency` for the free-running interval. It converts the requested period
through the kernel's ART base-clock relationship, avoiding drift from a nominal
crystal value that differs from the calibrated system timebase.

In PHC mode, hardware periodic output uses the PHC `base_art_hz` plus the
current `adjfine` frequency scale. PHC steps re-prime the output; PHC frequency
changes refresh the hardware interval without restarting the waveform.

The CPUID `0x15` ratio is the TSC-to-ART ratio:
`TSC_Hz = ART_Hz * tsc_art_numerator / tsc_art_denominator`. It is useful for
diagnostics or for deriving the CPU TSC rate, but it is not applied to the
TGPIO hardware timestamp conversion because the TGPIO compare/capture values
are treated as ART-domain cycles.

## License

Non-commercial use only. The Software is free to use, modify, and
redistribute for personal, academic, research, teaching, evaluation, and
hobby purposes. **Any commercial use requires the prior written permission
of Ahmad Byagowi** (ahmadexp@gmail.com), who reserves exclusively for
himself the right to commercialize it. See [LICENSE](LICENSE) for the
full terms. The kernel module declares `MODULE_LICENSE("Dual BSD/GPL")`
solely to satisfy the kernel's licensed-symbol requirements when built and
loaded by end users; versions published before this license change remain
under their original terms.

## Public Sources

This repository is intended to contain only original project files and public
Linux/kernel interface information. See [PUBLIC_SOURCES.md](PUBLIC_SOURCES.md)
for public references.
