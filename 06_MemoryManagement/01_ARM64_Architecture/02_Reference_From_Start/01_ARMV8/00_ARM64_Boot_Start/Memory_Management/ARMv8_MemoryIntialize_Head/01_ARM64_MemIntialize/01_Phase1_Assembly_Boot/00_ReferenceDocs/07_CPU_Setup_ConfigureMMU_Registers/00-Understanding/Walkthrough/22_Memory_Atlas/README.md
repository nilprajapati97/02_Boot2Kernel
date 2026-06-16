# 22 Memory Atlas

This chapter summarizes the memory-model ideas that appear across the walkthrough.

## Virtual Address, Physical Address, And Translation

A virtual address is what software issues. A physical address is what the system fabric ultimately sees. The MMU plus the page tables plus the control registers translate one into the other.

## Memory Type

Memory type is not just "cacheable or not." Device and normal memory have different ordering and speculation behavior. Linux expresses those classes through `AttrIndx` values in descriptors and `MAIR_EL1` definitions in the control plane.

## Shareability

Shareability controls how the architecture treats the visibility domain of memory and page-walk behavior. For page-table walks, Linux programs the translation regime with specific shareability and cacheability choices so the walker behaves consistently with the kernel's memory model.

## Granule Size

The granule size affects page size, table shape, number of levels, TLB utilization patterns, and whether some extended address-size features are efficient or even legal.

## Access Flag

The access flag is part of the translation metadata that records whether a page has been accessed. Hardware support allows the MMU to set it directly under the rules Linux enables.

## Why The Memory Atlas Matters Here

Most of `__cpu_setup` is not managing data structures directly. It is telling the hardware how to interpret the page tables and what memory model to apply once translation starts.