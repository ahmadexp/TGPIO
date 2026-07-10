#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
MODULE="${ROOT_DIR}/src/tgpio-ptp-input.ko"
ADDR0="${ADDR0:-0xFE001210}"
ADDR1="${ADDR1:-0xFE001310}"
MMIO_SIZE="${MMIO_SIZE:-0x38}"
USE_SECOND="${USE_SECOND:-1}"
TGPIO0="${TGPIO0:-input}"
TGPIO1="${TGPIO1:-input}"
EDGE0="${EDGE0:-rising}"
EDGE1="${EDGE1:-rising}"
CLOCK_MODE="${CLOCK_MODE:-phc}"
TIMESTAMP_MODE="${TIMESTAMP_MODE:-realtime}"
OUTPUT_POLARITY="${OUTPUT_POLARITY:-normal}"
POLL_MS="${POLL_MS:-10}"
ART_FREQUENCY="${ART_FREQUENCY:-0}"
HARDWARE_TIMESTAMPS="${HARDWARE_TIMESTAMPS:-1}"
HARDWARE_PERIODIC_OUTPUT="${HARDWARE_PERIODIC_OUTPUT:-1}"
ACTIVITY_LOG="${ACTIVITY_LOG:-0}"
VERBOSE_ROUNDING="${VERBOSE_ROUNDING:-0}"
INPUT0_ENABLE="${INPUT0_ENABLE:-0}"
INPUT1_ENABLE="${INPUT1_ENABLE:-0}"
INPUT0_CHANNEL="${INPUT0_CHANNEL:-0}"
INPUT1_CHANNEL="${INPUT1_CHANNEL:-1}"
OUTPUT0_CHANNEL="${OUTPUT0_CHANNEL:-0}"
OUTPUT1_CHANNEL="${OUTPUT1_CHANNEL:-1}"
OUTPUT0_PERIOD_NS="${OUTPUT0_PERIOD_NS:-0}"
OUTPUT1_PERIOD_NS="${OUTPUT1_PERIOD_NS:-0}"
OUTPUT0_DUTY_NS="${OUTPUT0_DUTY_NS:-0}"
OUTPUT1_DUTY_NS="${OUTPUT1_DUTY_NS:-0}"
OUTPUT_START_DELAY_NS="${OUTPUT_START_DELAY_NS:-0}"
OUTPUT_PHASE_OFFSET_NS="${OUTPUT_PHASE_OFFSET_NS:-0}"

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
	use_second="${USE_SECOND}" tgpio0="${TGPIO0}" tgpio1="${TGPIO1}" \
	edge0="${EDGE0}" edge1="${EDGE1}" clock_mode="${CLOCK_MODE}" \
	timestamp_mode="${TIMESTAMP_MODE}" \
	output_polarity="${OUTPUT_POLARITY}" poll_ms="${POLL_MS}" \
	art_frequency="${ART_FREQUENCY}" hardware_timestamps="${HARDWARE_TIMESTAMPS}" \
	hardware_periodic_output="${HARDWARE_PERIODIC_OUTPUT}" \
	activity_log="${ACTIVITY_LOG}" \
	verbose_rounding="${VERBOSE_ROUNDING}" \
	input0_enable="${INPUT0_ENABLE}" input1_enable="${INPUT1_ENABLE}" \
	input0_channel="${INPUT0_CHANNEL}" input1_channel="${INPUT1_CHANNEL}" \
	output0_channel="${OUTPUT0_CHANNEL}" output1_channel="${OUTPUT1_CHANNEL}" \
	output0_period_ns="${OUTPUT0_PERIOD_NS}" \
	output1_period_ns="${OUTPUT1_PERIOD_NS}" \
	output0_duty_ns="${OUTPUT0_DUTY_NS}" \
	output1_duty_ns="${OUTPUT1_DUTY_NS}" \
	output_start_delay_ns="${OUTPUT_START_DELAY_NS}" \
	output_phase_offset_ns="${OUTPUT_PHASE_OFFSET_NS}"

"${SCRIPT_DIR}/status.sh"
