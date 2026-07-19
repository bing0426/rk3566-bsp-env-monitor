KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?=
APP_CC ?= $(CROSS_COMPILE)gcc

.PHONY: all modules app clean help

all: modules app

modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

app:
	$(MAKE) -C app CC=$(APP_CC)

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
	$(MAKE) -C app clean

help:
	@echo "Build on board:"
	@echo "  make KERNEL_DIR=/lib/modules/`uname -r`/build ARCH=arm64"
	@echo ""
	@echo "Cross build example:"
	@echo "  make KERNEL_DIR=/path/to/rk3566/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-"
