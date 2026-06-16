// SPDX-License-Identifier: GPL-2.0
/*
 * 08_memblock_dump.c — Phase 8: Memblock Subsystem Inspector
 *
 * Walks memblock.memory and memblock.reserved arrays to show all
 * physical memory regions and reservations. Verifies the linear map
 * formula: VA = PA - memstart_addr + PAGE_OFFSET
 *
 * Maps to: 08_Memblock/01_Memblock_Subsystem.md, 02_ARM64_Memblock_Init.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <asm/memory.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 8: Memblock memory/reserved region dump");
MODULE_AUTHOR("MM Inspector");

static int __init memblock_dump_init(void)
{
	phys_addr_t start, end;
	u64 idx;
	int count;
	phys_addr_t total_mem = 0, total_rsv = 0;

	MI_INFO("######################################\n");
	MI_INFO("# Module 08: Memblock Dump           #\n");
	MI_INFO("# Phase 8 — Early Memory Allocator   #\n");
	MI_INFO("######################################\n");

	MI_INFO("memstart_addr = 0x%llx\n", (u64)memstart_addr);
	MI_INFO("PAGE_OFFSET   = 0x%016lx\n", PAGE_OFFSET);
	MI_INFO("Linear map formula: VA = PA - 0x%llx + 0x%lx\n",
		(u64)memstart_addr, PAGE_OFFSET);

	/* Walk memblock.memory */
	MI_INFO("\n--- memblock.memory (all RAM) ---\n");
	count = 0;
	for_each_mem_range(idx, &start, &end) {
		phys_addr_t size = end - start;

		MI_INFO("  [%llu] 0x%016llx - 0x%016llx  size=0x%llx (%llu MB)\n",
			idx, (u64)start, (u64)end - 1, (u64)size,
			(u64)(size >> 20));

		/* Verify linear map for this region */
		{
			unsigned long va = __phys_to_virt(start);
			phys_addr_t pa_back = __virt_to_phys(va);

			MI_INFO("    -> VA start = 0x%016lx\n", va);
			MI_INFO("    -> Round-trip PA = 0x%016llx %s\n",
				(u64)pa_back,
				pa_back == start ? "✓" : "✗");
		}

		total_mem += size;
		count++;
	}
	MI_INFO("  Total: %d regions, %llu MB (%llu GB)\n",
		count, (u64)(total_mem >> 20), (u64)(total_mem >> 30));

	/* Walk memblock.reserved */
	MI_INFO("\n--- memblock.reserved (in-use) ---\n");
	count = 0;
	for_each_reserved_mem_range(idx, &start, &end) {
		phys_addr_t size = end - start;

		MI_INFO("  [%llu] 0x%016llx - 0x%016llx  size=0x%llx (%llu KB)\n",
			idx, (u64)start, (u64)end - 1, (u64)size,
			(u64)(size >> 10));
		total_rsv += size;
		count++;
	}
	MI_INFO("  Total: %d regions, %llu MB reserved\n",
		count, (u64)(total_rsv >> 20));

	MI_INFO("\n--- Summary ---\n");
	MI_INFO("  Total RAM:      %llu MB\n", (u64)(total_mem >> 20));
	MI_INFO("  Total Reserved: %llu MB\n", (u64)(total_rsv >> 20));
	MI_INFO("  Available:      %llu MB\n",
		(u64)((total_mem - total_rsv) >> 20));

	return 0;
}

static void __exit memblock_dump_exit(void)
{
	MI_INFO("Module 08 unloaded\n");
}

module_init(memblock_dump_init);
module_exit(memblock_dump_exit);
