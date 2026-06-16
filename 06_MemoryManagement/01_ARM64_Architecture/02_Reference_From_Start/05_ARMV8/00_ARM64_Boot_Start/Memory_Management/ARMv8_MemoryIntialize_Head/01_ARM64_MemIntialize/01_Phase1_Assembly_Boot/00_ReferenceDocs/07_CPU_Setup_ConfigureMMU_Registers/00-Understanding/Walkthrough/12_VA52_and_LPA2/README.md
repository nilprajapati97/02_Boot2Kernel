# 12 VA52 And LPA2

This chapter explains one of the most important runtime-adaptive parts of `__cpu_setup`.

## The Problem

The kernel may be built with support for larger virtual addresses, but that does not mean every CPU that boots the image can legally use the larger regime.

Linux therefore separates:

- what the binary is capable of supporting
- what the current CPU actually advertises at runtime

## VA52 Handling

If the kernel supports 52-bit virtual addressing, Linux still starts with `T1SZ(VA_BITS_MIN)`. Then, if the runtime alternative path confirms `ARM64_HAS_VA52`, Linux patches `T1SZ` to the wider value.

That means the boot image is portable across CPUs with different capabilities.

## LPA2 Handling

When `CONFIG_ARM64_LPA2` is relevant and the CPU can support the extended regime, Linux also sets the `TCR_EL1.DS` control that changes the translation-format interpretation for LPA2-style operation.

## Hardware Meaning

These changes alter the translation geometry. They are not cosmetic. They affect how many bits of the virtual address participate in translation and what table format the hardware expects.

## Why This Belongs Here

This decision must be made before translation becomes live, because the table format and control register values must agree from the first translated access.