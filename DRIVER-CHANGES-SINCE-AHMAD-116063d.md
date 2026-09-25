# TGPIO driver checkpoint: changes since Ahmad's last commit

Checkpoint date: 2026-09-24

A companion system-level progress record covers the phc2sys/controller, FPGA,
reciprocal PPS, and low-bandwidth startup work that is intentionally out of
scope for this driver-only comparison.

## Comparison baseline and scope

The baseline for this inventory is commit
`116063dff2329ca5d6e0244662b4cbe3429b7ddc` (`Add files via upload`), authored
by Ahmad Byagowi on 2026-07-26. It is the most recent commit authored by Ahmad
in the repository's reachable history. That commit changed only paper files;
the driver source itself is identical to its parent at that point.

The comparison target is the current working tree on branch
`monotonic-dither-extrapolation-fix`, whose committed `HEAD` is
`b0b8e9383f73d5d5550f83581453d9d978d516af`, plus the uncommitted driver work
present at this checkpoint.

This document covers driver behavior, driver build/load/status plumbing, and
the directly related documentation. It deliberately excludes unrelated paper
editing after the baseline.

The exact comparison can be reproduced with:

```text
git diff 116063dff2329ca5d6e0244662b4cbe3429b7ddc -- \
    src/tgpio-ptp-input.c src/tgpio-trace.h Makefile README.md \
    scripts/load.sh scripts/status.sh .gitignore
```

At this checkpoint, only the software-rearm runt-pulse fix is already in a
commit after the baseline (`198258c`). The remaining functional items below
are present in the working tree and still need to be committed.

## Executive summary

The most consequential correction changes how a future `CLOCK_REALTIME` edge
is converted into an ART compare value. The prior path inverted the requested
future realtime using one instantaneous Linux timekeeper multiplier state.
Linux alternates adjacent integer multiplier values to realize a fractional
average frequency correction. Sampling one side of that dither and
extrapolating it roughly half a second could therefore move the programmed
edge by several ART cycles.

The new path anchors ART and realtime at the current instant, measures their
realized average rate over multiple seconds, and projects from that coherent
anchor. Other changes eliminate a software-output runt pulse, correct a
two-cycle domain mismatch in phase-error calculation, reduce the phase-nudge
dead-band to the hardware's actual one-ART-cycle resolution, and expose the
independent measurements needed to calculate retrospective Qerr.

## Functional changes

### 1. Average-rate realtime-to-ART projection

**Status:** validated on the current Z890/Core Ultra test system; primary bug
fix; uncommitted at this checkpoint.

**Previous behavior:** a future `CLOCK_REALTIME` timestamp was passed directly
to `ktime_real_to_base_clock(..., CSID_X86_ART, ...)`. This uses the
timekeeper's multiplier state visible at that instant. When the kernel dithers
between adjacent integer multipliers to synthesize the requested fractional
frequency correction, the selected instantaneous state is not necessarily the
realized average slope. Extrapolating that instantaneous slope to a future PPS
edge amplified the small slope discrepancy into a multi-cycle phase error.

**Current behavior:** realtime mode now:

1. takes a coherent current ART/realtime anchor;
2. measures the realized `CLOCK_REALTIME`/ART ratio with an eight-entry sliding
   ring and a minimum four-second window;
3. preserves that ratio as two integers instead of rounding it to integer ART
   cycles per second; and
4. converts a future or past delta with that measured average ratio.

The rate pair is protected by a seqlock because it is read from output and
phase-maintenance paths while refinement work updates it.

**Rationale:** PPS generation needs the average clock trajectory over the
prediction interval, not one instantaneous phase of the Linux multiplier
dither. Retaining the full integer ratio also preserves fractional rate
information smaller than one ART cycle per second.

**Safety behavior:** realtime output is not armed until an average-rate sample
is available. Rate acquisition begins immediately when the module loads, and
pending output requests can be retried after acquisition.

### 2. Coherent ART/realtime sampling and clock-step rejection

**Status:** validated as part of the realtime-projection and Qerr experiments;
uncommitted.

ART and realtime are now obtained from the same
`system_time_snapshot` whenever the kernel supplies ART as the base clock.
When that is unavailable, the fallback brackets the ART read with two realtime
snapshots and chooses the tightest valid bracket.

Samples spanning a change in `clock_was_set_seq` are rejected. The average-rate
ring is restarted after a realtime step or nonmonotonic observation.

**Rationale:** separately reading ART and realtime makes software latency look
like clock phase error. A realtime step invalidates a slope calculation across
the step even though neither clock's local rate is necessarily bad.

### 3. Diagnostic switch for the former instantaneous projection

**Status:** experimental A/B control; not intended as the production default;
uncommitted.

The read-only module parameter
`experimental_instantaneous_realtime_projection=1` restores the earlier direct
future-time inversion for controlled comparisons. Its default is `0`, which
selects the corrected average-rate path. `scripts/load.sh` exposes the same
setting through `EXPERIMENTAL_INSTANTANEOUS_REALTIME_PROJECTION`.

**Rationale:** retaining a deliberate negative control makes the root-cause
hypothesis falsifiable and allows future platforms or kernel versions to be
compared without reverting source code.

### 4. COMPV-domain phase-error correction

**Status:** code-reviewed and exercised by the phase-nudge tests; uncommitted.

The periodic phase-maintenance code previously compared a target value already
expressed in the hardware COMPV domain with `COMPV + 2`, which is the estimated
physical-edge ART value. The revised calculation compares the target COMPV
with the actual COMPV register value.

**Rationale:** `tgpio_clock_ns_to_compare_art()` has already subtracted the
two-cycle compare-to-pin delay. Adding those two cycles again to only one side
mixed the comparator and physical-edge domains and biased the phase error by
two ART cycles. The two-cycle value is still used where a physical edge ART
estimate is actually required, such as Qerr reconstruction.

### 5. One-ART-cycle phase-nudge dead-band

**Status:** implemented, built, and activated on the running system;
uncommitted.

`output_phase_tolerance_ns` now defaults to `0`, with zero defined to mean the
dynamically calculated duration of one ART cycle. A positive value still
requests a wider dead-band, but the driver never tries to correct less than one
ART cycle. The comparison uses an open interval, so an error equal to one full
cycle is correctable rather than being deferred until a second cycle is
accumulated.

The one-cycle duration comes from the measured realtime/ART ratio, falling
back to the configured ART frequency while rate acquisition is incomplete. It
is not hard-coded to the approximately 26 ns value of the current platform.

**Rationale:** the comparator can move an edge by one ART cycle. Waiting for a
200 ns error accumulated roughly eight cycles and then produced the observed
single-period negative/positive phase excursion. Correcting at one cycle
minimizes the steady-state sawtooth without requesting an unrealizable
sub-cycle adjustment.

The default was changed consistently in the source, top-level Makefile,
loader, help text, and README.

### 6. Stable-toggle software rearm to eliminate runt pulses

**Status:** committed as `198258c`; tested on both TGPIO outputs on the current
platform; portability to other hardware should still be regression-tested.

When `HARDWARE_PERIODIC_OUTPUT=0`, the older software path rewrote the EP
rising/falling selector for each edge. On the tested platform that register
rewrite produced a spurious pulse approximately two ART cycles wide.

The new default `software_rearm_toggle=1` establishes a known-low state once,
then keeps EP in toggle mode and writes only the next future COMPV. In toggle
mode COMPV is written before the control register. Setting the parameter to
`0` preserves the legacy behavior for diagnosis.

`Makefile`, `scripts/load.sh`, `scripts/status.sh`, and the README expose and
describe the option.

**Rationale:** avoiding a selector transition while the engine is running
prevents the observed hardware glitch without suppressing any intentional
rising edge.

### 7. Read-only Qerr snapshot interface

**Status:** experimental measurement interface used successfully by the Qerr
campaign; uncommitted.

The new debugfs file `/sys/kernel/debug/tgpio/qerr_snapshot` returns one CSV
record containing:

```text
art_before,realtime_before_ns,clock_step_seq_before,
art_after,realtime_after_ns,clock_step_seq_after,COMPV,PIV,CTL
```

The ART/realtime observations bracket the MMIO reads. If the kernel snapshot
does not directly carry ART cycles, each snapshot's own realtime value is
inverted separately rather than extrapolating a future value. Reading the file
does not alter output state or scheduling. The current implementation reports
block 0 only and requires that block to be configured as an output.

**Rationale:** after an edge, userspace can reconstruct realtime at the
physical edge estimate (`COMPV + 2`) and calculate:

```text
Qerr = reconstructed edge realtime - requested edge realtime
```

This correction is independent of the external FPGA or NIC capture timestamp;
the external capture is used only to test whether the predicted correction
removes the observed error.

### 8. More specific software-edge trace labels

**Status:** diagnostic improvement; uncommitted.

The rounding trace now distinguishes `software_prime`, `software_rising`, and
`software_falling` rather than labeling all events `software_edge`.

**Rationale:** startup artifacts, deliberate edges, and polarity errors can be
separated during trace analysis without inferring event type from timing.

## Portability and repository-hygiene changes

### Platform-neutral ART wording

Comments and help text no longer assert that one ART cycle is approximately
26 ns or that one ART hertz has a platform-specific ppb meaning. They describe
resolution as one ART cycle, and runtime calculations derive its duration from
the actual measured or configured rate.

**Rationale:** those numerical relationships are true for the current system's
ART frequency, not for every platform on which the driver might be used.

### Output-polarity wording

The EP comment now states that the encoding is inverted on the hardware the
driver supports and explains that `output_polarity` accommodates a board with
the opposite external polarity. It no longer presents one measured board
configuration as a universal physical property.

### SPDX identifiers

`src/tgpio-ptp-input.c` and `src/tgpio-trace.h` now carry
`SPDX-License-Identifier: LicenseRef-TGPIO-Non-Commercial` while retaining the
existing human-readable license notice.

**Rationale:** make automated license scanning less ambiguous without changing
the repository's license terms.

### Generated-file exclusions

`.gitignore` now excludes kernel-module build products, the loopback helper
binary, Python bytecode, editor state, and local Secure Boot material.

**Rationale:** keep generated objects, machine-local credentials, and temporary
experiment artifacts out of commits. This has no runtime effect.

## Files changed relative to the baseline

| File | Relevant change |
|---|---|
| `src/tgpio-ptp-input.c` | Average-rate projection, coherent sampling, phase-domain fix, dynamic nudge floor, Qerr endpoint, trace labels, software-toggle fix, parameter descriptions |
| `src/tgpio-trace.h` | SPDX/license-header cleanup |
| `Makefile` | New/revised parameters, one-cycle default, help text, loopback build target |
| `scripts/load.sh` | Pass software-toggle and experimental-projection parameters; use one-cycle phase policy by default |
| `scripts/status.sh` | Report the software-rearm-toggle parameter |
| `README.md` | Document software-toggle behavior and one-cycle phase policy |
| `.gitignore` | Ignore generated, editor, bytecode, and local signing artifacts |

## Validation performed by this checkpoint

- The module builds successfully against Ubuntu kernel headers
  `7.0.0-31-generic` with GCC 15.2.0.
- The built module metadata reports the new `output_phase_tolerance_ns`
  semantics.
- The live writable parameter was changed from `200` to `0` without restarting
  either PPS output or the active phc2sys loop.
- FPGA captures demonstrated the former approximately 200 ns phase corrections
  and motivated the one-cycle policy.
- The stable-toggle rearm was observed to remove the software-path runt pulse
  on both tested TGPIO outputs.
- Qerr arithmetic tests cover positive and negative correction signs,
  integer-first large-epoch interpolation, bracket validation, and required
  subtraction direction.
- The Qerr experiments showed that independently reconstructed mapping error
  tracks the physical capture error; detailed statistics and plots live in the
  companion experiment repository.

## Known limitations and follow-up work

- Cross-platform validation is still required. In particular, EP encoding,
  compare-to-pin latency, stable-toggle behavior, and whether ART appears
  directly in `system_time_snapshot` may differ across systems.
- The two-ART-cycle compare-to-pin constant remains a separate static-bias
  question. It must not be silently tuned from the same data used to evaluate
  Qerr.
- `qerr_snapshot` currently exposes only block 0 and does not itself assign a
  rising-edge sequence number. A production sidecar should also log compare
  rewrites with generation, edge slot, old COMPV, and new COMPV.
- The new one-cycle nudge policy should receive a longer FPGA regression run to
  quantify its steady-state distribution and confirm that increased rewrite
  frequency introduces no unexpected pulse-width or polarity artifact.
- The experimental instantaneous-projection parameter should remain clearly
  marked as a negative-control mechanism rather than a recommended operating
  mode.
- The post-Ahmad changes should be split into reviewable commits: the already
  committed software-toggle fix, the realtime/ART projection correction, the
  COMPV/nudge correction, the Qerr instrumentation, and documentation/hygiene.
