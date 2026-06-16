# Makefile for ARM64 MM inspection kernel modules
KDIR ?= ../linux-6.12
CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH ?= arm64

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
