# TGPIO ISPCS Measurement Log Template

Use this file to record the measurements that should become the evaluation
tables and figures in the paper.

## Platform

- Motherboard:
- BIOS version:
- CPU model:
- Kernel version:
- Distribution:
- TGPIO module commit:
- Oscilloscope or time interval counter:
- Signal generator or PPS source:

## Module Load

Command:

```sh
sudo make reload MODE0=output MODE1=input EDGE1=rising TIMESTAMP_MODE=realtime OUTPUT_POLARITY=normal
```

Record:

```text
make status output here
```

Record:

```text
dmesg | grep -i tgpio output here
```

## Input Timestamp Experiment

Configuration:

- Input block:
- PTP device:
- Input signal frequency:
- Input edge mode:
- Timestamp mode:
- Poll interval:

Commands:

```sh
sudo testptp -i 0 -L 1,1 -d /dev/ptpX
sudo testptp -i 0 -e 100 -d /dev/ptpX
```

Raw log path:

- `paper/data/TODO-input-testptp.log`

Metrics:

- Number of events:
- Missed event count:
- Mean interval:
- Minimum interval:
- Maximum interval:
- Standard deviation:

## Output Period Experiment

Configuration:

- Output block:
- PTP device:
- Requested period:
- Output polarity:
- Scope sample rate:

Commands:

```sh
sudo testptp -i 0 -L 0,2 -d /dev/ptpX
sudo testptp -i 0 -p 1000000000 -d /dev/ptpX
```

Artifacts:

- Startup screenshot:
- Steady-state screenshot:
- CSV capture:

Metrics:

- First active edge polarity:
- Mean period:
- Minimum period:
- Maximum period:
- Standard deviation:
- Startup success over repeated reloads:

## Realtime vs. ART Timestamp Experiment

Configuration:

- Input block:
- Input signal:
- ART frequency:
- CPUID TSC/ART ratio:

Realtime command:

```sh
sudo make reload MODE0=input TIMESTAMP_MODE=realtime
sudo testptp -i 0 -L 0,1 -d /dev/ptpX
sudo testptp -i 0 -e 20 -d /dev/ptpX
```

ART command:

```sh
sudo make reload MODE0=input TIMESTAMP_MODE=art
sudo testptp -i 0 -L 0,1 -d /dev/ptpX
sudo testptp -i 0 -e 20 -d /dev/ptpX
```

Notes:

- Realtime timestamps should align with the PTP clock epoch.
- ART timestamps should reflect raw ART-derived nanoseconds and should not be
  interpreted as Unix epoch time.
