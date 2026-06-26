#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
MODULE="${ROOT_DIR}/src/tgpio-ptp-input.ko"
ADDR0="${ADDR0:-0xFE001210}"
ADDR1="${ADDR1:-0xFE001310}"
MMIO_SIZE="${MMIO_SIZE:-0x38}"
USE_SECOND="${USE_SECOND:-1}"
MODE0="${MODE0:-input}"
MODE1="${MODE1:-input}"
EDGE0="${EDGE0:-rising}"
EDGE1="${EDGE1:-rising}"
CLOCK_MODE="${CLOCK_MODE:-phc}"
TIMESTAMP_MODE="${TIMESTAMP_MODE:-realtime}"
OUTPUT_POLARITY="${OUTPUT_POLARITY:-normal}"
POLL_MS="${POLL_MS:-10}"
ART_FREQUENCY="${ART_FREQUENCY:-0}"
HARDWARE_TIMESTAMPS="${HARDWARE_TIMESTAMPS:-1}"
HARDWARE_PERIODIC_OUTPUT="${HARDWARE_PERIODIC_OUTPUT:-1}"

tgpio_input_loaded()
{
	lsmod | awk '$1 == "tgpio_ptp_input" { found = 1 } END { exit !found }'
}

tgpio_platform_loaded()
{
	lsmod | awk '$1 == "tgpio_platform" { found = 1 } END { exit !found }'
}

tgpio_output_devices_exist()
{
	find /sys/bus/platform/devices -maxdepth 1 -name 'intel-pps-gen-tio*' 2>/dev/null |
		grep -q .
}

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo $0" >&2
	exit 1
fi

if [ ! -f "${MODULE}" ]; then
	echo "${MODULE} not found; run make first" >&2
	exit 1
fi

if tgpio_platform_loaded; then
	echo "Unloading legacy TGPIO-Platform module"
	rmmod tgpio_platform 2>/dev/null || {
		echo "TGPIO-Platform is loaded and could not be unloaded." >&2
		exit 1
	}
fi

if tgpio_input_loaded; then
	echo "tgpio_ptp_input is already loaded"
	"${SCRIPT_DIR}/status.sh"
	exit 0
fi

if tgpio_output_devices_exist; then
	echo "intel-pps-gen-tio platform devices already exist; unload their owner first." >&2
	exit 1
fi

insmod "${MODULE}" addr0="${ADDR0}" addr1="${ADDR1}" mmio_size="${MMIO_SIZE}" \
	use_second="${USE_SECOND}" mode0="${MODE0}" mode1="${MODE1}" \
	edge0="${EDGE0}" edge1="${EDGE1}" clock_mode="${CLOCK_MODE}" \
	timestamp_mode="${TIMESTAMP_MODE}" \
	output_polarity="${OUTPUT_POLARITY}" poll_ms="${POLL_MS}" \
	art_frequency="${ART_FREQUENCY}" hardware_timestamps="${HARDWARE_TIMESTAMPS}" \
	hardware_periodic_output="${HARDWARE_PERIODIC_OUTPUT}"

"${SCRIPT_DIR}/status.sh"
