// SPDX-License-Identifier: GPL-2.0
/*
 * 07_fixmap_inspect.c — Phase 7: Fixmap Inspector
 *
 * Enumerates fixmap slots and shows their fixed VA, mapped state,
 * and purpose. Fixmaps provide compile-time-constant virtual addresses
 * for early boot mappings before the full allocator is available.
 *
 * Maps to: 07_Setup_Arch/01_Early_Fixmap.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/fixmap.h>
#include <asm/pgtable.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 7: Fixmap slot inspector");
MODULE_AUTHOR("MM Inspector");

struct fixmap_entry {
	int idx;
	const char *name;
};

/* Known ARM64 fixmap slots */
static const struct fixmap_entry known_slots[] = {
	{ FIX_HOLE,       "FIX_HOLE (guard)" },
	{ FIX_FDT,        "FIX_FDT (device tree)" },
	{ FIX_EARLYCON_MEM_BASE, "FIX_EARLYCON_MEM_BASE" },
	{ FIX_TEXT_POKE0,  "FIX_TEXT_POKE0" },
	{ FIX_APEI_GHES_IRQ, "FIX_APEI_GHES_IRQ" },
	{ FIX_APEI_GHES_SEA, "FIX_APEI_GHES_SEA" },
#ifdef FIX_ENTRY_TRAMP_TEXT0
	{ FIX_ENTRY_TRAMP_TEXT0, "FIX_ENTRY_TRAMP_TEXT0 (KPTI)" },
#endif
	{ FIX_PTE,        "FIX_PTE (temp PTE mapping)" },
	{ FIX_PMD,        "FIX_PMD (temp PMD mapping)" },
	{ FIX_PUD,        "FIX_PUD (temp PUD mapping)" },
	{ FIX_PGD,        "FIX_PGD (temp PGD mapping)" },
};

static void check_fixmap_pte(unsigned long va)
{
	pgd_t *pgdp = pgd_offset_pgd(init_mm.pgd, va);
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;

	if (pgd_none(READ_ONCE(*pgdp))) {
		MI_INFO("    PGD: NONE\n");
		return;
	}

	p4dp = p4d_offset(pgdp, va);
	pudp = pud_offset(p4dp, va);
	pud = READ_ONCE(*pudp);

	if (pud_none(pud)) {
		MI_INFO("    PUD: NONE\n");
		return;
	}
	if (pud_sect(pud)) {
		MI_INFO("    PUD: 1GB block → PA=0x%llx\n",
			pud_val(pud) & 0x0000ffffc0000000ULL);
		return;
	}

	pmdp = pmd_offset(pudp, va);
	pmd = READ_ONCE(*pmdp);

	if (pmd_none(pmd)) {
		MI_INFO("    PMD: NONE\n");
		return;
	}
	if (pmd_sect(pmd)) {
		MI_INFO("    PMD: 2MB block → PA=0x%llx\n",
			pmd_val(pmd) & 0x0000ffffffe00000ULL);
		return;
	}

	ptep = pte_offset_kernel(pmdp, va);
	pte = READ_ONCE(*ptep);

	if (pte_none(pte)) {
		MI_INFO("    PTE: NONE (slot not currently mapped)\n");
	} else {
		MI_INFO("    PTE: 0x%016llx → PA=0x%llx (MAPPED)\n",
			pte_val(pte),
			pte_val(pte) & 0x0000fffffffff000ULL);
	}
}

static int __init fixmap_inspect_init(void)
{
	int i;
	unsigned long va;

	MI_INFO("######################################\n");
	MI_INFO("# Module 07: Fixmap Inspector        #\n");
	MI_INFO("# Phase 7 — Early Fixed Mappings     #\n");
	MI_INFO("######################################\n");

	MI_INFO("FIXADDR_TOP   = 0x%016lx\n", FIXADDR_TOP);
	MI_INFO("FIXADDR_START = 0x%016lx\n",
		FIXADDR_TOP - (__end_of_fixed_addresses * PAGE_SIZE));
	MI_INFO("Total slots   = %d\n", __end_of_fixed_addresses);
	MI_INFO("Total size    = %lu KB\n",
		(__end_of_fixed_addresses * PAGE_SIZE) >> 10);

	MI_INFO("\n--- Known Fixmap Slots ---\n");
	for (i = 0; i < ARRAY_SIZE(known_slots); i++) {
		va = __fix_to_virt(known_slots[i].idx);
		MI_INFO("[%3d] %-30s VA=0x%016lx\n",
			known_slots[i].idx, known_slots[i].name, va);
		check_fixmap_pte(va);
	}

	/* FIX_BTMAP slots (early ioremap) */
	MI_INFO("\n--- Early ioremap (FIX_BTMAP) ---\n");
	MI_INFO("FIX_BTMAP_BEGIN = %d\n", FIX_BTMAP_BEGIN);
	MI_INFO("FIX_BTMAP_END   = %d\n", FIX_BTMAP_END);

	for (i = FIX_BTMAP_BEGIN; i >= FIX_BTMAP_END; i--) {
		va = __fix_to_virt(i);
		MI_INFO("[%3d] FIX_BTMAP_%d VA=0x%016lx\n",
			i, FIX_BTMAP_BEGIN - i, va);
	}

	return 0;
}

static void __exit fixmap_inspect_exit(void)
{
	MI_INFO("Module 07 unloaded\n");
}

module_init(fixmap_inspect_init);
module_exit(fixmap_inspect_exit);
