The diagram below illustrates the Linux kernel probe() call flow using the Goodix touchscreen controller driver as an example; however, the probe() call flow is almost similar to all Linux device drivers.

During the boot, the kernel parses the Device Tree Binary (DTB) to discover hardware devices, creating device structures and registering them with the appropriate bus (I2C, SPI, platform, etc.). Later, when driver init happens, the drivers register themselves with their respective bus subsystems.

When a driver registers, it triggers the matching phase, which binds the devices to the driver. The kernel iterates through all devices on the bus, comparing each against the driver using multiple matching mechanisms: Device Tree compatible strings, ACPI IDs, or bus-specific ID tables. This flexible approach allows the same driver to work across different firmware interfaces.

Upon finding a match, the kernel calls the driver's probe() function. This is the function where the driver requests all necessary resources like memory-mapped I/O regions, interrupt lines, clocks, power regulators, GPIO pins, and DMA channels. The probe function also performs hardware-specific initialization sequences and registers the device with higher-level kernel subsystems (input, network, block, etc.).

