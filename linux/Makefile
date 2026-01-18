# ==========================================
#  Ploytec Xone:DB4 Linux Driver Makefile
# ==========================================

# Module Name
MODULE_NAME := snd-usb-xonedb4

# Kernel Build Directory (defaults to current running kernel)
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# ------------------------------------------
#  Kbuild Definitions
# ------------------------------------------

# The target module
obj-m := $(MODULE_NAME).o

# Source files: Local driver files + Common library
# Note: We link ../common/ploytec.o relative to this directory
$(MODULE_NAME)-y := chip.o pcm.o midi.o ../common/ploytec.o

# ------------------------------------------
#  Targets
# ------------------------------------------

all:
	@echo "Building $(MODULE_NAME)..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	@echo "Cleaning..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

# Standard install (installs to /lib/modules/$(uname -r)/extra/)
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: all clean modules_install