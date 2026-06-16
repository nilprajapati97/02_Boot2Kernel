```c
static void __init setup_processor(void)
{
	unsigned int midr = read_cpuid_id();
	struct proc_info_list *list = lookup_processor(midr);

	cpu_name = list->cpu_name;
	__cpu_architecture = __get_cpu_architecture();

	init_proc_vtable(list->proc);
#ifdef MULTI_TLB
	cpu_tlb = *list->tlb;
#endif
#ifdef MULTI_USER
	cpu_user = *list->user;
#endif
#ifdef MULTI_CACHE
	cpu_cache = *list->cache;
#endif

	pr_info("CPU: %s [%08x] revision %d (ARMv%s), cr=%08lx\n",
		list->cpu_name, midr, midr & 15,
		proc_arch[cpu_architecture()], get_cr());

	snprintf(init_utsname()->machine, __NEW_UTS_LEN + 1, "%s%c",
		 list->arch_name, ENDIANNESS);
	snprintf(elf_platform, ELF_PLATFORM_SIZE, "%s%c",
		 list->elf_name, ENDIANNESS);
	elf_hwcap = list->elf_hwcap;

	cpuid_init_hwcaps();
	patch_aeabi_idiv();

#ifndef CONFIG_ARM_THUMB
	elf_hwcap &= ~(HWCAP_THUMB | HWCAP_IDIVT);
#endif
#ifdef CONFIG_MMU
	init_default_cache_policy(list->__cpu_mm_mmu_flags);
#endif
	erratum_a15_798181_init();

	elf_hwcap_fixup();

	cacheid_init();
	cpu_init();
}
```
