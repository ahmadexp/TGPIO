#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
MODULE_FILE="tgpio-ptp-input.ko"
MODULE_PATH="${ROOT_DIR}/src/${MODULE_FILE}"
MODULE_NAME="tgpio-ptp-input"
KREL=$(uname -r)
DEST_DIR="/lib/modules/${KREL}/extra"
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

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo $0" >&2
	exit 1
fi

if [ ! -f "${MODULE_PATH}" ]; then
	echo "${MODULE_PATH} not found; run make first" >&2
	exit 1
fi

install -d "${DEST_DIR}"
install -m 0644 "${MODULE_PATH}" "${DEST_DIR}/${MODULE_FILE}"
depmod "${KREL}"

{
	printf 'options %s' "${MODULE_NAME}"
	printf ' addr0=%s' "${ADDR0}"
	printf ' addr1=%s' "${ADDR1}"
	printf ' mmio_size=%s' "${MMIO_SIZE}"
	printf ' use_second=%s' "${USE_SECOND}"
	printf ' tgpio0=%s' "${TGPIO0}"
	printf ' tgpio1=%s' "${TGPIO1}"
	printf ' edge0=%s' "${EDGE0}"
	printf ' edge1=%s' "${EDGE1}"
	printf ' clock_mode=%s' "${CLOCK_MODE}"
	printf ' timestamp_mode=%s' "${TIMESTAMP_MODE}"
	printf ' output_polarity=%s' "${OUTPUT_POLARITY}"
	printf ' poll_ms=%s' "${POLL_MS}"
	printf ' art_frequency=%s' "${ART_FREQUENCY}"
	printf ' hardware_timestamps=%s' "${HARDWARE_TIMESTAMPS}"
	printf ' hardware_periodic_output=%s' "${HARDWARE_PERIODIC_OUTPUT}"
	printf ' activity_log=%s' "${ACTIVITY_LOG}"
	printf ' input0_enable=%s' "${INPUT0_ENABLE}"
	printf ' input1_enable=%s' "${INPUT1_ENABLE}"
	printf ' input0_channel=%s' "${INPUT0_CHANNEL}"
	printf ' input1_channel=%s' "${INPUT1_CHANNEL}"
	printf ' output0_channel=%s' "${OUTPUT0_CHANNEL}"
	printf ' output1_channel=%s' "${OUTPUT1_CHANNEL}"
	printf ' output0_period_ns=%s' "${OUTPUT0_PERIOD_NS}"
	printf ' output1_period_ns=%s' "${OUTPUT1_PERIOD_NS}"
	printf ' output0_duty_ns=%s' "${OUTPUT0_DUTY_NS}"
	printf ' output1_duty_ns=%s' "${OUTPUT1_DUTY_NS}"
	printf ' output_start_delay_ns=%s\n' "${OUTPUT_START_DELAY_NS}"
} >/etc/modprobe.d/tgpio-ptp-input.conf

cat >/etc/modules-load.d/tgpio-ptp-input.conf <<EOF
${MODULE_NAME}
EOF

modprobe "${MODULE_NAME}" || modprobe tgpio_ptp_input
echo "Installed ${MODULE_NAME} for ${KREL}"
