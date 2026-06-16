// SPDX-License-Identifier: GPL-2.0
/*
 * 10_zone_inspect.c — Phase 10: Zone and NUMA Inspector
 *
 * For each NUMA node, dumps zone information:
 *   - Zone name, PFN range, spanned/present/managed pages
 *   - Watermarks (min, low, high)
 *   - DMA limits
 *
 * Maps to: 10_Bootmem_Init/01_NUMA_Zones.md, 02_DMA_CMA.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 10: Zone/NUMA topology inspector");
MODULE_AUTHOR("MM Inspector");

static const char * const zone_names[] = {
	"DMA", "DMA32", "Normal", "Movable"
};

static int __init zone_inspect_init(void)
{
	int nid;
	pg_data_t *pgdat;

	MI_INFO("######################################\n");
	MI_INFO("# Module 10: Zone Inspector          #\n");
	MI_INFO("# Phase 10 — NUMA Zones & DMA Limits #\n");
	MI_INFO("######################################\n");

	MI_INFO("max_pfn     = %lu (0x%lx)\n", max_pfn, max_pfn);
	MI_INFO("totalram    = %lu pages (%lu MB)\n",
		totalram_pages(), (totalram_pages() << PAGE_SHIFT) >> 20);

	MI_INFO("nr_online_nodes = %d\n", num_online_nodes());

	for_each_online_node(nid) {
		struct zone *zone;
		int i;

		pgdat = NODE_DATA(nid);

		MI_INFO("\n=== Node %d ===\n", nid);
		MI_INFO("  node_start_pfn = %lu (PA: 0x%llx)\n",
			pgdat->node_start_pfn,
			(u64)pgdat->node_start_pfn << PAGE_SHIFT);
		MI_INFO("  node_spanned_pages = %lu (%lu MB)\n",
			pgdat->node_spanned_pages,
			(pgdat->node_spanned_pages << PAGE_SHIFT) >> 20);
		MI_INFO("  node_present_pages = %lu (%lu MB)\n",
			pgdat->node_present_pages,
			(pgdat->node_present_pages << PAGE_SHIFT) >> 20);

		for (i = 0; i < MAX_NR_ZONES; i++) {
			zone = &pgdat->node_zones[i];

			if (!zone->present_pages && !zone->spanned_pages)
				continue;

			MI_INFO("\n  Zone: %s\n", zone->name);
			MI_INFO("    zone_start_pfn  = %lu (PA: 0x%llx)\n",
				zone->zone_start_pfn,
				(u64)zone->zone_start_pfn << PAGE_SHIFT);
			MI_INFO("    spanned_pages   = %lu (%lu MB)\n",
				zone->spanned_pages,
				(zone->spanned_pages << PAGE_SHIFT) >> 20);
			MI_INFO("    present_pages   = %lu (%lu MB)\n",
				zone->present_pages,
				(zone->present_pages << PAGE_SHIFT) >> 20);
			MI_INFO("    managed_pages   = %lu (%lu MB)\n",
				zone_managed_pages(zone),
				(zone_managed_pages(zone) << PAGE_SHIFT) >> 20);
			MI_INFO("    Watermarks:\n");
			MI_INFO("      min  = %lu pages (%lu KB)\n",
				zone->_watermark[WMARK_MIN],
				(zone->_watermark[WMARK_MIN] << PAGE_SHIFT) >> 10);
			MI_INFO("      low  = %lu pages (%lu KB)\n",
				zone->_watermark[WMARK_LOW],
				(zone->_watermark[WMARK_LOW] << PAGE_SHIFT) >> 10);
			MI_INFO("      high = %lu pages (%lu KB)\n",
				zone->_watermark[WMARK_HIGH],
				(zone->_watermark[WMARK_HIGH] << PAGE_SHIFT) >> 10);

			MI_INFO("    PFN range: [%lu — %lu]\n",
				zone->zone_start_pfn,
				zone->zone_start_pfn + zone->spanned_pages - 1);
			MI_INFO("    PA range:  [0x%llx — 0x%llx]\n",
				(u64)zone->zone_start_pfn << PAGE_SHIFT,
				(u64)(zone->zone_start_pfn + zone->spanned_pages) << PAGE_SHIFT);
		}
	}

	/* Zone type mapping for ARM64 */
	MI_INFO("\n--- ARM64 Zone Layout (typical) ---\n");
	MI_INFO("  ZONE_DMA:    0 — arm64_dma_phys_limit (30-32 bit DMA)\n");
	MI_INFO("  ZONE_DMA32:  0 — 4GB (32-bit DMA devices)\n");
	MI_INFO("  ZONE_NORMAL: 4GB+ (general purpose)\n");
	MI_INFO("  ZONE_MOVABLE: (optional, CMA/hotplug)\n");

	return 0;
}

static void __exit zone_inspect_exit(void)
{
	MI_INFO("Module 10 unloaded\n");
}

module_init(zone_inspect_init);
module_exit(zone_inspect_exit);
