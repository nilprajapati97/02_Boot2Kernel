# Feature Detection Inside `__cpu_setup`

This function contains both compile-time and runtime adaptation.

## Compile-time adaptation

Some blocks only exist if the kernel was built with certain configuration options enabled.

Examples:
- `CONFIG_ARM64_VA_BITS_52`
- `CONFIG_ARM64_LPA2`
- `CONFIG_ARM64_HW_AFDBM`
- `CONFIG_ARM64_HAFT`

## Runtime adaptation

The function reads CPU identification registers.

### `ID_AA64MMFR1_EL1`
Used to detect support for hardware access-flag management and related features.

### `ID_AA64MMFR3_EL1`
Used to detect S1PIE support and TCR2 support.

## Why the two-level model exists

The kernel image may contain support code for features, but the running CPU may not implement all of them. So Linux must first be built with support and then check the actual hardware before using it.

## Practical result

Two machines booting the same kernel image can take slightly different internal `__cpu_setup` paths depending on their architectural feature set.
