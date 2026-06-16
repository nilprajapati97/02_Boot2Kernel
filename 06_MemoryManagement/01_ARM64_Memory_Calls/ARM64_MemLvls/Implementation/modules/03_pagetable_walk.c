// SPDX-License-Identifier: GPL-2.0
/*
 * 03_pagetable_walk.c — Phase 5,9: Kernel Page Table Walker
 *
 * Walks swapper_pg_dir (via init_mm.pgd) for key kernel addresses:
 *   - _text          (kernel code — should be ROX)
 *   - _sdata         (kernel data — should be RW-NX)
 *   - PAGE_OFFSET    (linear map start)
 *   - VMALLOC_START  (vmalloc region)
 *   - FIXADDR_TOP    (fixmap top)
 *
 * At each level, decodes PTE bits: type, protection, shareability,
 * cache attributes, contiguous.
 *
 * Maps to: 05_Map_Kernel/02_Map_Kernel_Segments.md
 *          09_Paging_Init/01_Map_Mem.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/fixmap.h>
#include <asm/memory.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 5,9: Kernel page table walker with bit decode");
MODULE_AUTHOR("MM Inspector");

extern char _text[], _sdata[];

static void walk_addr(const char *name, unsigned long addr)
{
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	int pgd_idx, pud_idx, pmd_idx, pte_idx;

	MI_INFO("--- Walking: %s = 0x%016lx ---\n", name, addr);

	/* PGD */
	pgdp = pgd_offset_pgd(init_mm.pgd, addr);
	pgd = READ_ONCE(*pgdp);
	pgd_idx = pgd_index(addr);

	MI_INFO("  L0 PGD[%d] = 0x%016llx\n", pgd_idx, pgd_val(pgd));
	if (pgd_none(pgd)) {
		MI_INFO("    -> PGD NONE (unmapped)\n");
		return;
	}

	/* P4D (on 4-level tables, this is folded into PGD) */
	p4dp = p4d_offset(pgdp, addr);
	p4d = READ_ONCE(*p4dp);

	/* PUD */
	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	pud_idx = pud_index(addr);

	MI_INFO("  L1 PUD[%d] = 0x%016llx\n", pud_idx, pud_val(pud));
	if (pud_none(pud)) {
		MI_INFO("    -> PUD NONE (unmapped)\n");
		return;
	}
	if (pud_sect(pud)) {
		MI_INFO("    -> 1GB BLOCK mapping\n");
		MI_INFO("    PA = 0x%llx\n",
			pud_val(pud) & 0x0000ffffc0000000ULL);
		mi_decode_pte("    ", pud_val(pud), 1);
		return;
	}

	/* PMD */
	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	pmd_idx = pmd_index(addr);

	MI_INFO("  L2 PMD[%d] = 0x%016llx\n", pmd_idx, pmd_val(pmd));
	if (pmd_none(pmd)) {
		MI_INFO("    -> PMD NONE (unmapped)\n");
		return;
	}
	if (pmd_sect(pmd)) {
		MI_INFO("    -> 2MB BLOCK mapping\n");
		MI_INFO("    PA = 0x%llx\n",
			pmd_val(pmd) & 0x0000ffffffe00000ULL);
		mi_decode_pte("    ", pmd_val(pmd), 2);
		return;
	}

	/* PTE */
	ptep = pte_offset_kernel(pmdp, addr);
	pte = READ_ONCE(*ptep);
	pte_idx = pte_index(addr);

	MI_INFO("  L3 PTE[%d] = 0x%016llx\n", pte_idx, pte_val(pte));
	if (pte_none(pte)) {
		MI_INFO("    -> PTE NONE (unmapped)\n");
		return;
	}
	MI_INFO("    -> 4KB PAGE mapping\n");
	MI_INFO("    PA = 0x%llx\n",
		pte_val(pte) & 0x0000fffffffff000ULL);
	mi_decode_pte("    ", pte_val(pte), 3);

	/* Verify protection for known segments */
	if (addr == (unsigned long)_text) {
		u64 ap = FIELD(pte_val(pte), 7, 6);
		u64 pxn = BIT_VAL(pte_val(pte), 53);
		u64 uxn = BIT_VAL(pte_val(pte), 54);

		MI_INFO("    [VERIFY] Kernel text should be ROX:\n");
		MI_INFO("      AP=%llu (%s), PXN=%llu, UXN=%llu\n",
			ap, mi_ap_str(ap), pxn, uxn);
		if (ap >= 2 && pxn == 0)
			MI_INFO("      PASS: Read-Only + Executable ✓\n");
		else
			MI_INFO("      CHECK: Protection may differ (block mapping?)\n");
	}
}

static int __init pagetable_walk_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 03: Page Table Walker       #\n");
	MI_INFO("# Phase 5,9 — swapper_pg_dir Walk    #\n");
	MI_INFO("######################################\n");
	MI_INFO("init_mm.pgd (swapper_pg_dir) = %px\n", init_mm.pgd);

	walk_addr("_text", (unsigned long)_text);
	walk_addr("_sdata", (unsigned long)_sdata);
	walk_addr("PAGE_OFFSET", PAGE_OFFSET);
	walk_addr("PAGE_OFFSET+16MB", PAGE_OFFSET + SZ_16M);
	walk_addr("VMALLOC_START", VMALLOC_START);

	return 0;
}

static void __exit pagetable_walk_exit(void)
{
	MI_INFO("Module 03 unloaded\n");
}

module_init(pagetable_walk_init);
module_exit(pagetable_walk_exit);
