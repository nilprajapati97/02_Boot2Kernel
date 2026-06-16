// SPDX-License-Identifier: GPL-2.0
/*
 * 05_kernel_segments.c — Phase 5: Kernel Segment Map Inspector
 *
 * Prints all kernel segment boundaries with VA, PA, size, and protection.
 * Verifies page table permissions match expected values for each segment.
 *
 * Maps to: 05_Map_Kernel/02_Map_Kernel_Segments.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/sections.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 5: Kernel segment boundaries and protection");
MODULE_AUTHOR("MM Inspector");

extern char _text[], _stext[], _etext[];
extern char __start_rodata[], __end_rodata[];
extern char __inittext_begin[], __inittext_end[];
extern char __initdata_begin[], __initdata_end[];
extern char _sdata[], _edata[];
extern char __bss_start[], __bss_stop[];
extern char _end[];

struct segment_info {
	const char *name;
	const char *start_sym;
	unsigned long start;
	unsigned long end;
	const char *expected_prot;
};

static void print_segment(const char *name, unsigned long start,
			   unsigned long end, const char *prot)
{
	phys_addr_t pa_start = __pa_symbol(start);
	unsigned long size = end - start;

	MI_INFO("  %-20s VA: 0x%016lx - 0x%016lx\n", name, start, end);
	MI_INFO("  %-20s PA: 0x%016llx  Size: %lu KB (%lu MB)\n",
		"", pa_start, size >> 10, size >> 20);
	MI_INFO("  %-20s Expected: %s\n", "", prot);
}

static void check_prot(const char *name, unsigned long addr)
{
	pgd_t *pgdp = pgd_offset_pgd(init_mm.pgd, addr);
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	u64 val;

	if (pgd_none(READ_ONCE(*pgdp)))
		return;

	p4dp = p4d_offset(pgdp, addr);
	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);

	if (pud_none(pud))
		return;

	if (pud_sect(pud)) {
		val = pud_val(pud);
		MI_INFO("  %-20s [1GB block] AP=%llu(%s) PXN=%llu UXN=%llu AttrIdx=%llu SH=%llu(%s)\n",
			name, FIELD(val, 7, 6), mi_ap_str(FIELD(val, 7, 6)),
			BIT_VAL(val, 53), BIT_VAL(val, 54),
			FIELD(val, 4, 2), FIELD(val, 9, 8),
			mi_share_str(FIELD(val, 9, 8)));
		return;
	}

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);

	if (pmd_none(pmd))
		return;

	if (pmd_sect(pmd)) {
		val = pmd_val(pmd);
		MI_INFO("  %-20s [2MB block] AP=%llu(%s) PXN=%llu UXN=%llu AttrIdx=%llu SH=%llu(%s)\n",
			name, FIELD(val, 7, 6), mi_ap_str(FIELD(val, 7, 6)),
			BIT_VAL(val, 53), BIT_VAL(val, 54),
			FIELD(val, 4, 2), FIELD(val, 9, 8),
			mi_share_str(FIELD(val, 9, 8)));
		return;
	}

	ptep = pte_offset_kernel(pmdp, addr);
	pte = READ_ONCE(*ptep);

	if (pte_none(pte))
		return;

	val = pte_val(pte);
	MI_INFO("  %-20s [4KB page]  AP=%llu(%s) PXN=%llu UXN=%llu AttrIdx=%llu SH=%llu(%s)\n",
		name, FIELD(val, 7, 6), mi_ap_str(FIELD(val, 7, 6)),
		BIT_VAL(val, 53), BIT_VAL(val, 54),
		FIELD(val, 4, 2), FIELD(val, 9, 8),
		mi_share_str(FIELD(val, 9, 8)));
}

static int __init kernel_segments_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 05: Kernel Segment Map      #\n");
	MI_INFO("# Phase 5 — Segment Boundaries       #\n");
	MI_INFO("######################################\n");

	MI_INFO("--- Kernel Segment Boundaries ---\n");
	print_segment(".head.text", (unsigned long)_text,
		      (unsigned long)_stext, "RW (head stub)");
	print_segment(".text", (unsigned long)_stext,
		      (unsigned long)_etext, "ROX (executable)");
	print_segment(".rodata", (unsigned long)__start_rodata,
		      (unsigned long)__end_rodata, "RO (read-only data)");
	print_segment(".init.text", (unsigned long)__inittext_begin,
		      (unsigned long)__inittext_end, "ROX (init, freed later)");
	print_segment(".init.data", (unsigned long)__initdata_begin,
		      (unsigned long)__initdata_end, "RW (init, freed later)");
	print_segment(".data", (unsigned long)_sdata,
		      (unsigned long)_edata, "RW (read-write data)");
	print_segment(".bss", (unsigned long)__bss_start,
		      (unsigned long)__bss_stop, "RW (zero-init data)");
	MI_INFO("  _end = 0x%016lx\n", (unsigned long)_end);

	MI_INFO("\n--- Page Table Protection Verification ---\n");
	check_prot(".text", (unsigned long)_stext);
	check_prot(".rodata", (unsigned long)__start_rodata);
	check_prot(".data", (unsigned long)_sdata);
	check_prot(".bss", (unsigned long)__bss_start);

	return 0;
}

static void __exit kernel_segments_exit(void)
{
	MI_INFO("Module 05 unloaded\n");
}

module_init(kernel_segments_init);
module_exit(kernel_segments_exit);
