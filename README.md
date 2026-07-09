# TGPIO

Author: Ahmad Byagowi

Linux kernel module that exposes Time-Aware GPIO / Timed I/O blocks on systems.
It also works where system's firmware does not enumerate the ACPI devices.

Each static TGPIO block can be selected independently as:

- `off`: leave the block unused (0).
- `input`: PTP external timestamp input (1).
- `output`: PTP periodic output (2).

The default is both blocks as inputs, matching the first working input setup.

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
mode0=input
mode1=input
edge0=rising
edge1=rising
clock_mode=phc
timestamp_mode=realtime
output_polarity=normal
poll_ms=10
art_frequency=0
hardware_periodic_output=Y
```

`art_frequency=0` means auto-detect the ART/crystal frequency from CPUID leaf
`0x15` when PHC mode, hardware periodic output, or raw ART timestamp mode needs
it. Set `ART_FREQUENCY=<Hz>` to override it manually.

CPUID leaf `0x15` also reports the TSC/ART ratio. The driver records it as
`tsc_art_numerator` and `tsc_art_denominator` and shows it in `make status`.

`clock_mode=phc` is the default and exposes an adjustable ART-backed PHC. In
PHC mode the driver implements `gettime64`, `settime64`, `adjtime`, and
`adjfine`, reports a non-zero `max_adj`, and emits hardware external timestamp
events in the adjusted PHC time domain. This is the mode to use with tools that
discipline a PHC from external timestamps, such as `ts2phc`.

Use `CLOCK_MODE=realtime` to keep the PTP clock tied directly to Linux
`CLOCK_REALTIME`. In realtime clock mode, `TIMESTAMP_MODE=realtime` reports
hardware input captures in the same `CLOCK_REALTIME` timebase returned by the
PTP clock. Use `TIMESTAMP_MODE=art` to report raw ART-cycle-derived nanoseconds
instead.

Mixed-mode examples:

```sh
sudo make reload MODE0=output MODE1=input
sudo make reload MODE0=input MODE1=output
sudo make reload MODE0=output MODE1=off
sudo make reload MODE0=output MODE1=input EDGE1=rising
sudo make reload MODE0=input TIMESTAMP_MODE=art
sudo make reload MODE0=output OUTPUT_POLARITY=inverted
sudo make reload MODE0=output HARDWARE_PERIODIC_OUTPUT=0
sudo make reload MODE0=input MODE1=off
sudo make reload CLOCK_MODE=realtime MODE0=input MODE1=off
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
sudo make reload MODE0=output MODE1=input EDGE1=rising
sudo make reload MODE0=output MODE1=input EDGE1=falling
sudo make reload MODE0=output MODE1=input EDGE1=both
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
sudo make reload MODE0=input MODE1=off EDGE0=rising
```

In PHC mode:

- `testptp -g` reads the adjusted TGPIO PHC time.
- `testptp -s` and linuxptp adjustment calls can set or adjust the PHC.
- External timestamp events are reported in the adjusted PHC time domain.
- Periodic output requests are interpreted in the adjusted PHC time domain and
  converted back to ART compare values before programming hardware.
- If a tool such as `phc2sys` steps the TGPIO PHC while periodic output is
  active, the driver re-primes and re-arms the output in the adjusted PHC
  domain so a stale compare value does not stop the waveform.

`TIMESTAMP_MODE=art` is intended for explicit `CLOCK_MODE=realtime` debugging.
In the default PHC mode, hardware input events are emitted in adjusted PHC time
so that PHC tools see a consistent clock domain.

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
generate the steady toggle edges. This avoids reprogramming every transition
from software and supports faster periodic transitions than the hrtimer re-arm
path can reliably sustain.

Use `HARDWARE_PERIODIC_OUTPUT=0` to return to the older software re-arm path:

```sh
sudo make reload MODE0=output HARDWARE_PERIODIC_OUTPUT=0
```

If your board or measurement path inverts the output, reload with
`OUTPUT_POLARITY=inverted`.

## Persistent Install

Install for the running kernel:

```sh
sudo make install MODE0=output MODE1=input EDGE1=rising
```

Remove the persistent install:

```sh
sudo make uninstall
```

## Known working setup

The following is a known working setup with confirmed results:

ASUS ProArt Z890-CREATOR WIFI with the BIOS version 3202

link: https://www.asus.com/us/motherboards-components/motherboards/proart/proart-z890-creator-wifi/

Using "out of the box" Ubuntu 26.04 LTS (that comes with the default Linux kernel 7.0.0-22-generic)

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

In PHC mode, `art_frequency` is required so the driver can convert between ART
cycles and adjusted PHC nanoseconds. `ART_FREQUENCY=0` still means auto-detect
from CPUID leaf `0x15` when the CPU reports it.

Hardware periodic output also requires `art_frequency`, because the TGPIO
periodic interval register is programmed in ART cycles. If CPUID does not
report the crystal frequency, set `ART_FREQUENCY=<Hz>` manually or load with
`HARDWARE_PERIODIC_OUTPUT=0` to use the software re-arm fallback.

The CPUID `0x15` ratio is the TSC-to-ART ratio:
`TSC_Hz = ART_Hz * tsc_art_numerator / tsc_art_denominator`. It is useful for
diagnostics or for deriving the CPU TSC rate, but it is not applied to the
TGPIO hardware timestamp conversion because the TGPIO compare/capture values
are treated as ART-domain cycles.

## License

BSD 2-Clause for the project. The kernel module source is marked
`BSD-2-Clause OR GPL-2.0-only` and declares `MODULE_LICENSE("Dual BSD/GPL")`
for Linux kernel module compatibility. Use, modify, redistribute, and
commercialize it freely while retaining Ahmad Byagowi's copyright notice and
license terms.

## Public Sources

This repository is intended to contain only original project files and public
Linux/kernel interface information. See [PUBLIC_SOURCES.md](PUBLIC_SOURCES.md)
for public references.
