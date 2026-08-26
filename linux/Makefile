# ==========================================
#  Ozzy USB Audio Driver Makefile
# ==========================================

# Module Name
MODULE_NAME := snd-usb-ozzy

# Kernel Build Directory (defaults to current running kernel)
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# ------------------------------------------
#  Kbuild Definitions
# ------------------------------------------

# The target module
obj-m := $(MODULE_NAME).o

# Source files: Core + PCM + MIDI + Device families + Shared codecs
$(MODULE_NAME)-y := ozzy_core.o \
                    ozzy_pcm.o \
                    ozzy_midi.o \
                    devices/ploytec.o \
                    ../common/devices/ploytec/ploytec_codec.o

# To add a new device family, append:
#   devices/newdevice.o
#   ../common/devices/newdevice/newdevice_codec.o  (if shared codec)

# ------------------------------------------
#  Targets
# ------------------------------------------

all:
	@echo "Building $(MODULE_NAME)..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	@echo "Cleaning..."
	@if [ -f "$(KERNELDIR)/Makefile" ]; then \
		$(MAKE) -C "$(KERNELDIR)" M="$(PWD)" clean; \
	else \
		echo "Kernel build directory unavailable; removing known local build artifacts."; \
		rm -f $(MODULE_NAME).ko $(MODULE_NAME).o $(MODULE_NAME).mod \
			$(MODULE_NAME).mod.c $(MODULE_NAME).mod.o \
			ozzy_core.o ozzy_pcm.o ozzy_midi.o devices/ploytec.o \
			Module.symvers modules.order .modules.order.cmd \
			.Module.symvers.cmd .$(MODULE_NAME).ko.cmd \
			.$(MODULE_NAME).o.cmd .$(MODULE_NAME).mod.cmd \
			.$(MODULE_NAME).mod.o.cmd .$(MODULE_NAME).mod.c.cmd \
			.ozzy_core.o.cmd .ozzy_pcm.o.cmd .ozzy_midi.o.cmd \
			devices/.ploytec.o.cmd; \
		rm -rf .tmp_versions; \
	fi; \
	rm -f ../common/devices/ploytec/ploytec_codec.o \
		../common/devices/ploytec/.ploytec_codec.o.cmd

# Reproducible CI entrypoint. KERNELDIR must identify the headers under test.
ci:
	@test -f "$(KERNELDIR)/Makefile" || { \
		echo "KERNELDIR does not contain prepared kernel headers: $(KERNELDIR)" >&2; \
		exit 2; \
	}
	$(MAKE) clean KERNELDIR="$(KERNELDIR)"
	$(MAKE) all KERNELDIR="$(KERNELDIR)" W=1

# Fast, non-mutating source checks suitable for local use and CI.
check:
	@git -C "$(PWD)/.." diff --check

# Standard install (installs to /lib/modules/$(uname -r)/extra/)
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: all clean ci check modules_install
