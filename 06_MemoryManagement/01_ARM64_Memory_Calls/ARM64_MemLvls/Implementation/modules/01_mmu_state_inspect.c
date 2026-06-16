// SPDX-License-Identifier: GPL-2.0
/*
 * 01_mmu_state_inspect.c — Phase 1,3,4: MMU Register State Inspector
 *
 * Reads and decodes bit-by-bit:
 *   - SCTLR_EL1  (System Control Register)
 *   - TCR_EL1    (Translation Control Register)
 *   - MAIR_EL1   (Memory Attribute Indirection Register)
 *   - TTBR0_EL1  (Translation Table Base Register 0)
 *   - TTBR1_EL1  (Translation Table Base Register 1)
 *   - ID_AA64MMFR0_EL1 (Memory Model Feature Register 0)
 *
 * Maps to documentation:
 *   01_Primary_Entry/01_Record_MMU_State.md
 *   03_CPU_Setup/01_TCR_EL1.md, 02_MAIR_EL1.md
 *   04_Enable_MMU/01_TTBR_Setup.md
 */

#include <linux/init.h>
#include <linux/module.h>
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 1,3,4: MMU register bit-by-bit decode");
MODULE_AUTHOR("MM Inspector");

static void decode_sctlr(void)
{
	u64 val = mi_read_sctlr_el1();

	MI_INFO("============================================\n");
	MI_INFO("SCTLR_EL1 = 0x%016llx\n", val);
	MI_INFO("============================================\n");
	MI_INFO("  M    [0]  = %llu  (MMU %s)\n",
		BIT_VAL(val, 0), BIT_VAL(val, 0) ? "ON" : "OFF");
	MI_INFO("  A    [1]  = %llu  (Alignment check)\n", BIT_VAL(val, 1));
	MI_INFO("  C    [2]  = %llu  (D-cache %s)\n",
		BIT_VAL(val, 2), BIT_VAL(val, 2) ? "ON" : "OFF");
	MI_INFO("  SA   [3]  = %llu  (SP align EL1)\n", BIT_VAL(val, 3));
	MI_INFO("  SA0  [4]  = %llu  (SP align EL0)\n", BIT_VAL(val, 4));
	MI_INFO("  CP15BEN [5] = %llu\n", BIT_VAL(val, 5));
	MI_INFO("  nAA  [6]  = %llu  (Non-aligned access)\n", BIT_VAL(val, 6));
	MI_INFO("  ITD  [7]  = %llu  (IT disable)\n", BIT_VAL(val, 7));
	MI_INFO("  SED  [8]  = %llu  (SETEND disable)\n", BIT_VAL(val, 8));
	MI_INFO("  UMA  [9]  = %llu  (User Mask Access)\n", BIT_VAL(val, 9));
	MI_INFO("  EnRCTX [10] = %llu\n", BIT_VAL(val, 10));
	MI_INFO("  EOS  [11] = %llu\n", BIT_VAL(val, 11));
	MI_INFO("  I    [12] = %llu  (I-cache %s)\n",
		BIT_VAL(val, 12), BIT_VAL(val, 12) ? "ON" : "OFF");
	MI_INFO("  EnDB [13] = %llu  (PAC DB)\n", BIT_VAL(val, 13));
	MI_INFO("  DZE  [14] = %llu  (DC ZVA EL0)\n", BIT_VAL(val, 14));
	MI_INFO("  UCT  [15] = %llu  (CTR_EL0 at EL0)\n", BIT_VAL(val, 15));
	MI_INFO("  nTWI [16] = %llu  (WFI trap)\n", BIT_VAL(val, 16));
	MI_INFO("  nTWE [18] = %llu  (WFE trap)\n", BIT_VAL(val, 18));
	MI_INFO("  WXN  [19] = %llu  (Write-Execute-Never)\n", BIT_VAL(val, 19));
	MI_INFO("  TSCXT [20] = %llu\n", BIT_VAL(val, 20));
	MI_INFO("  IESB [21] = %llu\n", BIT_VAL(val, 21));
	MI_INFO("  EIS  [22] = %llu\n", BIT_VAL(val, 22));
	MI_INFO("  SPAN [23] = %llu\n", BIT_VAL(val, 23));
	MI_INFO("  E0E  [24] = %llu  (EL0 endian)\n", BIT_VAL(val, 24));
	MI_INFO("  EE   [25] = %llu  (EL1 endian: %s)\n",
		BIT_VAL(val, 25), BIT_VAL(val, 25) ? "Big" : "Little");
	MI_INFO("  UCI  [26] = %llu\n", BIT_VAL(val, 26));
	MI_INFO("  EnDA [27] = %llu  (PAC DA)\n", BIT_VAL(val, 27));
	MI_INFO("  nTLSMD [28] = %llu\n", BIT_VAL(val, 28));
	MI_INFO("  LSMAOE [29] = %llu\n", BIT_VAL(val, 29));
	MI_INFO("  EnIB [30] = %llu  (PAC IB)\n", BIT_VAL(val, 30));
	MI_INFO("  EnIA [31] = %llu  (PAC IA)\n", BIT_VAL(val, 31));
	MI_INFO("  --- Summary: MMU=%s D$=%s I$=%s Endian=%s ---\n",
		BIT_VAL(val, 0) ? "ON" : "OFF",
		BIT_VAL(val, 2) ? "ON" : "OFF",
		BIT_VAL(val, 12) ? "ON" : "OFF",
		BIT_VAL(val, 25) ? "BE" : "LE");
}

static void decode_tcr(void)
{
	u64 val = mi_read_tcr_el1();
	u64 t0sz, t1sz, tg0, tg1, ips, sh0, sh1;
	static const char * const ips_str[] = {
		"32", "36", "40", "42", "44", "48", "52"
	};
	static const char * const tg0_str[] = {
		"4KB", "64KB", "16KB", "Reserved"
	};
	static const char * const tg1_str[] = {
		"Invalid", "16KB", "4KB", "64KB"
	};

	t0sz = FIELD(val, 5, 0);
	t1sz = FIELD(val, 21, 16);
	tg0  = FIELD(val, 15, 14);
	tg1  = FIELD(val, 31, 30);
	ips  = FIELD(val, 34, 32);
	sh0  = FIELD(val, 13, 12);
	sh1  = FIELD(val, 29, 28);

	MI_INFO("============================================\n");
	MI_INFO("TCR_EL1 = 0x%016llx\n", val);
	MI_INFO("============================================\n");
	MI_INFO("  T0SZ    [5:0]   = %llu  -> TTBR0 VA bits = %llu\n", t0sz, 64 - t0sz);
	MI_INFO("  EPD0    [7]     = %llu  (TTBR0 walk %s)\n",
		BIT_VAL(val, 7), BIT_VAL(val, 7) ? "DISABLED" : "enabled");
	MI_INFO("  IRGN0   [9:8]   = %llu  (inner cache)\n", FIELD(val, 9, 8));
	MI_INFO("  ORGN0   [11:10] = %llu  (outer cache)\n", FIELD(val, 11, 10));
	MI_INFO("  SH0     [13:12] = %llu  (%s)\n", sh0, mi_share_str(sh0));
	MI_INFO("  TG0     [15:14] = %llu  (%s)\n", tg0,
		tg0 < 4 ? tg0_str[tg0] : "?");
	MI_INFO("  T1SZ    [21:16] = %llu  -> TTBR1 VA bits = %llu\n", t1sz, 64 - t1sz);
	MI_INFO("  A1      [22]    = %llu  (ASID from TTBR%llu)\n",
		BIT_VAL(val, 22), BIT_VAL(val, 22));
	MI_INFO("  EPD1    [23]    = %llu  (TTBR1 walk %s)\n",
		BIT_VAL(val, 23), BIT_VAL(val, 23) ? "DISABLED" : "enabled");
	MI_INFO("  IRGN1   [25:24] = %llu  (inner cache)\n", FIELD(val, 25, 24));
	MI_INFO("  ORGN1   [27:26] = %llu  (outer cache)\n", FIELD(val, 27, 26));
	MI_INFO("  SH1     [29:28] = %llu  (%s)\n", sh1, mi_share_str(sh1));
	MI_INFO("  TG1     [31:30] = %llu  (%s)\n", tg1,
		tg1 < 4 ? tg1_str[tg1] : "?");
	MI_INFO("  IPS     [34:32] = %llu  (%s bits PA)\n", ips,
		ips < 7 ? ips_str[ips] : "?");
	MI_INFO("  AS      [36]    = %llu  (%s-bit ASID)\n",
		BIT_VAL(val, 36), BIT_VAL(val, 36) ? "16" : "8");
	MI_INFO("  TBI0    [37]    = %llu  (Top Byte Ignore TTBR0)\n", BIT_VAL(val, 37));
	MI_INFO("  TBI1    [38]    = %llu  (Top Byte Ignore TTBR1)\n", BIT_VAL(val, 38));
	MI_INFO("  HA      [39]    = %llu  (HW Access Flag %s)\n",
		BIT_VAL(val, 39), BIT_VAL(val, 39) ? "ON" : "OFF");
	MI_INFO("  HD      [40]    = %llu  (HW Dirty Bit %s)\n",
		BIT_VAL(val, 40), BIT_VAL(val, 40) ? "ON" : "OFF");
	MI_INFO("  HPD0    [41]    = %llu\n", BIT_VAL(val, 41));
	MI_INFO("  HPD1    [42]    = %llu\n", BIT_VAL(val, 42));
	MI_INFO("  TBID0   [51]    = %llu\n", BIT_VAL(val, 51));
	MI_INFO("  TBID1   [52]    = %llu\n", BIT_VAL(val, 52));
	MI_INFO("  DS      [59]    = %llu  (LPA2)\n", BIT_VAL(val, 59));
	MI_INFO("  --- Summary: VA0=%llub VA1=%llub PA=%sb Gran0=%s Gran1=%s ---\n",
		64 - t0sz, 64 - t1sz,
		ips < 7 ? ips_str[ips] : "?",
		tg0 < 4 ? tg0_str[tg0] : "?",
		tg1 < 4 ? tg1_str[tg1] : "?");
}

static void decode_mair(void)
{
	u64 val = mi_read_mair_el1();
	int i;

	MI_INFO("============================================\n");
	MI_INFO("MAIR_EL1 = 0x%016llx\n", val);
	MI_INFO("============================================\n");

	for (i = 0; i < 8; i++) {
		unsigned int attr = (val >> (i * 8)) & 0xff;

		MI_INFO("  Attr%d = 0x%02x  (%s)\n", i, attr,
			mi_mair_attr_str(attr));
		MI_INFO("    outer[7:4] = 0x%x  inner[3:0] = 0x%x\n",
			(attr >> 4) & 0xf, attr & 0xf);
	}
}

static void decode_ttbr(void)
{
	u64 ttbr0 = mi_read_ttbr0_el1();
	u64 ttbr1 = mi_read_ttbr1_el1();

	MI_INFO("============================================\n");
	MI_INFO("TTBR0_EL1 = 0x%016llx\n", ttbr0);
	MI_INFO("  BADDR [47:1] = 0x%llx  (page table phys base)\n",
		ttbr0 & 0x0000ffffffffffffULL);
	MI_INFO("  ASID  [63:48] = %llu\n", (ttbr0 >> 48) & 0xffff);
	MI_INFO("TTBR1_EL1 = 0x%016llx\n", ttbr1);
	MI_INFO("  BADDR [47:1] = 0x%llx  (page table phys base)\n",
		ttbr1 & 0x0000ffffffffffffULL);
	MI_INFO("  ASID  [63:48] = %llu\n", (ttbr1 >> 48) & 0xffff);
}

static void decode_mmfr0(void)
{
	u64 val = mi_read_id_aa64mmfr0();
	static const char * const pa_str[] = {
		"32", "36", "40", "42", "44", "48", "52"
	};
	u64 pa;

	pa = FIELD(val, 3, 0);

	MI_INFO("============================================\n");
	MI_INFO("ID_AA64MMFR0_EL1 = 0x%016llx\n", val);
	MI_INFO("============================================\n");
	MI_INFO("  PARange   [3:0]   = %llu  (%s bits)\n", pa,
		pa < 7 ? pa_str[pa] : "?");
	MI_INFO("  ASIDBits  [7:4]   = %llu  (%s-bit ASID)\n",
		FIELD(val, 7, 4),
		FIELD(val, 7, 4) == 2 ? "16" : "8");
	MI_INFO("  BigEnd    [11:8]  = %llu\n", FIELD(val, 11, 8));
	MI_INFO("  SNSMem    [15:12] = %llu\n", FIELD(val, 15, 12));
	MI_INFO("  BigEndEL0 [19:16] = %llu\n", FIELD(val, 19, 16));
	MI_INFO("  TGran16   [23:20] = %llu  (%s)\n",
		FIELD(val, 23, 20),
		FIELD(val, 23, 20) == 1 ? "supported" : "check impl");
	MI_INFO("  TGran64   [27:24] = %llu  (%s)\n",
		FIELD(val, 27, 24),
		FIELD(val, 27, 24) == 0 ? "supported" : "not supported");
	MI_INFO("  TGran4    [31:28] = %llu  (%s)\n",
		FIELD(val, 31, 28),
		FIELD(val, 31, 28) <= 1 ? "supported" : "not supported");
	MI_INFO("  TGran16_2 [35:32] = %llu\n", FIELD(val, 35, 32));
	MI_INFO("  TGran64_2 [39:36] = %llu\n", FIELD(val, 39, 36));
	MI_INFO("  TGran4_2  [43:40] = %llu\n", FIELD(val, 43, 40));
	MI_INFO("  ExS       [47:44] = %llu\n", FIELD(val, 47, 44));

	/* Derived: compute page table levels */
	MI_INFO("  --- Derived ---\n");
	MI_INFO("  Max PA: %s bits\n", pa < 7 ? pa_str[pa] : "?");
	MI_INFO("  4KB granule: %s\n",
		FIELD(val, 31, 28) <= 1 ? "YES" : "NO");
}

static int __init mmu_state_init(void)
{
	MI_INFO("######################################\n");
	MI_INFO("# Module 01: MMU State Inspector     #\n");
	MI_INFO("# Phase 1,3,4 — Register Decode      #\n");
	MI_INFO("######################################\n");

	decode_sctlr();
	decode_tcr();
	decode_mair();
	decode_ttbr();
	decode_mmfr0();

	return 0;
}

static void __exit mmu_state_exit(void)
{
	MI_INFO("Module 01 unloaded\n");
}

module_init(mmu_state_init);
module_exit(mmu_state_exit);
