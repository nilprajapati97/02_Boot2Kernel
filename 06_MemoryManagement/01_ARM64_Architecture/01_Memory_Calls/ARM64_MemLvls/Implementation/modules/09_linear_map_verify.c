// SPDX-License-Identifier: GPL-2.0
/*
 * 09_linear_map_verify.c — Phase 9: Linear Map Verification
 *
 * Tests phys_to_virt/virt_to_phys round-trip for multiple addresses.
 * Walks swapper_pg_dir for the linear map region and counts mapping
 * types: 1GB blocks, 2MB blocks, 4KB pages, contiguous entries.
 *
 * Maps to: 09_Paging_Init/01_Map_Mem.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 9: Linear map round-trip and mapping type count");
MODULE_AUTHOR("MM Inspector");

static int blocks_1g, blocks_2m, pages_4k, cont_entries;

static void count_linear_pte(pmd_t *pmdp, unsigned long addr, unsigned long end)
{
	pte_t *ptep;
	unsigned long next;

	ptep = pte_offset_kernel(pmdp, addr);
	do {
		pte_t pte = READ_ONCE(*ptep);

		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		if (pte_none(pte))
			continue;

		pages_4k++;
		if (BIT_VAL(pte_val(pte), 52))
			cont_entries++;
	} while (ptep++, addr = next, addr < end);
}

static void count_linear_pmd(pud_t *pudp, unsigned long addr, unsigned long end)
{
	pmd_t *pmdp;
	unsigned long next;

	pmdp = pmd_offset(pudp, addr);
	do {
		pmd_t pmd = READ_ONCE(*pmdp);

		next = pmd_addr_end(addr, end);
		if (pmd_none(pmd))
			continue;

		if (pmd_sect(pmd)) {
			blocks_2m++;
			if (BIT_VAL(pmd_val(pmd), 52))
				cont_entries++;
			continue;
		}

		count_linear_pte(pmdp, addr, next);
	} while (pmdp++, addr = next, addr < end);
}

static void count_linear_pud(p4d_t *p4dp, unsigned long addr, unsigned long end)
{
	pud_t *pudp;
	unsigned long next;

	pudp = pud_offset(p4dp, addr);
	do {
		pud_t pud = READ_ONCE(*pudp);

		next = pud_addr_end(addr, end);
		if (pud_none(pud))
			continue;

		if (pud_sect(pud)) {
			blocks_1g++;
			continue;
		}

		count_linear_pmd(pudp, addr, next);
	} while (pudp++, addr = next, addr < end);
}

static void count_linear_map(void)
{
	unsigned long addr = PAGE_OFFSET;
	/* Scan a reasonable range: first 8GB of linear map */
	unsigned long end = PAGE_OFFSET + (8UL << 30);
	pgd_t *pgdp;
	unsigned long next;

	blocks_1g = 0;
	blocks_2m = 0;
	pages_4k = 0;
	cont_entries = 0;

	MI_INFO("Scanning linear map 0x%lx - 0x%lx...\n", addr, end);

	pgdp = pgd_offset_pgd(init_mm.pgd, addr);
	do {
		pgd_t pgd = READ_ONCE(*pgdp);
		p4d_t *p4dp;

		next = pgd_addr_end(addr, end);
		if (pgd_none(pgd)) {
			pgdp++;
			addr = next;
			continue;
		}

		p4dp = p4d_offset(pgdp, addr);
		count_linear_pud(p4dp, addr, next);
		pgdp++;
		addr = next;
	} while (addr < end);
}

static void test_round_trip(phys_addr_t pa)
{
	unsigned long va = __phys_to_virt(pa);
	phys_addr_t pa2 = __virt_to_phys(va);

	MI_INFO("  PA 0x%016llx → VA 0x%016lx → PA 0x%016llx  %s\n",
		(u64)pa, va, (u64)pa2,
		pa == pa2 ? "✓ PASS" : "✗ FAIL");
}

static int __init linear_map_init(void)
{
	phys_addr_t start, end;
	u64 idx;
	int tested = 0;

	MI_INFO("######################################\n");
	MI_INFO("# Module 09: Linear Map Verify       #\n");
	MI_INFO("# Phase 9 — phys↔virt round-trip     #\n");
	MI_INFO("######################################\n");

	MI_INFO("PAGE_OFFSET   = 0x%016lx\n", PAGE_OFFSET);
	MI_INFO("memstart_addr = 0x%llx\n", (u64)memstart_addr);
	MI_INFO("Formula: VA = PA - memstart_addr + PAGE_OFFSET\n\n");

	/* Test round-trip for start/middle/end of each memory region */
	MI_INFO("--- Round-trip tests ---\n");
	for_each_mem_range(idx, &start, &end) {
		phys_addr_t mid = start + ((end - start) / 2);

		test_round_trip(start);
		test_round_trip(mid);
		if (end > PAGE_SIZE)
			test_round_trip(end - PAGE_SIZE);
		tested += 3;
	}
	MI_INFO("  %d addresses tested\n\n", tested);

	/* Count mapping types in linear map */
	MI_INFO("--- Linear map mapping types ---\n");
	count_linear_map();
	MI_INFO("  1GB blocks:       %d\n", blocks_1g);
	MI_INFO("  2MB blocks:       %d\n", blocks_2m);
	MI_INFO("  4KB pages:        %d\n", pages_4k);
	MI_INFO("  Contiguous (PTE_CONT): %d\n", cont_entries);

	MI_INFO("  Total mapped: %d GB + %d MB + %d KB\n",
		blocks_1g,
		blocks_2m * 2,
		pages_4k * 4);

	/* TLB efficiency */
	{
		int tlb_entries = blocks_1g + blocks_2m + pages_4k;
		int tlb_cont = blocks_1g + (blocks_2m - cont_entries) +
			       (pages_4k - cont_entries) +
			       (cont_entries / 16);

		MI_INFO("\n  TLB entries needed (without contiguous): %d\n",
			tlb_entries);
		MI_INFO("  TLB entries needed (with contiguous):    ~%d\n",
			tlb_cont > 0 ? tlb_cont : tlb_entries);
	}

	return 0;
}

static void __exit linear_map_exit(void)
{
	MI_INFO("Module 09 unloaded\n");
}

module_init(linear_map_init);
module_exit(linear_map_exit);
