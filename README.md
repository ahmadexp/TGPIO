# TGPIO

Author: Ahmad Byagowi

Linux kernel module that exposes Time-Aware GPIO / Timed I/O blocks on systems.
It also works where system's firmware does not enumerate the ACPI devices.

Each static TGPIO block can be selected independently as:

- `off`: leave the block unused (0).
- `input`: PTP external timestamp input (1).
- `output`: PTP periodic output (2).

The default is both blocks as inputs, matching the first working input setup.

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
timestamp_mode=realtime
poll_ms=10
art_frequency=0
```

`art_frequency=0` means auto-detect the ART/crystal frequency from CPUID leaf
`0x15`. Set `ART_FREQUENCY=<Hz>` to override it manually.

CPUID leaf `0x15` also reports the TSC/ART ratio. The driver records it as
`tsc_art_numerator` and `tsc_art_denominator` and shows it in `make status`.

`timestamp_mode=realtime` reports hardware input captures in the same
`CLOCK_REALTIME` timebase returned by the PTP clock. Use
`TIMESTAMP_MODE=art` to report raw ART-cycle-derived nanoseconds instead.

Mixed-mode examples:

```sh
sudo make reload MODE0=output MODE1=input
sudo make reload MODE0=input MODE1=output
sudo make reload MODE0=output MODE1=off
sudo make reload MODE0=output MODE1=input EDGE1=rising
sudo make reload MODE0=input TIMESTAMP_MODE=art
```

`reload` unloads the add-on and reloads it with the selected modes and input
edge defaults.

Before proceeding with Input and Output, make sure testptp is installed. If not, here is a repo:
https://github.com/Time-Appliances-Project/Incubation-Projects/tree/master/Software/testptp

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
1-second period output; the driver programs TGPIO toggle edges every half
period.

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
