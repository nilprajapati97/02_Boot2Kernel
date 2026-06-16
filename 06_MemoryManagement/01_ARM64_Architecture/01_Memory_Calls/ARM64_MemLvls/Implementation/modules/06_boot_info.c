// SPDX-License-Identifier: GPL-2.0
/*
 * 06_boot_info.c — Phase 6: Boot Information Inspector
 *
 * Prints boot-time state established by __primary_switched:
 *   - __boot_cpu_mode (EL1 vs EL2)
 *   - __fdt_pointer (FDT physical address)
 *   - kimage_voffset (VA-PA offset)
 *   - init_task details
 *   - VBAR_EL1, SP_EL0, TPIDR_EL1, CurrentEL
 *   - MIDR_EL1 (CPU identification — implementer, part number)
 *
 * Maps to: 06_Primary_Switched/01_Init_CPU_Task.md, 02_Transition_To_C.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <asm/memory.h>
#include <asm/sysreg.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 6: Boot info — mode, FDT, kimage, MIDR");
MODULE_AUTHOR("MM Inspector");

extern phys_addr_t __fdt_pointer;
extern s64 kimage_voffset;

static void decode_midr(void)
{
	u64 midr = mi_read_midr_el1();

	MI_INFO("MIDR_EL1 = 0x%016llx\n", midr);
	MI_INFO("  Implementer [31:24] = 0x%llx", FIELD(midr, 31, 24));
	switch (FIELD(midr, 31, 24)) {
	case 0x41: MI_INFO(" (ARM)\n"); break;
	case 0x42: MI_INFO(" (Broadcom)\n"); break;
	case 0x43: MI_INFO(" (Cavium)\n"); break;
	case 0x44: MI_INFO(" (DEC)\n"); break;
	case 0x46: MI_INFO(" (Fujitsu)\n"); break;
	case 0x48: MI_INFO(" (HiSilicon)\n"); break;
	case 0x4e: MI_INFO(" (NVIDIA)\n"); break;
	case 0x50: MI_INFO(" (APM)\n"); break;
	case 0x51: MI_INFO(" (Qualcomm)\n"); break;
	case 0x53: MI_INFO(" (Samsung)\n"); break;
	case 0x56: MI_INFO(" (Marvell)\n"); break;
	case 0x61: MI_INFO(" (Apple)\n"); break;
	case 0x69: MI_INFO(" (Intel)\n"); break;
	default:   MI_INFO(" (unknown)\n"); break;
	}
	MI_INFO("  Variant     [23:20] = 0x%llx\n", FIELD(midr, 23, 20));
	MI_INFO("  Arch        [19:16] = 0x%llx", FIELD(midr, 19, 16));
	if (FIELD(midr, 19, 16) == 0xf)
		MI_INFO(" (ARMv8/defined by CPUID)\n");
	else
		MI_INFO("\n");
	MI_INFO("  PartNum     [15:4]  = 0x%llx", FIELD(midr, 15, 4));
	/* Common part numbers */
	switch (FIELD(midr, 15, 4)) {
	case 0xd03: MI_INFO(" (Cortex-A53)\n"); break;
	case 0xd04: MI_INFO(" (Cortex-A35)\n"); break;
	case 0xd05: MI_INFO(" (Cortex-A55)\n"); break;
	case 0xd07: MI_INFO(" (Cortex-A57)\n"); break;
	case 0xd08: MI_INFO(" (Cortex-A72)\n"); break;
	case 0xd09: MI_INFO(" (Cortex-A73)\n"); break;
	case 0xd0a: MI_INFO(" (Cortex-A75)\n"); break;
	case 0xd0b: MI_INFO(" (Cortex-A76)\n"); break;
	case 0xd0d: MI_INFO(" (Cortex-A77)\n"); break;
	case 0xd40: MI_INFO(" (Neoverse-V1)\n"); break;
	case 0xd41: MI_INFO(" (Cortex-A78)\n"); break;
	case 0xd44: MI_INFO(" (Cortex-X1)\n"); break;
	case 0x800: MI_INFO(" (Kryo 260/280 Gold)\n"); break;
	case 0x801: MI_INFO(" (Kryo 260/280 Silver)\n"); break;
	case 0x802: MI_INFO(" (Kryo 385 Gold)\n"); break;
	default:    MI_INFO(" (check ARM TRM)\n"); break;
	}
	MI_INFO("  Revision    [3:0]   = 0x%llx\n", FIELD(midr, 3, 0));

	/* SDM660 note */
	MI_INFO("  [NOTE] SDM660 uses Kryo 260: A73-based (Gold) + A53-based (Silver)\n");
	MI_INFO("  [NOTE] QEMU cortex-a72 reports PartNum=0xd08 (close to A73=0xd09)\n");
}

static int __init boot_info_init(void)
{
	u64 vbar, sp_el0, tpidr;

	MI_INFO("######################################\n");
	MI_INFO("# Module 06: Boot Info Inspector     #\n");
	MI_INFO("# Phase 6 — Primary Switched State   #\n");
	MI_INFO("######################################\n");

	/* CPU identification */
	decode_midr();

	/* FDT pointer */
	MI_INFO("\n__fdt_pointer = 0x%llx (FDT physical address)\n",
		(u64)__fdt_pointer);

	/* kimage offset */
	MI_INFO("kimage_voffset = 0x%llx (kernel VA - PA offset)\n",
		(u64)kimage_voffset);
	MI_INFO("  _text VA = 0x%lx\n", (unsigned long)_text);
	MI_INFO("  _text PA = 0x%llx (computed: VA - voffset)\n",
		(u64)((unsigned long)_text - kimage_voffset));

	/* System registers */
	vbar = mi_read_vbar_el1();
	sp_el0 = mi_read_sp_el0();
	tpidr = mi_read_tpidr_el1();

	MI_INFO("\nVBAR_EL1   = 0x%016llx (exception vector table)\n", vbar);
	MI_INFO("SP_EL0     = 0x%016llx (current task ptr)\n", sp_el0);
	MI_INFO("TPIDR_EL1  = 0x%016llx (per-CPU offset)\n", tpidr);

	/* init_task info */
	MI_INFO("\ninit_task:\n");
	MI_INFO("  address    = %px\n", &init_task);
	MI_INFO("  comm       = %s\n", init_task.comm);
	MI_INFO("  pid        = %d\n", init_task.pid);
	MI_INFO("  stack      = %px\n", init_task.stack);

	return 0;
}

static void __exit boot_info_exit(void)
{
	MI_INFO("Module 06 unloaded\n");
}

module_init(boot_info_init);
module_exit(boot_info_exit);
