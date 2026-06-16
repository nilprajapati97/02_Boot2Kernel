// SPDX-License-Identifier: GPL-2.0
/*
 * 04_ttbr_translate.c — Phase 4,5: Manual VA-to-PA Translation
 *
 * Demonstrates the full translation table walk:
 *   VA → PGD[47:39] → PUD[38:30] → PMD[29:21] → PTE[20:12] → offset[11:0]
 *
 * Extracts indices at each level, reads entries, and computes final PA.
 * Compares with virt_to_phys() result to verify.
 *
 * Maps to: 04_Enable_MMU/01_TTBR_Setup.md
 *          05_Map_Kernel/01_Early_Map_Kernel.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 4,5: Manual VA→PA translation demo");
MODULE_AUTHOR("MM Inspector");

extern char _text[];

static void manual_translate(const char *name, unsigned long va)
{
	u64 ttbr1 = mi_read_ttbr1_el1();
	u64 tcr   = mi_read_tcr_el1();
	u64 t1sz  = FIELD(tcr, 21, 16);
	u64 va_bits = 64 - t1sz;
	int pgd_idx, pud_idx, pmd_idx, pte_idx;
	unsigned long offset;
	pgd_t *pgdp, pgd;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	phys_addr_t computed_pa;
	phys_addr_t kernel_pa;

	MI_INFO("========================================\n");
	MI_INFO("Manual VA→PA: %s = 0x%016lx\n", name, va);
	MI_INFO("========================================\n");

	/* Show TTBR1 selection */
	MI_INFO("  VA bit[63] = %d → using TTBR1 (kernel space)\n",
		(va >> 63) & 1 ? 1 : 0);
	MI_INFO("  TTBR1_EL1 BADDR = 0x%llx\n",
		ttbr1 & 0x0000ffffffffffffULL);
	MI_INFO("  TCR.T1SZ = %llu → VA bits = %llu\n", t1sz, va_bits);

	/* Extract indices (4KB granule, 48-bit VA, 4 levels) */
	pgd_idx = (va >> 39) & 0x1ff;
	pud_idx = (va >> 30) & 0x1ff;
	pmd_idx = (va >> 21) & 0x1ff;
	pte_idx = (va >> 12) & 0x1ff;
	offset  = va & 0xfff;

	MI_INFO("  Index extraction (4KB, 4-level):\n");
	MI_INFO("    PGD index [47:39] = %d  (VA bits 47-39 = 0x%lx)\n",
		pgd_idx, (va >> 39) & 0x1ff);
	MI_INFO("    PUD index [38:30] = %d  (VA bits 38-30 = 0x%lx)\n",
		pud_idx, (va >> 30) & 0x1ff);
	MI_INFO("    PMD index [29:21] = %d  (VA bits 29-21 = 0x%lx)\n",
		pmd_idx, (va >> 21) & 0x1ff);
	MI_INFO("    PTE index [20:12] = %d  (VA bits 20-12 = 0x%lx)\n",
		pte_idx, (va >> 12) & 0x1ff);
	MI_INFO("    Offset    [11:0]  = 0x%lx  (%lu bytes)\n",
		offset, offset);

	/* Walk the table */
	pgdp = pgd_offset_pgd(init_mm.pgd, va);
	pgd = READ_ONCE(*pgdp);
	MI_INFO("  L0: PGD entry = 0x%016llx  (Valid=%llu, Type=%s)\n",
		pgd_val(pgd), BIT_VAL(pgd_val(pgd), 0),
		mi_pte_type_str(pgd_val(pgd), 0));

	if (pgd_none(pgd)) {
		MI_INFO("  STOP: PGD entry is NONE\n");
		return;
	}

	p4dp = p4d_offset(pgdp, va);
	pudp = pud_offset(p4dp, va);
	pud = READ_ONCE(*pudp);
	MI_INFO("  L1: PUD entry = 0x%016llx  (Valid=%llu, Type=%s)\n",
		pud_val(pud), BIT_VAL(pud_val(pud), 0),
		mi_pte_type_str(pud_val(pud), 1));

	if (pud_none(pud)) {
		MI_INFO("  STOP: PUD entry is NONE\n");
		return;
	}

	if (pud_sect(pud)) {
		computed_pa = (pud_val(pud) & 0x0000ffffc0000000ULL) |
			      (va & 0x3fffffff);
		MI_INFO("  -> 1GB BLOCK: PA = 0x%llx | offset = 0x%lx\n",
			pud_val(pud) & 0x0000ffffc0000000ULL, va & 0x3fffffff);
		MI_INFO("  Computed PA = 0x%llx\n", computed_pa);
		goto verify;
	}

	pmdp = pmd_offset(pudp, va);
	pmd = READ_ONCE(*pmdp);
	MI_INFO("  L2: PMD entry = 0x%016llx  (Valid=%llu, Type=%s)\n",
		pmd_val(pmd), BIT_VAL(pmd_val(pmd), 0),
		mi_pte_type_str(pmd_val(pmd), 2));

	if (pmd_none(pmd)) {
		MI_INFO("  STOP: PMD entry is NONE\n");
		return;
	}

	if (pmd_sect(pmd)) {
		computed_pa = (pmd_val(pmd) & 0x0000ffffffe00000ULL) |
			      (va & 0x1fffff);
		MI_INFO("  -> 2MB BLOCK: PA = 0x%llx | offset = 0x%lx\n",
			pmd_val(pmd) & 0x0000ffffffe00000ULL, va & 0x1fffff);
		MI_INFO("  Computed PA = 0x%llx\n", computed_pa);
		goto verify;
	}

	ptep = pte_offset_kernel(pmdp, va);
	pte = READ_ONCE(*ptep);
	MI_INFO("  L3: PTE entry = 0x%016llx  (Valid=%llu, Type=%s)\n",
		pte_val(pte), BIT_VAL(pte_val(pte), 0),
		mi_pte_type_str(pte_val(pte), 3));

	if (pte_none(pte)) {
		MI_INFO("  STOP: PTE entry is NONE\n");
		return;
	}

	computed_pa = (pte_val(pte) & 0x0000fffffffff000ULL) | offset;
	MI_INFO("  -> 4KB PAGE: PA = 0x%llx | offset = 0x%lx\n",
		pte_val(pte) & 0x0000fffffffff000ULL, offset);
	MI_INFO("  Computed PA = 0x%llx\n", computed_pa);

verify:
	kernel_pa = virt_to_phys((void *)va);
	MI_INFO("  virt_to_phys() = 0x%llx\n", kernel_pa);
	if (computed_pa == kernel_pa)
		MI_INFO("  MATCH ✓ Manual walk agrees with virt_to_phys()\n");
	else
		MI_INFO("  MISMATCH ✗ computed=0x%llx vs kernel=0x%llx\n",
			computed_pa, kernel_pa);
}

static int __init ttbr_translate_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 04: TTBR Translation Demo   #\n");
	MI_INFO("# Phase 4,5 — Manual VA→PA Walk      #\n");
	MI_INFO("######################################\n");

	/* Show current TTBR state */
	MI_INFO("TTBR0_EL1 = 0x%016llx (user/idmap)\n", mi_read_ttbr0_el1());
	MI_INFO("TTBR1_EL1 = 0x%016llx (kernel)\n", mi_read_ttbr1_el1());

	/* Translate key addresses */
	manual_translate("_text", (unsigned long)_text);
	manual_translate("PAGE_OFFSET", PAGE_OFFSET);
	manual_translate("module_self", (unsigned long)ttbr_translate_init);

	return 0;
}

static void __exit ttbr_translate_exit(void)
{
	MI_INFO("Module 04 unloaded\n");
}

module_init(ttbr_translate_init);
module_exit(ttbr_translate_exit);
