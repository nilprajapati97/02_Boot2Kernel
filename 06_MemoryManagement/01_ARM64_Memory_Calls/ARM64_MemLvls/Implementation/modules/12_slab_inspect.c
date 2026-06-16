// SPDX-License-Identifier: GPL-2.0
/*
 * 12_slab_inspect.c — Phase 12: SLAB/SLUB Allocator Inspector
 *
 * Lists key kmem_caches with object size, slab size, alignment.
 * Tests kmalloc/kfree, creates and destroys a custom cache.
 * Inspects slub_debug poisoning/redzone patterns.
 *
 * Maps to: 12_SLAB_SLUB/01_Kmem_Cache_Init.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/list.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 12: SLAB/SLUB cache inspector and test");
MODULE_AUTHOR("MM Inspector");

/* Custom cache for testing */
struct mi_test_obj {
	u64 field_a;
	u32 field_b;
	char name[20];
};

static struct kmem_cache *mi_test_cache;

static void dump_slab_caches(void)
{
	struct kmem_cache *s;
	int count = 0;

	MI_INFO("--- Key SLUB Caches ---\n");
	MI_INFO("  %-30s  %6s  %6s  %6s\n",
		"Name", "ObjSz", "Size", "Align");

	list_for_each_entry(s, &slab_caches, list) {
		/* Print first 30 and any interesting ones */
		if (count < 30 ||
		    strstr(s->name, "kmalloc") ||
		    strstr(s->name, "task_struct") ||
		    strstr(s->name, "mm_struct") ||
		    strstr(s->name, "vm_area") ||
		    strstr(s->name, "dentry") ||
		    strstr(s->name, "inode") ||
		    strstr(s->name, "filp")) {
			MI_INFO("  %-30s  %6u  %6u  %6u\n",
				s->name,
				s->object_size,
				s->size,
				s->align);
		}
		count++;
	}

	MI_INFO("  Total caches: %d\n", count);
}

static void test_kmalloc(void)
{
	void *p;
	int i;
	static const int sizes[] = { 8, 32, 64, 128, 256, 512, 1024, 4096 };

	MI_INFO("\n--- kmalloc Tests ---\n");

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		p = kmalloc(sizes[i], GFP_KERNEL);
		if (p) {
			struct kmem_cache *slab = virt_to_cache(p);

			MI_INFO("  kmalloc(%4d) → %px  cache='%s' (objsz=%u, slabsz=%u)\n",
				sizes[i], p,
				slab ? slab->name : "?",
				slab ? slab->object_size : 0,
				slab ? slab->size : 0);
			kfree(p);
		} else {
			MI_INFO("  kmalloc(%4d) → FAILED ✗\n", sizes[i]);
		}
	}

	/* Test kzalloc zero verification */
	p = kzalloc(128, GFP_KERNEL);
	if (p) {
		u8 *bytes = p;
		int all_zero = 1;
		int j;

		for (j = 0; j < 128; j++) {
			if (bytes[j] != 0) {
				all_zero = 0;
				break;
			}
		}
		MI_INFO("  kzalloc(128): zeroed=%s\n",
			all_zero ? "✓ YES" : "✗ NO");
		kfree(p);
	}
}

static void test_custom_cache(void)
{
	struct mi_test_obj *obj;

	MI_INFO("\n--- Custom Cache Test ---\n");

	mi_test_cache = kmem_cache_create(
		"mi_test_cache",
		sizeof(struct mi_test_obj),
		__alignof__(struct mi_test_obj),
		SLAB_HWCACHE_ALIGN | SLAB_PANIC,
		NULL);

	if (!mi_test_cache) {
		MI_INFO("  kmem_cache_create FAILED ✗\n");
		return;
	}

	MI_INFO("  Created cache '%s'\n", mi_test_cache->name);
	MI_INFO("    object_size = %u\n", mi_test_cache->object_size);
	MI_INFO("    size        = %u (with metadata)\n", mi_test_cache->size);
	MI_INFO("    align       = %u\n", mi_test_cache->align);

	/* Allocate an object */
	obj = kmem_cache_alloc(mi_test_cache, GFP_KERNEL);
	if (obj) {
		obj->field_a = 0xDEADBEEFCAFEBABEULL;
		obj->field_b = 42;
		strscpy(obj->name, "test_object", sizeof(obj->name));

		MI_INFO("  Allocated object at %px\n", obj);
		MI_INFO("    field_a = 0x%llx\n", obj->field_a);
		MI_INFO("    field_b = %u\n", obj->field_b);
		MI_INFO("    name    = '%s'\n", obj->name);

		kmem_cache_free(mi_test_cache, obj);
		MI_INFO("  Object freed ✓\n");
	} else {
		MI_INFO("  kmem_cache_alloc FAILED ✗\n");
	}

	kmem_cache_destroy(mi_test_cache);
	mi_test_cache = NULL;
	MI_INFO("  Cache destroyed ✓\n");
}

static void inspect_slub_debug(void)
{
	void *p;

	MI_INFO("\n--- SLUB Debug Inspection ---\n");
	MI_INFO("  slub_debug flags control:\n");
	MI_INFO("    F = Sanity checks (SLAB_DEBUG_CONSISTENCY_CHECKS)\n");
	MI_INFO("    Z = Red zoning (SLAB_RED_ZONE)\n");
	MI_INFO("    P = Poisoning (SLAB_POISON)\n");
	MI_INFO("    U = User tracking (SLAB_STORE_USER)\n");
	MI_INFO("    T = Tracing (SLAB_TRACE)\n");

	/* Allocate and free to see poison patterns */
	p = kmalloc(64, GFP_KERNEL);
	if (p) {
		MI_INFO("  Allocated 64B at %px\n", p);
		MI_INFO("    First 8 bytes: %016llx\n", *(u64 *)p);
		kfree(p);
		/* After free, poison pattern (0x6b) should appear if SLAB_POISON */
		MI_INFO("    After free, addr was %px\n", p);
		MI_INFO("    (Check /sys/kernel/slab/ for detailed per-cache stats)\n");
	}
}

static int __init slab_inspect_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 12: SLAB/SLUB Inspector     #\n");
	MI_INFO("# Phase 12 — kmem_cache & kmalloc    #\n");
	MI_INFO("######################################\n");

	dump_slab_caches();
	test_kmalloc();
	test_custom_cache();
	inspect_slub_debug();

	return 0;
}

static void __exit slab_inspect_exit(void)
{
	if (mi_test_cache) {
		kmem_cache_destroy(mi_test_cache);
		mi_test_cache = NULL;
	}
	MI_INFO("Module 12 unloaded\n");
}

module_init(slab_inspect_init);
module_exit(slab_inspect_exit);
