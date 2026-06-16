// SPDX-License-Identifier: GPL-2.0
/*
 * 02_idmap_walk.c — Phase 2: Identity Map Page Table Walker
 *
 * Walks idmap_pg_dir to dump all VA==PA identity map entries.
 * Decodes every PTE bit: Valid, Type, AttrIndx, NS, AP, SH, AF, nG,
 * Output Address, Contiguous, PXN, UXN.
 *
 * Maps to: 02_Identity_Map/01_Create_Init_Idmap.md, 02_Map_Range.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 2: Identity map page table walker");
MODULE_AUTHOR("MM Inspector");

/* idmap_pg_dir is the final identity map (created in paging_init) */
extern pgd_t idmap_pg_dir[];

static int entries_found;

static void walk_pte(pmd_t *pmdp, unsigned long addr, unsigned long end)
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

		entries_found++;
		MI_INFO("  PTE: VA=0x%016lx -> PA=0x%llx [4KB page]\n",
			addr, pte_val(pte) & 0x0000fffffffff000ULL);
		if (entries_found <= 5)
			mi_decode_pte("    ", pte_val(pte), 3);
	} while (ptep++, addr = next, addr < end);
}

static void walk_pmd(pud_t *pudp, unsigned long addr, unsigned long end)
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
			/* 2MB block mapping */
			u64 pa = pmd_val(pmd) & 0x0000ffffffe00000ULL;

			entries_found++;
			MI_INFO("  PMD: VA=0x%016lx -> PA=0x%llx [2MB block]\n",
				addr, pa);
			if (addr == pa)
				MI_INFO("    VA==PA ✓ (identity mapped)\n");
			else
				MI_INFO("    VA!=PA ✗ (NOT identity mapped!)\n");

			if (entries_found <= 3)
				mi_decode_pte("    ", pmd_val(pmd), 2);
			continue;
		}

		/* Table entry — walk next level */
		walk_pte(pmdp, addr, next);
	} while (pmdp++, addr = next, addr < end);
}

static void walk_pud(p4d_t *p4dp, unsigned long addr, unsigned long end)
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
			/* 1GB block mapping */
			u64 pa = pud_val(pud) & 0x0000ffffc0000000ULL;

			entries_found++;
			MI_INFO("  PUD: VA=0x%016lx -> PA=0x%llx [1GB block]\n",
				addr, pa);
			if (addr == pa)
				MI_INFO("    VA==PA ✓ (identity mapped)\n");
			else
				MI_INFO("    VA!=PA ✗ (NOT identity mapped!)\n");
			continue;
		}

		walk_pmd(pudp, addr, next);
	} while (pudp++, addr = next, addr < end);
}

static void walk_idmap(void)
{
	pgd_t *pgdp;
	unsigned long addr = 0;
	unsigned long end = (1UL << 48); /* 48-bit VA space */
	unsigned long next;
	int pgd_idx;

	MI_INFO("Walking idmap_pg_dir at %px\n", idmap_pg_dir);
	MI_INFO("Scanning for valid entries in 48-bit VA space...\n");

	pgdp = idmap_pg_dir;
	for (pgd_idx = 0; pgd_idx < PTRS_PER_PGD; pgd_idx++, pgdp++) {
		pgd_t pgd = READ_ONCE(*pgdp);
		p4d_t *p4dp;

		addr = (unsigned long)pgd_idx << PGDIR_SHIFT;
		next = addr + (1UL << PGDIR_SHIFT);
		if (next > end)
			next = end;

		if (pgd_none(pgd))
			continue;

		MI_INFO("PGD[%d]: VA base=0x%016lx entry=0x%016llx\n",
			pgd_idx, addr, pgd_val(pgd));

		p4dp = p4d_offset(&pgd, addr);
		walk_pud(p4dp, addr, next);
	}
}

static int __init idmap_walk_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 02: Identity Map Walker     #\n");
	MI_INFO("# Phase 2 — VA==PA Verification      #\n");
	MI_INFO("######################################\n");

	entries_found = 0;
	walk_idmap();

	MI_INFO("Total identity map entries found: %d\n", entries_found);

	return 0;
}

static void __exit idmap_walk_exit(void)
{
	MI_INFO("Module 02 unloaded\n");
}

module_init(idmap_walk_init);
module_exit(idmap_walk_exit);
