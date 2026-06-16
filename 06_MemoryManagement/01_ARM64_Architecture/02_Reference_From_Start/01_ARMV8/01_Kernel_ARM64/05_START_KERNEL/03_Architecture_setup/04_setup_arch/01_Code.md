```c
void __init setup_arch(char **cmdline_p)
{
	const struct machine_desc *mdesc = NULL;
	void *atags_vaddr = NULL;

	if (__atags_pointer)
		atags_vaddr = FDT_VIRT_BASE(__atags_pointer);

	setup_processor();
	if (atags_vaddr) {
		mdesc = setup_machine_fdt(atags_vaddr);
		if (mdesc)
			memblock_reserve(__atags_pointer,
					 fdt_totalsize(atags_vaddr));
	}
	if (!mdesc)
		mdesc = setup_machine_tags(atags_vaddr, __machine_arch_type);
	if (!mdesc) {
		early_print("\nError: invalid dtb and unrecognized/unsupported machine ID\n");
		early_print("  r1=0x%08x, r2=0x%08x\n", __machine_arch_type,
			    __atags_pointer);
		if (__atags_pointer)
			early_print("  r2[]=%*ph\n", 16, atags_vaddr);
		dump_machine_table();
	}

	machine_desc = mdesc;
	machine_name = mdesc->name;
	dump_stack_set_arch_desc("%s", mdesc->name);

	if (mdesc->reboot_mode != REBOOT_HARD)
		reboot_mode = mdesc->reboot_mode;

	setup_initial_init_mm(_text, _etext, _edata, _end);

	/* populate cmd_line too for later use, preserving boot_command_line */
	strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = cmd_line;

	early_fixmap_init();
	early_ioremap_init();

	parse_early_param();

#ifdef CONFIG_MMU
	early_mm_init(mdesc);
#endif
	setup_dma_zone(mdesc);
	xen_early_init();
	arm_efi_init();
	/*
	 * Make sure the calculation for lowmem/highmem is set appropriately
	 * before reserving/allocating any memory
	 */
	adjust_lowmem_bounds();
	arm_memblock_init(mdesc);
	/* Memory may have been removed so recalculate the bounds. */
	adjust_lowmem_bounds();

	early_ioremap_reset();

	paging_init(mdesc);
	kasan_init();
	request_standard_resources(mdesc);

	if (mdesc->restart) {
		__arm_pm_restart = mdesc->restart;
		register_restart_handler(&arm_restart_nb);
	}

	unflatten_device_tree();

	arm_dt_init_cpu_maps();
	psci_dt_init();
#ifdef CONFIG_SMP
	if (is_smp()) {
		if (!mdesc->smp_init || !mdesc->smp_init()) {
			if (psci_smp_available())
				smp_set_ops(&psci_smp_ops);
			else if (mdesc->smp)
				smp_set_ops(mdesc->smp);
		}
		smp_init_cpus();
		smp_build_mpidr_hash();
	}
#endif

	if (!is_smp())
		hyp_mode_check();

	reserve_crashkernel();

#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	vgacon_register_screen(&vgacon_screen_info);
#endif
#endif

	if (mdesc->init_early)
		mdesc->init_early();
}
```
