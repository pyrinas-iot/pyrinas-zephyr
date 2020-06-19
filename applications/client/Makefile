
# Check for errors. These should be set in the app.
ifndef PROG_SERIAL
PROG_SERIAL := 682978319
$(info PROG_SERIAL not set. Using default of $(PROG_SERIAL))
endif

ifndef PROG_PORT
PROG_PORT := 19021
$(info PROG_PORT not set. Using default of $(PROG_PORT))
endif

# This should not change.
BOARD_VARIANT := particle_xenon

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
	@echo "Cleaning.."
	@west build -b $(BOARD_VARIANT) -p

.PHONY: flash
flash: build
	# @west flash -r nrfjprog
	nrfjprog --program build/zephyr/app_signed.hex --sectorerase -s $(PROG_SERIAL)
	nrfjprog -r -s $(PROG_SERIAL)

.PHONY: config
config:
	@west build -t menuconfig

.PHONY: load
load: build
	@echo "Loading app_update.bin via $(SERIAL_PORT)"
	@mcumgr --conntype=serial --connstring $(SERIAL_PORT) image upload build/zephyr/app_update.bin
	@mcumgr --conntype=serial --connstring $(SERIAL_PORT) reset

.PHONY: debug
debug:
	@echo Debug using JLinkExe
	JLinkExe -device NRF52 -speed 4000 -if SWD -autoconnect 1 -SelectEmuBySN $(PROG_SERIAL) -RTTTelnetPort $(PROG_PORT)

.PHONY: rtt
rtt:
	jlinkrttclient -RTTTelnetPort $(PROG_PORT)