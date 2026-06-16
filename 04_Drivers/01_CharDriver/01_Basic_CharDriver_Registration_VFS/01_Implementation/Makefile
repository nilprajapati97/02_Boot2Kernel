# =============================================================================
# Makefile — basic_chardriver (ARM64 cross-compile)
# =============================================================================
#
# Usage:
#   make                        — build using default KDIR (set by setup_env.sh)
#   make KDIR=/path/to/kernel   — specify a custom kernel build directory
#   make clean                  — remove build artifacts
#
# KDIR must point to the kernel build output directory (not source root),
# i.e., the directory that contains a valid .config and built vmlinux.
# =============================================================================

DRIVER_NAME   := basic_chardriver
ARCH          := arm64
CROSS_COMPILE := aarch64-linux-gnu-

# Default KDIR: kernel built by ../qemu_arm64/setup_env.sh
# Override on the command line: make KDIR=/your/kernel/build
KDIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../qemu_arm64/env/linux-build)

obj-m := $(DRIVER_NAME).o

# ── Targets ──────────────────────────────────────────────────────────────────

all:
	@echo "[CC] Cross-compiling $(DRIVER_NAME).ko  (ARCH=$(ARCH))"
	@echo "[CC] Kernel build dir: $(KDIR)"
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@echo "[OK] $(CURDIR)/$(DRIVER_NAME).ko"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

.PHONY: all clean
