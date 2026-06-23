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
- GPS receiver model:
- GPS PPS electrical interface/level:

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

## Adjustable PHC Experiment

Configuration:

- Input block:
- PTP device:
- Input signal:
- ART frequency:
- CPUID TSC/ART ratio:
- linuxptp version:

Load command:

```sh
sudo make reload CLOCK_MODE=phc MODE0=input MODE1=off EDGE0=rising
```

Basic PHC checks:

```sh
sudo testptp -d /dev/ptpX -g
sudo testptp -d /dev/ptpX -s
sudo testptp -d /dev/ptpX -a 1000000
sudo testptp -d /dev/ptpX -f 1000
```

ts2phc command:

```sh
sudo ts2phc -f TODO-ts2phc.conf -m
```

Artifacts:

- `paper/data/TODO-phc-testptp.log`
- `paper/data/TODO-ts2phc.log`

Metrics:

- PHC set/read behavior:
- PHC step response:
- PHC frequency adjustment response:
- External timestamp event continuity:
- ts2phc offset before convergence:
- ts2phc offset after convergence:

## GPS PPS Minimal-Hardware Experiment

Configuration:

- GPS receiver model:
- GPS antenna/location:
- PPS electrical level:
- TGPIO input block:
- PTP device:
- System clock tool:
- linuxptp version:

Load command:

```sh
sudo make reload CLOCK_MODE=phc MODE0=input MODE1=off EDGE0=rising
```

External timestamp setup:

```sh
sudo testptp -i 0 -L 0,1 -d /dev/ptpX
```

ts2phc command:

```sh
sudo ts2phc -f TODO-gps-tgpio-ts2phc.conf -m
```

System-time command:

```sh
sudo phc2sys -s /dev/ptpX -c CLOCK_REALTIME -m
```

Artifacts:

- `paper/data/TODO-gps-testptp.log`
- `paper/data/TODO-gps-ts2phc.log`
- `paper/data/TODO-gps-phc2sys.log`

Metrics:

- PPS event continuity:
- ts2phc convergence time:
- TGPIO PHC offset after convergence:
- CLOCK_REALTIME offset after phc2sys convergence:
- CPU load:
- Extra timing hardware used:

## TSN OS Participation Experiment

Configuration:

- Timing source:
- TGPIO PHC:
- OS clock:
- TSN stack/tools:
- Network interface:
- NIC hardware timestamping support:
- Traffic scheduling mechanism:

Timing setup:

```sh
sudo ts2phc -f TODO-gps-tgpio-ts2phc.conf -m
sudo phc2sys -s /dev/ptpX -c CLOCK_REALTIME -m
```

Artifacts:

- `paper/data/TODO-tsn-phc2sys.log`
- `paper/data/TODO-tsn-os-clock.log`
- `paper/data/TODO-tsn-application.log`

Metrics:

- OS clock offset relative to TGPIO PHC:
- OS clock offset relative to GPS PPS:
- Host-side timestamp error:
- TSN application timing error:
- NIC-specific limitations:
