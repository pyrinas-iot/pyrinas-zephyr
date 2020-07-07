
# Set this to your J-Link Serial
ifndef PROG_SERIAL
PROG_SERIAL := 682978319
$(info PROG_SERIAL not set. Using default of $(PROG_SERIAL))
endif

# Set this to the port you want to use with `make debug`
ifndef PROG_PORT
PROG_PORT := 19021
$(info PROG_PORT not set. Using default of $(PROG_PORT))
endif

# This should not change.
BOARD_VARIANT := particle_xenon

# Note, this may be different on your machine. Set MCUMGR_SERIAL_PORT accordingly.
MCUMGR_SERIAL_PORT:= /dev/tty.SLAB_USBtoUART

# Common Makefile shared for all Pyrinas projects
include ../../lib/Makefile.common