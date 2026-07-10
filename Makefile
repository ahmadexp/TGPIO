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

LOAD_ENV := ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)"
LOAD_ENV += USE_SECOND="$(USE_SECOND)" TGPIO0="$(TGPIO0)" TGPIO1="$(TGPIO1)"
LOAD_ENV += EDGE0="$(EDGE0)" EDGE1="$(EDGE1)"
LOAD_ENV += CLOCK_MODE="$(CLOCK_MODE)"
LOAD_ENV += TIMESTAMP_MODE="$(TIMESTAMP_MODE)"
LOAD_ENV += OUTPUT_POLARITY="$(OUTPUT_POLARITY)" POLL_MS="$(POLL_MS)"
LOAD_ENV += ART_FREQUENCY="$(ART_FREQUENCY)"
LOAD_ENV += HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)"
LOAD_ENV += HARDWARE_PERIODIC_OUTPUT="$(HARDWARE_PERIODIC_OUTPUT)"
LOAD_ENV += ACTIVITY_LOG="$(ACTIVITY_LOG)" VERBOSE_ROUNDING="$(VERBOSE_ROUNDING)"
LOAD_ENV += INPUT0_ENABLE="$(INPUT0_ENABLE)" INPUT1_ENABLE="$(INPUT1_ENABLE)"
LOAD_ENV += INPUT0_CHANNEL="$(INPUT0_CHANNEL)" INPUT1_CHANNEL="$(INPUT1_CHANNEL)"
LOAD_ENV += OUTPUT0_CHANNEL="$(OUTPUT0_CHANNEL)" OUTPUT1_CHANNEL="$(OUTPUT1_CHANNEL)"
LOAD_ENV += OUTPUT0_PERIOD_NS="$(OUTPUT0_PERIOD_NS)"
LOAD_ENV += OUTPUT1_PERIOD_NS="$(OUTPUT1_PERIOD_NS)"
LOAD_ENV += OUTPUT0_DUTY_NS="$(OUTPUT0_DUTY_NS)"
LOAD_ENV += OUTPUT1_DUTY_NS="$(OUTPUT1_DUTY_NS)"
LOAD_ENV += OUTPUT_START_DELAY_NS="$(OUTPUT_START_DELAY_NS)"

.PHONY: all clean help load reload unload status install persist uninstall

help:
	@echo "Targets:"
	@echo "  make                 Build tgpio-ptp-input.ko"
	@echo "  make load            Load the TGPIO add-on"
	@echo "  make reload          Unload and load with the current options"
	@echo "  make unload          Unload the TGPIO add-on"
	@echo "  make status          Show module and PTP status"
	@echo "  make install         Install persistently for the running kernel"
	@echo "  make persist         Alias for install, including persisted operations"
	@echo "  make uninstall       Remove persistent install"
	@echo
	@echo "Common overrides:"
	@echo "  ADDR0=$(ADDR0)"
	@echo "  ADDR1=$(ADDR1)"
	@echo "  MMIO_SIZE=$(MMIO_SIZE)"
	@echo "  USE_SECOND=$(USE_SECOND)"
	@echo "  TGPIO0=$(TGPIO0)"
	@echo "  TGPIO1=$(TGPIO1)"
	@echo "  EDGE0=$(EDGE0)"
	@echo "  EDGE1=$(EDGE1)"
	@echo "  CLOCK_MODE=$(CLOCK_MODE)"
	@echo "  TIMESTAMP_MODE=$(TIMESTAMP_MODE)"
	@echo "  OUTPUT_POLARITY=$(OUTPUT_POLARITY)"
	@echo "  POLL_MS=$(POLL_MS)"
	@echo "  ART_FREQUENCY=$(ART_FREQUENCY)"
	@echo "  HARDWARE_TIMESTAMPS=$(HARDWARE_TIMESTAMPS)"
	@echo "  HARDWARE_PERIODIC_OUTPUT=$(HARDWARE_PERIODIC_OUTPUT)"
	@echo "  ACTIVITY_LOG=$(ACTIVITY_LOG)"
	@echo "  VERBOSE_ROUNDING=$(VERBOSE_ROUNDING)"
	@echo "  INPUT0_ENABLE=$(INPUT0_ENABLE)"
	@echo "  INPUT1_ENABLE=$(INPUT1_ENABLE)"
	@echo "  INPUT0_CHANNEL=$(INPUT0_CHANNEL)"
	@echo "  INPUT1_CHANNEL=$(INPUT1_CHANNEL)"
	@echo "  OUTPUT0_CHANNEL=$(OUTPUT0_CHANNEL)"
	@echo "  OUTPUT1_CHANNEL=$(OUTPUT1_CHANNEL)"
	@echo "  OUTPUT0_PERIOD_NS=$(OUTPUT0_PERIOD_NS)"
	@echo "  OUTPUT1_PERIOD_NS=$(OUTPUT1_PERIOD_NS)"
	@echo "  OUTPUT0_DUTY_NS=$(OUTPUT0_DUTY_NS)"
	@echo "  OUTPUT1_DUTY_NS=$(OUTPUT1_DUTY_NS)"
	@echo "  OUTPUT_START_DELAY_NS=$(OUTPUT_START_DELAY_NS)"

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
