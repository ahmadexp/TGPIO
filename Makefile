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
EDGE0 ?= both
EDGE1 ?= both
POLL_MS ?= 10
ART_FREQUENCY ?= 0
HARDWARE_TIMESTAMPS ?= 1

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
	@echo "  POLL_MS=$(POLL_MS)"
	@echo "  ART_FREQUENCY=$(ART_FREQUENCY)"
	@echo "  HARDWARE_TIMESTAMPS=$(HARDWARE_TIMESTAMPS)"

all:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) clean

load: all
	$(SUDO) ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)" USE_SECOND="$(USE_SECOND)" MODE0="$(MODE0)" MODE1="$(MODE1)" EDGE0="$(EDGE0)" EDGE1="$(EDGE1)" POLL_MS="$(POLL_MS)" ART_FREQUENCY="$(ART_FREQUENCY)" HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)" ./scripts/load.sh

reload: all
	-$(SUDO) ./scripts/unload.sh
	$(SUDO) ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)" USE_SECOND="$(USE_SECOND)" MODE0="$(MODE0)" MODE1="$(MODE1)" EDGE0="$(EDGE0)" EDGE1="$(EDGE1)" POLL_MS="$(POLL_MS)" ART_FREQUENCY="$(ART_FREQUENCY)" HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)" ./scripts/load.sh

unload:
	$(SUDO) ./scripts/unload.sh

status:
	./scripts/status.sh

install: all
	$(SUDO) ADDR0="$(ADDR0)" ADDR1="$(ADDR1)" MMIO_SIZE="$(MMIO_SIZE)" USE_SECOND="$(USE_SECOND)" MODE0="$(MODE0)" MODE1="$(MODE1)" EDGE0="$(EDGE0)" EDGE1="$(EDGE1)" POLL_MS="$(POLL_MS)" ART_FREQUENCY="$(ART_FREQUENCY)" HARDWARE_TIMESTAMPS="$(HARDWARE_TIMESTAMPS)" ./scripts/install.sh

uninstall:
	$(SUDO) ./scripts/uninstall.sh
