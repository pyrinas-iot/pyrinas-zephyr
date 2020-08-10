
# Set this to your J-Link Serial
ifndef PROG_SERIAL
PROG_SERIAL := 960181154
$(info PROG_SERIAL not set. Using default of $(PROG_SERIAL))
endif

# Set this to the port you want to use with `make debug`
ifndef PROG_PORT
PROG_PORT := 19022
$(info PROG_PORT not set. Using default of $(PROG_PORT))
endif

# This should not change.
BOARD_VARIANT := circuitdojo_feather_nrf9160ns

# Note, this may be different on your machine. Set MCUMGR_SERIAL_PORT accordingly.
MCUMGR_SERIAL_PORT:= /dev/tty.SLAB_USBtoUART
MCUMGR_SERIAL_BAUD:= 115200

# Common Makefile shared for all Pyrinas projects
include ../../lib/Makefile.common