# ISPCS 2026 Paper Plan: TGPIO PTP Driver

Working title:

> A Comprehensive Linux PTP Driver for Time-Aware GPIO

Alternative titles:

- Time-Aware GPIO as a Linux PTP Clock
- A Linux PTP Interface for Firmware-Undeclared Intel Time-Aware GPIO Blocks
- Practical Timed GPIO Capture and Generation Using ART-Domain Hardware Counters
- Recovering Platform Timed I/O Functionality Through a PTP-Centric Linux Driver

## Target Venue Fit

ISPCS 2026 asks for six-page full papers describing original research results,
technical contributions, or practical applications relevant to precision clock
synchronization and distributed time-based applications.

This work is strongest as a practical systems paper: it turns hidden/static
timed I/O hardware into a Linux PTP device, covers real clock-domain issues
around ART/TSC/CLOCK_REALTIME, and demonstrates both external timestamp input
and periodic output.

## One-Sentence Thesis

Firmware-undeclared timed I/O blocks can still be made useful to timing
applications by exposing them through the Linux PTP hardware-clock API, provided
the driver explicitly handles MMIO ownership, ART-domain timestamp conversion,
PTP pin routing, and output polarity/startup behavior.

## Draft Abstract

Clock synchronization protocols make time comparable across systems, but
measurement and control applications also need a way to attach that time to
physical signals. Precision Time Measurement (PTM) and Precision Time Protocol
(PTP) can provide timing relationships for host, endpoint, and network clocks;
Time-Aware GPIO (TGPIO) is important because it provides the complementary timed
I/O boundary, capturing external edges and generating scheduled output
transitions in the platform time domain. This paper presents a Linux driver that
exposes firmware-undeclared Intel TGPIO hardware through the standard PTP
hardware clock interface. The driver maps known MMIO resources, configures TGPIO
blocks for external timestamp input or periodic output, and integrates the
hardware's Always Running Timer (ART) timebase with Linux timekeeping. It
supports realtime, raw ART, and adjustable PHC clock modes, allowing
synchronized platform time to be observed, disciplined, and exercised at
external pins with existing PTP user-space tools. A motivating application is
connecting a GPS receiver's pulse-per-second output directly to a TGPIO input so
that GPS-derived time can be made available to the host with minimal additional
timing hardware. We describe the driver architecture, pin-function model,
timestamp conversion path, and output scheduling mechanism, along with practical
issues encountered during bring-up, including firmware discovery gaps, ART/TSC
ratio handling, input edge defaults, deterministic output startup, and output
polarity selection. The work demonstrates that TGPIO can turn an otherwise
internal PTM/PTP-aware timing domain into usable physical I/O for
synchronization, measurement, and control.

## Main Contributions

1. A PTP-clock abstraction for static TGPIO MMIO blocks that firmware does not
   enumerate.
2. Per-block mode selection: off, external timestamp input, or periodic output.
3. Hardware timestamp handling across ART, TSC/ART CPUID metadata, PHC time,
   and CLOCK_REALTIME-compatible PTP events.
4. Default ART-backed adjustable PHC mode for external-reference tools such as
   `ts2phc`.
5. Deterministic output startup with explicit polarity control.
6. A reproducible bring-up workflow using standard Linux kernel and PTP user
   APIs, including GPS PPS input as a target application.

## Proposed Six-Page Structure

### 1. Introduction

Motivation:

- Timed I/O is useful for measurement, control, instrumentation, and
  synchronization experiments.
- A GPS receiver's PPS output can provide an external time reference, but the
  host still needs hardware that can timestamp that edge in the platform time
  domain.
- Some hardware exists but is not exposed by firmware, making standard kernel
  drivers unavailable.
- Linux PTP APIs are a natural userspace boundary because they already model
  external timestamp and periodic output functions.

Claim:

- A small driver can bridge static hardware resources into the standard PTP
  ecosystem while preserving timebase correctness.
- In adjustable PHC mode, the same path can support a minimal-hardware GPS PPS
  timing setup: GPS PPS into TGPIO, `ts2phc` disciplining the TGPIO PHC, and
  system time made available through normal Linux time tooling.
- Once disciplined time is available through the OS, the host can participate in
  the timing domain of a Time-Sensitive Networking (TSN) system for host-side
  timestamping, scheduling, and control logic. Network transmit scheduling still
  depends on the NIC and TSN stack.

### 2. Background

Cover briefly:

- Linux PTP clock API: `PTP_PF_EXTTS`, `PTP_PF_PEROUT`, pin assignment,
  `testptp`.
- Intel ART/TSC relationship and CPUID leaf `0x15`.
- TGPIO register model used by this driver: control, compare, capture timestamp,
  event count.
- Problem with firmware enumeration gaps.

### 3. Driver Design

Core design points:

- Static module parameters for known MMIO base addresses and block count.
- Per-block mode parsing.
- PTP pin descriptors and channel-to-block mapping.
- Input polling reads latched capture timestamp and event counter.
- Output uses Linux realtime-to-ART conversion to program compare values.
- Default `clock_mode=phc` maintains an adjustable ART-backed PHC model for
  tools such as `ts2phc`.
- Safety boundaries: do not load with a separate platform driver owning the same
  hardware.

Suggested figure:

```
External signal -> TGPIO capture registers -> driver polling -> PTP event
                                                 |
                                                 v
                                    CLOCK_REALTIME / PHC / ART timestamp mode

PTP perout request -> PTP clock domain -> ART compare value -> TGPIO output
```

### 4. Clock-Domain Handling

Explain the important engineering story:

- Captured TGPIO values are ART-domain cycles.
- Default `clock_mode=phc` exposes an ART-backed adjustable PHC for tools such as
  `ts2phc`.
- `clock_mode=realtime` keeps the PTP clock tied to `CLOCK_REALTIME` when
  explicitly requested.
- `timestamp_mode=realtime` converts captures into `CLOCK_REALTIME` when
  `clock_mode=realtime` is explicitly requested.
- `timestamp_mode=art` preserves raw ART-derived nanoseconds for debugging or
  comparison.
- CPUID leaf `0x15` provides ART frequency and TSC/ART ratio when available.
- Output scheduling converts from the active PTP clock domain to ART.

Suggested table:

| Mode | Event timestamp exposed to PTP | Dependency |
| --- | --- | --- |
| `hardware_timestamps=0` | Poll time in `CLOCK_REALTIME` | Kernel realtime clock |
| `timestamp_mode=realtime` | Captured edge converted to `CLOCK_REALTIME` | ART base clock in Linux timekeeping |
| `timestamp_mode=art` | ART-cycle-derived nanoseconds | `art_frequency` |
| `clock_mode=phc` | Captured edge converted to adjusted PHC time | ART base clock and `art_frequency` |

### 5. Practical Bring-Up Issues

Use this section to make the paper valuable rather than just descriptive:

- Firmware did not enumerate the ACPI device, so static MMIO mapping was needed.
- The GPS PPS application needs only a PPS-capable receiver and direct TGPIO
  input path rather than a separate timing card or FPGA.
- Default edge selection changed from both edges to rising edge.
- CPUID frequency/ratio should be probed but not required for realtime mode.
- Output polarity on the confirmed platform required treating output compare
  polarity separately from input edge names.
- Output startup required a preconditioning step before the first active edge.

Suggested figure:

- Scope trace showing corrected output startup: preconditioning transition,
  first rising active edge, steady toggles.

### 6. Evaluation

Measurements to collect:

1. Build/load reproducibility:
   - Kernel version, motherboard, BIOS, CPU.
   - Module parameters shown by `make status`.

2. Input timestamp behavior:
   - Feed a PPS or known periodic edge into TGPIO.
   - Capture with `testptp -e`.
   - Report event count continuity and timestamp interval statistics.

3. Output behavior:
   - Generate 1 Hz output with `testptp -p 1000000000`.
   - Measure rising edge phase and period on an oscilloscope or time interval
     counter.
   - Compare `OUTPUT_POLARITY=normal` and `OUTPUT_POLARITY=inverted`.

4. Realtime versus raw ART mode:
   - Show how timestamps differ in epoch/scale.
   - Confirm `timestamp_mode=realtime` aligns with PTP `gettime64`.

5. Adjustable PHC mode:
   - Load with the default `CLOCK_MODE=phc`.
   - Use `testptp` to set, step, and frequency-adjust the PHC.
   - Use `ts2phc` with an external timestamp input to confirm the clock can be
     disciplined by linuxptp tooling.

6. GPS PPS minimal-hardware timing path:
   - Connect a GPS receiver PPS output directly to a TGPIO input.
   - Use the default PHC mode and `ts2phc` to discipline the TGPIO PHC.
   - Use normal Linux time tooling to make the disciplined time visible to the
     system clock.
   - Report whether stable host time is obtained without an additional timing
     NIC, FPGA, or timing card.

7. TSN host participation:
   - Use the GPS-disciplined TGPIO PHC as the host timing source.
   - Transfer the PHC time to the OS clock.
   - Demonstrate OS-visible time suitable for host-side TSN applications while
     documenting any NIC-specific TSN dependencies separately.

Candidate metrics:

- Mean period error.
- RMS jitter or standard deviation of interval error.
- Min/max interval error.
- Startup polarity correctness over repeated reloads.
- Missed event count under polling interval settings.
- PHC adjustment response to `adjtime` and `adjfine`.
- GPS PPS to PHC convergence and host clock offset.
- OS clock offset relative to the GPS-disciplined PHC for TSN participation.

### 7. Limitations and Future Work

Be candid:

- Static MMIO addresses must be known and platform-specific.
- Polling detects capture events; an interrupt-backed path would reduce latency
  and event-loss risk.
- The default PHC mode adjusts a software clock model layered on ART rather
  than disciplining the ART oscillator itself.
- This is an out-of-tree driver intended for platforms where firmware does not
  expose hardware.
- More platforms should be tested to validate output polarity defaults.
- Upstreaming would require platform discovery/quirk handling and maintainer
  review.

### 8. Conclusion

Close with the reusable lesson:

- Standard PTP interfaces can make hidden timed I/O hardware usable.
- Correct clock-domain handling and deterministic output startup are essential.
- The approach provides a practical bridge for measurement/control experiments
  on systems with otherwise inaccessible timed I/O blocks.

## Measurement Checklist

Record these before writing the final paper:

- Motherboard exact model and BIOS version.
- CPU model.
- Kernel version.
- `make status` output after load.
- `dmesg | grep -i tgpio` after load.
- Oscilloscope screenshots for output startup and steady-state period.
- Input capture logs from `testptp -e`.
- Output command lines and module parameter sets used for each plot/table.

## Likely References

- Linux PTP clock kernel API documentation.
- Linux timekeeping and cross-timestamp APIs.
- Intel documentation or public kernel discussions for ART/TSC CPUID leaf
  `0x15`.
- Linux `pps_gen_tio.c` as related upstream PPS/TIO output work.
- IEEE 802.1AS timing and synchronization for TSN applications.
- Public Linux kernel mailing-list discussion of Intel TGPIO support.
- The project repository and its public-source notes.

## Submission and Policy Notes

- ISPCS 2026 full papers are six pages and must use IEEE conference templates.
- AI-generated text must be disclosed in the acknowledgments if used in the
  submitted manuscript.
- Because Ahmad Byagowi is listed as a General Chair for ISPCS 2026, verify the
  conflict-of-interest and review-handling process with the Technical Program
  Chairs or steering process before submission.

## Next Drafting Step

Turn this plan into an IEEE-style six-page LaTeX draft with:

- A compact two-paragraph introduction.
- One architecture figure.
- One timestamp-mode table.
- One evaluation table.
- Two scope/testptp plots.
- A short acknowledgments/compliance note.
