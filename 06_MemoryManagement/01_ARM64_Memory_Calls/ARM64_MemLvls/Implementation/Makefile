# Master Makefile for ARM64 Memory Management Inspector
#
# Orchestrates: kernel build, module build, rootfs, QEMU launch, tests
#
# Usage:
#   make kernel      — Download and build Linux 6.12 with MM debug config
#   make modules     — Cross-compile all 13 inspection modules
#   make rootfs      — Create BusyBox initramfs with modules
#   make run         — Boot QEMU with kernel + initramfs
#   make run-gdb     — Boot QEMU halted, waiting for GDB
#   make test        — Automated test run with validation
#   make logs        — Extract per-phase logs from QEMU output
#   make clean       — Remove build artifacts
#   make all         — Full pipeline: kernel → modules → rootfs

SHELL := /bin/bash
IMPL_DIR := $(shell pwd)
SCRIPTS  := $(IMPL_DIR)/scripts
TESTS    := $(IMPL_DIR)/tests
OUTPUT   := $(IMPL_DIR)/output
MODULES  := $(IMPL_DIR)/modules

# Cross-compilation
ARCH         := arm64
CROSS_COMPILE := aarch64-linux-gnu-

# Kernel source (set by build_kernel.sh or override)
KDIR ?= $(IMPL_DIR)/linux-6.12

.PHONY: all kernel modules rootfs run run-gdb test logs clean help

all: kernel modules rootfs
	@echo ""
	@echo "============================================"
	@echo " Build complete! Run 'make run' to boot QEMU"
	@echo "============================================"

help:
	@echo "ARM64 Memory Management Inspector"
	@echo ""
	@echo "Targets:"
	@echo "  make kernel     — Build Linux 6.12 with MM debug config"
	@echo "  make modules    — Cross-compile inspection modules"
	@echo "  make rootfs     — Create BusyBox initramfs"
	@echo "  make run        — Boot QEMU (interactive)"
	@echo "  make run-gdb    — Boot QEMU + GDB server on :1234"
	@echo "  make test       — Automated test run"
	@echo "  make logs       — Extract phase logs from output"
	@echo "  make clean      — Remove build artifacts"
	@echo "  make all        — Full build pipeline"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - aarch64-linux-gnu-gcc (cross compiler)"
	@echo "  - qemu-system-aarch64"
	@echo "  - gdb-multiarch (for GDB sessions)"

kernel:
	@echo "[*] Building kernel..."
	@bash $(SCRIPTS)/build_kernel.sh

modules: $(OUTPUT)/Image
	@echo "[*] Building modules..."
	$(MAKE) -C $(MODULES) KDIR=$(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	@mkdir -p $(OUTPUT)/modules
	@cp $(MODULES)/*.ko $(OUTPUT)/modules/ 2>/dev/null || true
	@echo "[*] Modules copied to $(OUTPUT)/modules/"

rootfs: modules
	@echo "[*] Creating rootfs..."
	@bash $(SCRIPTS)/create_rootfs.sh

run:
	@echo "[*] Launching QEMU..."
	@bash $(SCRIPTS)/run_qemu.sh

run-gdb:
	@echo "[*] Launching QEMU with GDB..."
	@bash $(SCRIPTS)/run_qemu_gdb.sh

test:
	@echo "[*] Running automated tests..."
	@bash $(TESTS)/run_all_tests.sh
	@echo ""
	@echo "[*] Checking phase order..."
	@bash $(TESTS)/test_phase_order.sh
	@echo ""
	@echo "[*] Validating registers..."
	@bash $(TESTS)/test_registers.sh

logs:
	@echo "[*] Extracting logs..."
	@bash $(SCRIPTS)/extract_mm_logs.sh
	@bash $(SCRIPTS)/extract_early_boot.sh

clean:
	@echo "[*] Cleaning..."
	$(MAKE) -C $(MODULES) KDIR=$(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean 2>/dev/null || true
	rm -rf $(OUTPUT)/modules
	rm -f $(OUTPUT)/qemu_test_output.log
	rm -f $(OUTPUT)/module_*.log
	rm -f $(OUTPUT)/phase_*.log
	@echo "[*] Clean done (kernel and rootfs preserved)"

distclean: clean
	rm -rf $(OUTPUT)
	rm -rf $(IMPL_DIR)/linux-6.12
	rm -rf $(IMPL_DIR)/busybox-*
	@echo "[*] Full clean done"

# Check prerequisites
$(OUTPUT)/Image:
	@echo "[ERROR] Kernel not built. Run 'make kernel' first."
	@exit 1
