# TGPIO

Author: Ahmad Byagowi

Experimental out-of-tree Linux kernel module that exposes Intel Time-Aware
GPIO / Timed I/O blocks on systems where firmware does not enumerate the ACPI
devices.

Each static TGPIO block can be selected independently as:

- `input`: PTP external timestamp input.
- `output`: PTP periodic output.
- `off`: leave the block unused.

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
edge0=both
edge1=both
poll_ms=10
art_frequency=25000000
```

Mixed-mode examples:

```sh
sudo make reload MODE0=output MODE1=input
sudo make reload MODE0=input MODE1=output
sudo make reload MODE0=output MODE1=off
sudo make reload MODE0=output MODE1=input EDGE1=rising
```

`reload` unloads the add-on and reloads it with the selected modes and input
edge defaults.

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

By default, input timestamps are captured on both rising and falling edges.
Choose one edge at load time with `EDGE0` or `EDGE1`:

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

## Important Notes

Do not load this at the same time as the separate TGPIO Platform output module.
This add-on owns the selected static blocks itself.

Hardware input timestamps are converted from captured ART cycles using
`ART_FREQUENCY`. If the value is wrong, timestamps will still count events but
their nanosecond scale will be wrong. Set `HARDWARE_TIMESTAMPS=0` to emit poll
time instead of hardware capture time while debugging.

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
