
# This should not change.
BOARD_VARIANT := circuitdojo_feather_nrf9160_ns

# Note, this may be different on your machine. Set SERIAL_PORT accordingly.
SERIAL_PORT:= /dev/tty.SLAB_USBtoUART,baud=250000

# Default step is to runt the build recipe
default: build

.PHONY: build
build:
	@echo "Building $(BOARD_VARIANT)"
	@west build -b $(BOARD_VARIANT)

.PHONY: clean
clean:
	@echo "Building $(BOARD_VARIANT)"
	@west build -b $(BOARD_VARIANT) --pristine

.PHONY: flash
flash:
	@west flash
	@nrfjprog -r

.PHONY: config
config:
	@west build -t menuconfig

.PHONY: load
load: build
	@echo "Loading app_update.bin via $(SERIAL_PORT)"
	@mcumgr --conntype=serial --connstring $(SERIAL_PORT) image upload build/zephyr/app_update.bin
	@mcumgr --conntype=serial --connstring $(SERIAL_PORT) reset