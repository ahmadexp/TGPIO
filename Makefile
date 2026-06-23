KDIR ?= /lib/modules/$(shell uname -r)/build
SRC_DIR := $(CURDIR)/src
SUDO ?= sudo
.DEFAULT_GOAL := all

ADDR0 ?= 0xFE001210
ADDR1 ?= 0xFE001310
MMIO_SIZE ?= 0x38
USE_SECOND ?= 1
MODE0 ?= input
MODE1 ?= input
EDGE0 ?= rising
EDGE1 ?= rising
TIMESTAMP_MODE ?= realtime
OUTPUT_POLARITY ?= normal
POLL_MS ?= 10
ART_FREQUENCY ?= 0
HARDWARE_TIMESTAMPS ?= 1

LOAD_ENV := ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)"
LOAD_ENV += USE_SECOND="$(USE_SECOND)" MODE0="$(MODE0)" MODE1="$(MODE1)"
LOAD_ENV += EDGE0="$(EDGE0)" EDGE1="$(EDGE1)"
LOAD_ENV += TIMESTAMP_MODE="$(TIMESTAMP_MODE)"
LOAD_ENV += OUTPUT_POLARITY="$(OUTPUT_POLARITY)" POLL_MS="$(POLL_MS)"
LOAD_ENV += ART_FREQUENCY="$(ART_FREQUENCY)"
LOAD_ENV += HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)"

.PHONY: all clean help load reload unload status install uninstall

help:
	@echo "Targets:"
	@echo "  make                 Build tgpio-ptp-input.ko"
	@echo "  make load            Load the TGPIO add-on"
	@echo "  make reload          Unload and load with the current options"
	@echo "  make unload          Unload the TGPIO add-on"
	@echo "  make status          Show module and PTP status"
	@echo "  make install         Install persistently for the running kernel"
	@echo "  make uninstall       Remove persistent install"
	@echo
	@echo "Common overrides:"
	@echo "  ADDR0=$(ADDR0)"
	@echo "  ADDR1=$(ADDR1)"
	@echo "  MMIO_SIZE=$(MMIO_SIZE)"
	@echo "  USE_SECOND=$(USE_SECOND)"
	@echo "  MODE0=$(MODE0)"
	@echo "  MODE1=$(MODE1)"
	@echo "  EDGE0=$(EDGE0)"
	@echo "  EDGE1=$(EDGE1)"
	@echo "  TIMESTAMP_MODE=$(TIMESTAMP_MODE)"
	@echo "  OUTPUT_POLARITY=$(OUTPUT_POLARITY)"
	@echo "  POLL_MS=$(POLL_MS)"
	@echo "  ART_FREQUENCY=$(ART_FREQUENCY)"
	@echo "  HARDWARE_TIMESTAMPS=$(HARDWARE_TIMESTAMPS)"

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

uninstall:
	$(SUDO) ./scripts/uninstall.sh
