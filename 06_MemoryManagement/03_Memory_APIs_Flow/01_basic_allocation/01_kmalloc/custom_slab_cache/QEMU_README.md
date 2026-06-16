# Running Custom Slab Cache in QEMU ARM64

## Quick Start

```bash
chmod +x quick_start.sh
./quick_start.sh
```

This will automatically:
1. Setup QEMU ARM64 environment
2. Build Linux kernel for ARM64
3. Build BusyBox for ARM64
4. Cross-compile the kernel module
5. Create root filesystem with the module
6. Boot QEMU and run the module

## Manual Step-by-Step

### Step 1: Setup QEMU Environment
```bash
chmod +x setup_qemu.sh
./setup_qemu.sh
```

This will:
- Install QEMU and ARM64 cross-compiler
- Download and build Linux kernel 6.1 for ARM64
- Download and build BusyBox for ARM64
- Takes ~30-60 minutes depending on your system

### Step 2: Build Kernel Module
```bash
chmod +x build_module.sh
./build_module.sh
```

This cross-compiles custom_slab.c for ARM64 architecture.

### Step 3: Create Root Filesystem
```bash
chmod +x create_rootfs.sh
./create_rootfs.sh
```

This creates an initramfs with:
- BusyBox utilities
- Custom slab cache module
- Init script to load/test the module

### Step 4: Run in QEMU
```bash
chmod +x run_qemu.sh
./run_qemu.sh
```

QEMU will boot and automatically:
- Load the custom_slab module
- Display kernel messages
- Show /proc/slabinfo entry
- Unload the module
- Shutdown

## Expected Output

```
===================================
  ARM64 QEMU - Custom Slab Cache
===================================

Loading custom_slab module...

Module output:
Creating custom slab cache
Constructor called for object at ffff800080xxxxx
Cache created successfully
Allocated obj1: id=1, name=Object-1
Allocated obj2: id=2, name=Object-2
Allocated obj3: id=3, name=Object-3
Objects freed back to cache

Checking /proc/slabinfo for my_cache:
my_cache              3      3     64    3    1 : tunables  ...

Unloading module...
Cache destroyed
```

## QEMU Configuration

- **Machine**: virt (ARM64 virtual platform)
- **CPU**: Cortex-A57
- **Cores**: 2
- **Memory**: 1GB
- **Console**: ttyAMA0 (serial)

## Directory Structure

```
custom_slab_cache/
├── custom_slab.c          # Module source
├── Makefile               # Build configuration
├── README.md              # Module documentation
├── QEMU_README.md         # This file
├── quick_start.sh         # One-command setup
├── setup_qemu.sh          # Setup environment
├── build_module.sh        # Build module
├── create_rootfs.sh       # Create filesystem
├── run_qemu.sh            # Run QEMU
└── qemu_arm64/            # Working directory (created)
    ├── linux/             # Kernel source
    ├── busybox/           # BusyBox source
    └── rootfs.cpio.gz     # Root filesystem
```

## Requirements

- Ubuntu/Debian Linux (or similar)
- ~10GB disk space
- Internet connection (for downloads)
- sudo access (for package installation)

## Troubleshooting

### QEMU not found
```bash
sudo apt-get install qemu-system-aarch64
```

### Cross-compiler not found
```bash
sudo apt-get install gcc-aarch64-linux-gnu
```

### Module build fails
Ensure kernel is built first:
```bash
cd qemu_arm64/linux
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

### QEMU hangs
Press `Ctrl+A` then `X` to exit QEMU

## Customization

### Modify Module Behavior
Edit `custom_slab.c` and rebuild:
```bash
./build_module.sh
./create_rootfs.sh
./run_qemu.sh
```

### Interactive Shell
Edit `create_rootfs.sh` and replace `poweroff -f` with:
```bash
exec /bin/sh
```

### Enable Kernel Debug
Edit `run_qemu.sh` and add to kernel append:
```bash
-append "console=ttyAMA0 rdinit=/init loglevel=8"
```

## Clean Up

```bash
# Remove built files
make clean
rm -f custom_slab.ko

# Remove QEMU environment (WARNING: ~10GB)
rm -rf qemu_arm64/
```

## Performance Notes

- First build takes 30-60 minutes
- Subsequent module rebuilds take seconds
- QEMU boot time: ~5-10 seconds
- Module test execution: ~1 second

## Next Steps

1. Modify the module to test different slab cache scenarios
2. Add more complex object allocations
3. Test cache performance with stress tests
4. Explore /proc/slabinfo in interactive mode
5. Add debugging with SLAB_DEBUG flags
