// SPDX-License-Identifier: GPL-2.0
/*
 * 11_buddy_inspect.c — Phase 11: Buddy Allocator Inspector
 *
 * Dumps per-order, per-migratetype free page counts.
 * Inspects per-cpu page (PCP) lists.
 * Tests alloc_pages/free_pages to verify the allocator works.
 *
 * Maps to: 11_MM_Core_Init/01_Memblock_Free_All.md,
 *          02_Buddy_Allocator.md, 03_Build_Zonelists.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/gfp.h>
#include <linux/smp.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 11: Buddy allocator free counts, PCP, alloc test");
MODULE_AUTHOR("MM Inspector");

static const char * const migtype_names[] = {
	"Unmovable", "Movable", "Reclaimable", "HighAtomic",
#ifdef CONFIG_CMA
	"CMA",
#endif
	"Isolate"
};

static void dump_free_area(struct zone *zone)
{
	int order, mt;

	MI_INFO("  Free pages by order/migratetype:\n");
	MI_INFO("  %-6s", "Order");
	for (mt = 0; mt < MIGRATE_TYPES && mt < ARRAY_SIZE(migtype_names); mt++)
		MI_INFO("  %-12s", migtype_names[mt]);
	MI_INFO("\n");

	for (order = 0; order <= MAX_PAGE_ORDER; order++) {
		struct free_area *area = &zone->free_area[order];

		MI_INFO("  %-6d", order);
		for (mt = 0; mt < MIGRATE_TYPES && mt < ARRAY_SIZE(migtype_names); mt++) {
			unsigned long count = 0;

			count = area->nr_free;
			MI_INFO("  %-12lu", count);
		}
		MI_INFO("  (nr_free=%lu, %lu pages total)\n",
			area->nr_free,
			area->nr_free << order);
	}
}

static void dump_pcp(struct zone *zone)
{
	int cpu;

	MI_INFO("  Per-CPU Pages (PCP):\n");
	for_each_online_cpu(cpu) {
		struct per_cpu_pages *pcp;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		MI_INFO("    CPU %d: count=%d high=%d batch=%d\n",
			cpu, pcp->count, pcp->high, pcp->batch);
	}
}

static void test_alloc(void)
{
	struct page *p;
	phys_addr_t pa;
	void *va;
	int order;

	MI_INFO("\n--- Allocation Tests ---\n");

	/* Test order-0 (4KB) */
	p = alloc_pages(GFP_KERNEL, 0);
	if (p) {
		pa = page_to_phys(p);
		va = page_address(p);
		MI_INFO("  alloc_pages(order=0): page=%px PA=0x%llx VA=%px ✓\n",
			p, (u64)pa, va);
		__free_pages(p, 0);
		MI_INFO("  free_pages(order=0): returned ✓\n");
	} else {
		MI_INFO("  alloc_pages(order=0): FAILED ✗\n");
	}

	/* Test order-2 (16KB) */
	p = alloc_pages(GFP_KERNEL, 2);
	if (p) {
		pa = page_to_phys(p);
		MI_INFO("  alloc_pages(order=2): page=%px PA=0x%llx (16KB) ✓\n",
			p, (u64)pa);
		MI_INFO("    Compound head=%px order=%d\n",
			compound_head(p), compound_order(p));
		__free_pages(p, 2);
		MI_INFO("  free_pages(order=2): returned ✓\n");
	} else {
		MI_INFO("  alloc_pages(order=2): FAILED ✗\n");
	}

	/* Test max order-4 (64KB) */
	order = 4;
	p = alloc_pages(GFP_KERNEL | __GFP_NOWARN, order);
	if (p) {
		pa = page_to_phys(p);
		MI_INFO("  alloc_pages(order=%d): page=%px PA=0x%llx (%dKB) ✓\n",
			order, p, (u64)pa, (PAGE_SIZE << order) >> 10);
		__free_pages(p, order);
		MI_INFO("  free_pages(order=%d): returned ✓\n", order);
	} else {
		MI_INFO("  alloc_pages(order=%d): FAILED ✗ (fragmented?)\n", order);
	}

	/* Test zeroed page */
	p = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
	if (p) {
		va = page_address(p);
		/* Check first and last word are zero */
		if (*(unsigned long *)va == 0 &&
		    *((unsigned long *)(va + PAGE_SIZE - sizeof(unsigned long))) == 0)
			MI_INFO("  alloc_pages(__GFP_ZERO): verified zeroed ✓\n");
		else
			MI_INFO("  alloc_pages(__GFP_ZERO): NOT zeroed ✗\n");
		__free_pages(p, 0);
	}
}

static int __init buddy_inspect_init(void)
{
	int nid;
	pg_data_t *pgdat;

	MI_INFO("######################################\n");
	MI_INFO("# Module 11: Buddy Allocator Inspect #\n");
	MI_INFO("# Phase 11 — Free lists & PCP        #\n");
	MI_INFO("######################################\n");

	MI_INFO("MAX_PAGE_ORDER = %d (max alloc = %lu KB)\n",
		MAX_PAGE_ORDER,
		(unsigned long)(PAGE_SIZE << MAX_PAGE_ORDER) >> 10);
	MI_INFO("MIGRATE_TYPES  = %d\n", MIGRATE_TYPES);

	for_each_online_node(nid) {
		struct zone *zone;
		int i;

		pgdat = NODE_DATA(nid);

		for (i = 0; i < MAX_NR_ZONES; i++) {
			zone = &pgdat->node_zones[i];

			if (!zone_managed_pages(zone))
				continue;

			MI_INFO("\n=== Node %d Zone %s ===\n", nid, zone->name);
			MI_INFO("  managed = %lu pages (%lu MB)\n",
				zone_managed_pages(zone),
				(zone_managed_pages(zone) << PAGE_SHIFT) >> 20);

			dump_free_area(zone);
			dump_pcp(zone);
		}
	}

	test_alloc();

	return 0;
}

static void __exit buddy_inspect_exit(void)
{
	MI_INFO("Module 11 unloaded\n");
}

module_init(buddy_inspect_init);
module_exit(buddy_inspect_exit);
