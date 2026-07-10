#!/bin/sh
# Persist the currently loaded module configuration (including runtime
# changes like a calibrated output_phase_offset_ns or a flipped tdc_start)
# into the modprobe options file, so the tuned setup survives reboots.
set -eu

PARAMS=/sys/module/tgpio_ptp_input/parameters
CONF=/etc/modprobe.d/tgpio-ptp-input.conf

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo $0" >&2
	exit 1
fi
if [ ! -d "${PARAMS}" ]; then
	echo "tgpio-ptp-input is not loaded" >&2
	exit 1
fi

line="options tgpio-ptp-input"
for f in "${PARAMS}"/*; do
	name=$(basename "${f}")
	value=$(cat "${f}")
	case "${value}" in
	Y) value=1 ;;
	N) value=0 ;;
	esac
	line="${line} ${name}=${value}"
done

printf '%s\n' "${line}" >"${CONF}"
echo "saved to ${CONF}:"
echo "  ${line}"
