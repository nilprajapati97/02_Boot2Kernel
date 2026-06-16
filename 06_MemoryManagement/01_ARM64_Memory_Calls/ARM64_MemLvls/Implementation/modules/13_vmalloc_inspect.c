// SPDX-License-Identifier: GPL-2.0
/*
 * 13_vmalloc_inspect.c — Phase 13: Vmalloc Inspector
 *
 * Shows VMALLOC_START/END, walks vmap_area list via /proc/vmallocinfo,
 * tests vmalloc/vfree, and demonstrates VA→PA for vmalloc addresses.
 *
 * Maps to: 13_Vmalloc/00_Overview.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/pgtable.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 13: Vmalloc space inspector and test");
MODULE_AUTHOR("MM Inspector");

static void walk_vmalloc_pte(unsigned long va)
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
		MI_INFO("    PUD: 1GB block (unexpected for vmalloc)\n");
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
		MI_INFO("    PTE: NONE\n");
	} else {
		phys_addr_t pa = pte_val(pte) & 0x0000fffffffff000ULL;

		MI_INFO("    PTE: 0x%016llx → PA=0x%llx\n",
			pte_val(pte), (u64)pa);
		mi_decode_pte(pte_val(pte));
	}
}

static void test_vmalloc(void)
{
	void *va;
	unsigned long addr;
	int i;
	static const int sizes[] = { PAGE_SIZE, PAGE_SIZE * 4, PAGE_SIZE * 16 };

	MI_INFO("\n--- vmalloc Allocation Tests ---\n");

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		va = vmalloc(sizes[i]);
		if (!va) {
			MI_INFO("  vmalloc(%d) FAILED ✗\n", sizes[i]);
			continue;
		}

		addr = (unsigned long)va;
		MI_INFO("  vmalloc(%d) = %px (%d pages)\n",
			sizes[i], va, sizes[i] >> PAGE_SHIFT);

		/* Check it's in vmalloc range */
		MI_INFO("    In vmalloc range: %s\n",
			is_vmalloc_addr(va) ? "✓ YES" : "✗ NO");

		/* Walk page table for first page */
		MI_INFO("    Page table walk for first page (VA=0x%lx):\n", addr);
		walk_vmalloc_pte(addr);

		/* Walk for last page */
		if (sizes[i] > PAGE_SIZE) {
			unsigned long last = addr + sizes[i] - PAGE_SIZE;

			MI_INFO("    Page table walk for last page (VA=0x%lx):\n",
				last);
			walk_vmalloc_pte(last);
		}

		/* Verify pages are NOT physically contiguous */
		if (sizes[i] >= 2 * PAGE_SIZE) {
			struct page *p1, *p2;
			phys_addr_t pa1, pa2;

			p1 = vmalloc_to_page(va);
			p2 = vmalloc_to_page(va + PAGE_SIZE);
			if (p1 && p2) {
				pa1 = page_to_phys(p1);
				pa2 = page_to_phys(p2);
				MI_INFO("    Page 0 PA: 0x%llx\n", (u64)pa1);
				MI_INFO("    Page 1 PA: 0x%llx\n", (u64)pa2);
				MI_INFO("    Contiguous: %s (delta=0x%llx)\n",
					(pa2 - pa1 == PAGE_SIZE) ?
					"yes (happens to be)" : "no (expected)",
					(u64)(pa2 - pa1));
			}
		}

		/* Write/read test */
		memset(va, 0xAB, PAGE_SIZE);
		if (*(unsigned char *)va == 0xAB)
			MI_INFO("    Write/read test: ✓\n");
		else
			MI_INFO("    Write/read test: ✗\n");

		vfree(va);
		MI_INFO("    Freed ✓\n");
	}
}

static void test_vmap(void)
{
	struct page *pages[4];
	void *va;
	int i;
	int ok = 1;

	MI_INFO("\n--- vmap Test (manual page mapping) ---\n");

	/* Allocate 4 individual pages */
	for (i = 0; i < 4; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i]) {
			MI_INFO("  alloc_page(%d) FAILED ✗\n", i);
			ok = 0;
			break;
		}
	}

	if (!ok) {
		for (i = 0; i < 4 && pages[i]; i++)
			__free_page(pages[i]);
		return;
	}

	MI_INFO("  Allocated 4 individual pages:\n");
	for (i = 0; i < 4; i++) {
		MI_INFO("    page[%d] PA=0x%llx\n", i,
			(u64)page_to_phys(pages[i]));
	}

	/* Map them contiguously */
	va = vmap(pages, 4, VM_MAP, PAGE_KERNEL);
	if (va) {
		MI_INFO("  vmap() → VA=%px (4 pages contiguous in VA)\n", va);
		MI_INFO("    is_vmalloc_addr: %s\n",
			is_vmalloc_addr(va) ? "✓" : "✗");

		/* Write to each page */
		for (i = 0; i < 4; i++) {
			*((int *)(va + i * PAGE_SIZE)) = 0xBEEF0000 + i;
		}
		/* Read back */
		for (i = 0; i < 4; i++) {
			int val = *((int *)(va + i * PAGE_SIZE));

			MI_INFO("    page[%d] written 0x%x read 0x%x %s\n",
				i, 0xBEEF0000 + i, val,
				val == 0xBEEF0000 + i ? "✓" : "✗");
		}

		vunmap(va);
		MI_INFO("  vunmap() ✓\n");
	} else {
		MI_INFO("  vmap() FAILED ✗\n");
	}

	for (i = 0; i < 4; i++)
		__free_page(pages[i]);
}

static int __init vmalloc_inspect_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 13: Vmalloc Inspector       #\n");
	MI_INFO("# Phase 13 — Virtual Memory Alloc    #\n");
	MI_INFO("######################################\n");

	MI_INFO("VMALLOC_START = 0x%016lx\n", VMALLOC_START);
	MI_INFO("VMALLOC_END   = 0x%016lx\n", VMALLOC_END);
	MI_INFO("VMALLOC size  = %lu GB\n",
		(VMALLOC_END - VMALLOC_START) >> 30);

	MI_INFO("\nAddress space layout:\n");
	MI_INFO("  PAGE_OFFSET    = 0x%016lx (linear map start)\n",
		PAGE_OFFSET);
	MI_INFO("  VMALLOC_START  = 0x%016lx\n", VMALLOC_START);
	MI_INFO("  VMALLOC_END    = 0x%016lx\n", VMALLOC_END);
	MI_INFO("  MODULES_VADDR  = 0x%016lx (module space)\n",
		MODULES_VADDR);
	MI_INFO("  MODULES_END    = 0x%016lx\n", MODULES_END);

	test_vmalloc();
	test_vmap();

	MI_INFO("\n[NOTE] Check /proc/vmallocinfo for full vmap_area list\n");
	MI_INFO("[NOTE] Check /proc/meminfo for VmallocTotal/Used/Chunk\n");

	return 0;
}

static void __exit vmalloc_inspect_exit(void)
{
	MI_INFO("Module 13 unloaded\n");
}

module_init(vmalloc_inspect_init);
module_exit(vmalloc_inspect_exit);
