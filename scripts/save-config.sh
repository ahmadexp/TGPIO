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
	# Skip values the driver detects at load: persisting them would turn
	# auto-detection into a stale explicit override (art_frequency in
	# particular would replace the calibrated PHC base rate with the
	# nominal crystal value).
	case "${name}" in
	art_frequency | tsc_art_numerator | tsc_art_denominator)
		continue
		;;
	esac
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
