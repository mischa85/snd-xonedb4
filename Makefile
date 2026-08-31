# ==========================================
#  Ozzy USB Audio Driver Makefile
# ==========================================
#
# kbuild on modern Linux (>= 5.x) does not reliably allow source files
# located outside the M= directory. The shared codec lives at
# ../common/devices/ploytec/ -- before each build we copy it into
# devices/, then build everything from inside this folder.
# `make clean` removes the staged copies.
#

MODULE_NAME := snd-usb-ozzy

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

SHARED_SRC_DIR := $(PWD)/../common/devices/ploytec
STAGED_FILES   := devices/ploytec_codec.c \
                  devices/ploytec_codec.h \
                  devices/ploytec_defs.h \
                  devices/ploytec_protocol.h

# ------------------------------------------
#  Kbuild Definitions
# ------------------------------------------

obj-m := $(MODULE_NAME).o

$(MODULE_NAME)-y := ozzy_core.o \
                    ozzy_pcm.o \
                    ozzy_midi.o \
                    devices/ploytec.o \
                    devices/ploytec_codec.o

# Make the staged headers visible at "../../common/devices/ploytec/<hdr>"
# (the include path used by ploytec.c) by also exposing them via the
# normal kbuild include directories.
ccflags-y := -I$(src) -I$(src)/devices

# ------------------------------------------
#  Targets
# ------------------------------------------

all: stage
	@echo "Building $(MODULE_NAME)..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# Copy shared codec sources into devices/ so kbuild can build them as
# regular in-tree objects. Safe to re-run -- always overwrites.
stage:
	@echo "Staging shared codec sources..."
	@cp -f $(SHARED_SRC_DIR)/ploytec_codec.c    devices/ploytec_codec.c
	@cp -f $(SHARED_SRC_DIR)/ploytec_codec.h    devices/ploytec_codec.h
	@cp -f $(SHARED_SRC_DIR)/ploytec_defs.h     devices/ploytec_defs.h
	@cp -f $(SHARED_SRC_DIR)/ploytec_protocol.h devices/ploytec_protocol.h

clean:
	@echo "Cleaning..."
	-$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	@rm -f $(STAGED_FILES)

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all stage clean modules_install
