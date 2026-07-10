KDIR ?= /lib/modules/$(shell uname -r)/build
SRC_DIR := $(CURDIR)/src
SUDO ?= sudo
.DEFAULT_GOAL := all

ADDR0 ?= 0xFE001210
ADDR1 ?= 0xFE001310
MMIO_SIZE ?= 0x38
USE_SECOND ?= 1
TGPIO0 ?= input
TGPIO1 ?= input
EDGE0 ?= rising
EDGE1 ?= rising
CLOCK_MODE ?= phc
TIMESTAMP_MODE ?= realtime
OUTPUT_POLARITY ?= normal
POLL_MS ?= 10
ART_FREQUENCY ?= 0
HARDWARE_TIMESTAMPS ?= 1
HARDWARE_PERIODIC_OUTPUT ?= 1
ACTIVITY_LOG ?= 0
VERBOSE_ROUNDING ?= 0
VERBOSE ?= 0
TDC ?= 0
TDC_START ?= 0
AUTO_POLARITY ?= 0
INPUT0_ENABLE ?= 0
INPUT1_ENABLE ?= 0
INPUT0_CHANNEL ?= 0
INPUT1_CHANNEL ?= 1
OUTPUT0_CHANNEL ?= 0
OUTPUT1_CHANNEL ?= 1
OUTPUT0_PERIOD_NS ?= 0
OUTPUT1_PERIOD_NS ?= 0
OUTPUT0_DUTY_NS ?= 0
OUTPUT1_DUTY_NS ?= 0
OUTPUT_START_DELAY_NS ?= 0
OUTPUT_PHASE_OFFSET_NS ?= 0

LOAD_ENV := ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)"
LOAD_ENV += USE_SECOND="$(USE_SECOND)" TGPIO0="$(TGPIO0)" TGPIO1="$(TGPIO1)"
LOAD_ENV += EDGE0="$(EDGE0)" EDGE1="$(EDGE1)"
LOAD_ENV += CLOCK_MODE="$(CLOCK_MODE)"
LOAD_ENV += TIMESTAMP_MODE="$(TIMESTAMP_MODE)"
LOAD_ENV += OUTPUT_POLARITY="$(OUTPUT_POLARITY)" POLL_MS="$(POLL_MS)"
LOAD_ENV += ART_FREQUENCY="$(ART_FREQUENCY)"
LOAD_ENV += HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)"
LOAD_ENV += HARDWARE_PERIODIC_OUTPUT="$(HARDWARE_PERIODIC_OUTPUT)"
LOAD_ENV += ACTIVITY_LOG="$(ACTIVITY_LOG)" VERBOSE_ROUNDING="$(VERBOSE_ROUNDING)" VERBOSE="$(VERBOSE)"
LOAD_ENV += TDC="$(TDC)" TDC_START="$(TDC_START)"
LOAD_ENV += AUTO_POLARITY="$(AUTO_POLARITY)"
LOAD_ENV += INPUT0_ENABLE="$(INPUT0_ENABLE)" INPUT1_ENABLE="$(INPUT1_ENABLE)"
LOAD_ENV += INPUT0_CHANNEL="$(INPUT0_CHANNEL)" INPUT1_CHANNEL="$(INPUT1_CHANNEL)"
LOAD_ENV += OUTPUT0_CHANNEL="$(OUTPUT0_CHANNEL)" OUTPUT1_CHANNEL="$(OUTPUT1_CHANNEL)"
LOAD_ENV += OUTPUT0_PERIOD_NS="$(OUTPUT0_PERIOD_NS)"
LOAD_ENV += OUTPUT1_PERIOD_NS="$(OUTPUT1_PERIOD_NS)"
LOAD_ENV += OUTPUT0_DUTY_NS="$(OUTPUT0_DUTY_NS)"
LOAD_ENV += OUTPUT1_DUTY_NS="$(OUTPUT1_DUTY_NS)"
LOAD_ENV += OUTPUT_START_DELAY_NS="$(OUTPUT_START_DELAY_NS)"
LOAD_ENV += OUTPUT_PHASE_OFFSET_NS="$(OUTPUT_PHASE_OFFSET_NS)"

.PHONY: all clean help load reload unload status install persist uninstall save-config tui

help:
	@echo "TGPIO PTP driver -- build, load, and configuration"
	@echo
	@echo "Targets:"
	@echo "  make                 Build tgpio-ptp-input.ko"
	@echo "  make load            Build and load with the options below"
	@echo "  make reload          Unload, then load with the options below"
	@echo "  make unload          Unload the module"
	@echo "  make status          Show module, PTP device, and debugfs status"
	@echo "  make install         Install persistently for the running kernel"
	@echo "  make persist         Alias for install, including persisted operations"
	@echo "  make uninstall       Remove the persistent install"
	@echo "  make save-config     Persist the current runtime configuration"
	@echo "  make tui             Launch the TGPIO Control Center TUI (sudo)"
	@echo "  make clean           Clean the build"
	@echo "  make help            This text"
	@echo
	@echo "Block functions (what each TGPIO block does):"
	@echo "  TGPIO0=$(TGPIO0) TGPIO1=$(TGPIO1)"
	@echo "                       input  = PTP external timestamp capture"
	@echo "                       output = PTP periodic output"
	@echo "                       off    = leave the block unused"
	@echo "  EDGE0=$(EDGE0) EDGE1=$(EDGE1)"
	@echo "                       Input capture edge: rising | falling | both"
	@echo
	@echo "Clock timebase:"
	@echo "  CLOCK_MODE=$(CLOCK_MODE)"
	@echo "                       phc      = adjustable ART-backed PHC (default);"
	@echo "                                  disciplined by ts2phc/phc2sys"
	@echo "                       art      = same clock model anchored to"
	@echo "                                  CLOCK_MONOTONIC_RAW; NOT adjustable"
	@echo "                       realtime = PTP clock is CLOCK_REALTIME; PTP"
	@echo "                                  adjustments steer the system clock"
	@echo "  TIMESTAMP_MODE=$(TIMESTAMP_MODE)"
	@echo "                       Input event domain in realtime clock mode:"
	@echo "                       realtime | art (raw ART nanoseconds, debug)"
	@echo "  ART_FREQUENCY=$(ART_FREQUENCY)"
	@echo "                       0 = auto/calibrated; set Hz only to override"
	@echo
	@echo "Output generation:"
	@echo "  OUTPUT0_PERIOD_NS=$(OUTPUT0_PERIOD_NS) OUTPUT1_PERIOD_NS=$(OUTPUT1_PERIOD_NS)"
	@echo "                       Non-zero starts that output at load (1 PPS ="
	@echo "                       1000000000). Otherwise use testptp/PyPTM."
	@echo "  OUTPUT0_DUTY_NS=$(OUTPUT0_DUTY_NS) OUTPUT1_DUTY_NS=$(OUTPUT1_DUTY_NS)"
	@echo "                       On-time per period. 50% duty, and any duty"
	@echo "                       whose halves are both >= 50 ms, run on the"
	@echo "                       ns-precise hardware engine; shorter halves"
	@echo "                       fall back to the ms-class software path"
	@echo "  OUTPUT_PHASE_OFFSET_NS=$(OUTPUT_PHASE_OFFSET_NS)"
	@echo "                       Calibration shift of every output edge in ns"
	@echo "                       (runtime-writable via /sys/module parameters)"
	@echo "  OUTPUT_START_DELAY_NS=$(OUTPUT_START_DELAY_NS)"
	@echo "                       Delay from load to the first persisted edge"
	@echo "  OUTPUT_POLARITY=$(OUTPUT_POLARITY)      normal | inverted"
	@echo "  HARDWARE_PERIODIC_OUTPUT=$(HARDWARE_PERIODIC_OUTPUT)"
	@echo "                       1 = hardware free-running toggle engine;"
	@echo "                       0 = legacy per-edge software re-arm"
	@echo
	@echo "Input capture:"
	@echo "  INPUT0_ENABLE=$(INPUT0_ENABLE) INPUT1_ENABLE=$(INPUT1_ENABLE)"
	@echo "                       1 = start capturing at load (persisted input)"
	@echo "  INPUT0_CHANNEL=$(INPUT0_CHANNEL) INPUT1_CHANNEL=$(INPUT1_CHANNEL)"
	@echo "                       PTP channel numbers for persisted inputs"
	@echo "  OUTPUT0_CHANNEL=$(OUTPUT0_CHANNEL) OUTPUT1_CHANNEL=$(OUTPUT1_CHANNEL)"
	@echo "                       PTP channel numbers for persisted outputs"
	@echo "  POLL_MS=$(POLL_MS)            Input capture poll interval"
	@echo "  HARDWARE_TIMESTAMPS=$(HARDWARE_TIMESTAMPS)  1 = hardware capture values;"
	@echo "                       0 = poll-time timestamps (debug only)"
	@echo
	@echo "Platform addresses (only change for a different board):"
	@echo "  ADDR0=$(ADDR0) ADDR1=$(ADDR1)"
	@echo "  MMIO_SIZE=$(MMIO_SIZE) USE_SECOND=$(USE_SECOND)"
	@echo
	@echo "Diagnostics (all runtime-writable under /sys/module/tgpio_ptp_input/parameters/):"
	@echo "  ACTIVITY_LOG=$(ACTIVITY_LOG)       1 = log input/output activity to the kernel"
	@echo "                       journal (journalctl -k -g 'activity=')"
	@echo "  VERBOSE_ROUNDING=$(VERBOSE_ROUNDING)   1 = log the ART-cycle rounding of every"
	@echo "                       output programming action"
	@echo "  VERBOSE=$(VERBOSE)            1 = log detail lines for EVERY feature:"
	@echo "                       input events/statistics, TDC pairing, PPS"
	@echo "                       asserts, crosststamps, clock adjustments,"
	@echo "                       output programming/rounding, one-shot, duty,"
	@echo "                       auto-polarity, watchdog. Superset of the two"
	@echo "                       above (journalctl -k -g 'verbose=')"
	@echo
	@echo "TDC (time-to-digital converter):"
	@echo "  TDC=$(TDC)                1 = pair block 0 (start) and block 1 (stop)"
	@echo "                       input captures into hardware-timestamped"
	@echo "                       bipolar durations: signed stop - start,"
	@echo "                       negative when stop precedes start"
	@echo "                       (~26 ns resolution, 64-bit range)."
	@echo "                       Requires TGPIO0=input TGPIO1=input; edges"
	@echo "                       via EDGE0/EDGE1. Stats in the status file;"
	@echo "                       clear with /sys/kernel/debug/tgpio/tdc_reset"
	@echo "  TDC_START=$(TDC_START)          Which block is Start (0 or 1); the other"
	@echo "                       is Stop. Runtime-writable via /sys/module"
	@echo "  AUTO_POLARITY=$(AUTO_POLARITY)      1 = with the output looped back into the"
	@echo "                       other block's input, flip inverted output"
	@echo "                       polarity automatically"
	@echo
	@echo "Runtime controls while loaded:"
	@echo "  sudo cat /sys/kernel/debug/tgpio/status"
	@echo "  echo 1 > /sys/kernel/debug/tgpio/outputN_invert   (flip polarity)"
	@echo "  echo <ns> > /sys/module/tgpio_ptp_input/parameters/output_phase_offset_ns"
	@echo "  echo '<start_ns> <width_ns>' > /sys/kernel/debug/tgpio/oneshotN"
	@echo "                       Fire one hardware-timed pulse (start 0 = now"
	@echo "                       +200ms; width >= 50 ms)"
	@echo
	@echo "Examples:"
	@echo "  sudo make reload TGPIO0=input TGPIO1=output OUTPUT1_PERIOD_NS=1000000000"
	@echo "      Reference PPS into block 0, 1 PPS out of block 1; discipline with:"
	@echo "      sudo ts2phc -m -s generic -c /dev/ptpX --ts2phc.pin_index 0"
	@echo "  sudo make reload TGPIO0=output TGPIO1=output OUTPUT0_PERIOD_NS=1000000000 OUTPUT1_PERIOD_NS=1000000000"
	@echo "      Two 1 PPS outputs; discipline via phc2sys from another PHC"
	@echo "  sudo make reload CLOCK_MODE=art TGPIO1=output OUTPUT1_PERIOD_NS=1000000000"
	@echo "      Free-running raw-timebase output nothing can steer"
	@echo "  sudo make reload CLOCK_MODE=realtime TGPIO0=input"
	@echo "      System-clock discipline from a captured PPS (chrony refclock:"
	@echo "      refclock PHC /dev/ptpX:extpps:pin=0 poll 2 refid TPPS)"
	@echo "  sudo make reload TGPIO0=output OUTPUT0_PERIOD_NS=1000000 OUTPUT0_DUTY_NS=250000"
	@echo "      1 ms period, 250 us on-time (software path)"
	@echo "  sudo make reload TDC=1 TGPIO0=input TGPIO1=input EDGE0=rising EDGE1=rising"
	@echo "      Measure start-to-stop durations between the two pins"
	@echo "  sudo make reload TDC=1 TDC_START=1 TGPIO0=input TGPIO1=input"
	@echo "      Same, with block 1 as Start and block 0 as Stop"
	@echo
	@echo "Full documentation: README.md"

all:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) clean

load: all
	$(SUDO) $(LOAD_ENV) ./scripts/load.sh

reload: all
	-$(SUDO) ./scripts/unload.sh
	$(SUDO) $(LOAD_ENV) ./scripts/load.sh

unload:
	$(SUDO) ./scripts/unload.sh

status:
	./scripts/status.sh

install: all
	$(SUDO) $(LOAD_ENV) ./scripts/install.sh

persist: install

uninstall:
	$(SUDO) ./scripts/uninstall.sh

save-config:
	$(SUDO) ./scripts/save-config.sh

tui:
	$(SUDO) ./tools/tgpio-tui/tgpio-tui
