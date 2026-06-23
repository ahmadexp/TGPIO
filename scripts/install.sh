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
MODE0="${MODE0:-input}"
MODE1="${MODE1:-input}"
EDGE0="${EDGE0:-rising}"
EDGE1="${EDGE1:-rising}"
TIMESTAMP_MODE="${TIMESTAMP_MODE:-realtime}"
OUTPUT_POLARITY="${OUTPUT_POLARITY:-normal}"
POLL_MS="${POLL_MS:-10}"
ART_FREQUENCY="${ART_FREQUENCY:-0}"
HARDWARE_TIMESTAMPS="${HARDWARE_TIMESTAMPS:-1}"

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

cat >/etc/modprobe.d/tgpio-ptp-input.conf <<EOF
options ${MODULE_NAME} addr0=${ADDR0} addr1=${ADDR1} mmio_size=${MMIO_SIZE} use_second=${USE_SECOND} mode0=${MODE0} mode1=${MODE1} edge0=${EDGE0} edge1=${EDGE1} timestamp_mode=${TIMESTAMP_MODE} output_polarity=${OUTPUT_POLARITY} poll_ms=${POLL_MS} art_frequency=${ART_FREQUENCY} hardware_timestamps=${HARDWARE_TIMESTAMPS}
EOF

cat >/etc/modules-load.d/tgpio-ptp-input.conf <<EOF
${MODULE_NAME}
EOF

modprobe "${MODULE_NAME}" || modprobe tgpio_ptp_input
echo "Installed ${MODULE_NAME} for ${KREL}"
