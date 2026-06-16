/* SPDX-License-Identifier: GPL-2.0 */
/*
 * common.h — Shared macros for ARM64 MM inspection modules
 *
 * Provides inline helpers to read ARM64 system registers and
 * decode bit fields. Used by all 13 inspection modules.
 */
#ifndef _MM_INSPECT_COMMON_H
#define _MM_INSPECT_COMMON_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <asm/sysreg.h>
#include <asm/cpufeature.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/pgtable-hwdef.h>

#define TAG "mm_inspect"
#define MI_INFO(fmt, ...) pr_info(TAG ": " fmt, ##__VA_ARGS__)
#define MI_WARN(fmt, ...) pr_warn(TAG ": " fmt, ##__VA_ARGS__)

/* ── System register read helpers ─────────────────────────────── */

static inline u64 mi_read_sctlr_el1(void)
{
	return read_sysreg(sctlr_el1);
}

static inline u64 mi_read_tcr_el1(void)
{
	return read_sysreg(tcr_el1);
}

static inline u64 mi_read_mair_el1(void)
{
	return read_sysreg(mair_el1);
}

static inline u64 mi_read_ttbr0_el1(void)
{
	return read_sysreg(ttbr0_el1);
}

static inline u64 mi_read_ttbr1_el1(void)
{
	return read_sysreg(ttbr1_el1);
}

static inline u64 mi_read_id_aa64mmfr0(void)
{
	return read_sysreg(id_aa64mmfr0_el1);
}

static inline u64 mi_read_midr_el1(void)
{
	return read_sysreg(midr_el1);
}

static inline u64 mi_read_vbar_el1(void)
{
	return read_sysreg(vbar_el1);
}

static inline u64 mi_read_tpidr_el1(void)
{
	return read_sysreg(tpidr_el1);
}

static inline u64 mi_read_sp_el0(void)
{
	return read_sysreg(sp_el0);
}

/* ── Bit extraction helpers ───────────────────────────────────── */

#define FIELD(val, hi, lo) (((val) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))
#define BIT_VAL(val, pos)  (((val) >> (pos)) & 1)

/* ── Page table entry decode helpers ──────────────────────────── */

static inline const char *mi_pte_type_str(u64 pte, int level)
{
	if (!(pte & 1))
		return "INVALID";
	if (level == 3)
		return (pte & 2) ? "PAGE" : "RESERVED";
	return (pte & 2) ? "TABLE" : "BLOCK";
}

static inline const char *mi_mair_attr_str(unsigned int attr)
{
	switch (attr) {
	case 0x00: return "Device-nGnRnE";
	case 0x04: return "Device-nGnRE";
	case 0x0c: return "Device-GRE";
	case 0x44: return "Normal-NC";
	case 0xff: return "Normal-WB-RWAlloc";
	case 0xbb: return "Normal-WT";
	case 0xf0: return "Normal-Tagged";
	default:   return "custom";
	}
}

static inline const char *mi_share_str(unsigned int sh)
{
	switch (sh) {
	case 0: return "Non-shareable";
	case 2: return "Outer-shareable";
	case 3: return "Inner-shareable";
	default: return "Reserved";
	}
}

static inline const char *mi_ap_str(unsigned int ap)
{
	switch (ap) {
	case 0: return "RW_EL1";
	case 1: return "RW_EL1_EL0";
	case 2: return "RO_EL1";
	case 3: return "RO_EL1_EL0";
	default: return "?";
	}
}

/* Print one PTE with full bit decode */
static inline void mi_decode_pte(const char *prefix, u64 pte, int level)
{
	MI_INFO("%sPTE = 0x%016llx\n", prefix, pte);
	MI_INFO("%s  Valid[0]     = %llu\n", prefix, BIT_VAL(pte, 0));
	MI_INFO("%s  Type[1]     = %llu (%s)\n", prefix,
		BIT_VAL(pte, 1), mi_pte_type_str(pte, level));
	MI_INFO("%s  AttrIndx[4:2] = %llu\n", prefix, FIELD(pte, 4, 2));
	MI_INFO("%s  NS[5]       = %llu\n", prefix, BIT_VAL(pte, 5));
	MI_INFO("%s  AP[7:6]     = %llu (%s)\n", prefix,
		FIELD(pte, 7, 6), mi_ap_str(FIELD(pte, 7, 6)));
	MI_INFO("%s  SH[9:8]     = %llu (%s)\n", prefix,
		FIELD(pte, 9, 8), mi_share_str(FIELD(pte, 9, 8)));
	MI_INFO("%s  AF[10]      = %llu\n", prefix, BIT_VAL(pte, 10));
	MI_INFO("%s  nG[11]      = %llu\n", prefix, BIT_VAL(pte, 11));
	if (level < 3 && !(pte & 2)) {
		/* Block descriptor: OA is [47:21] for PMD, [47:30] for PUD */
		MI_INFO("%s  OA(block)   = 0x%llx\n", prefix,
			pte & 0x0000ffffffe00000ULL);
	} else if (level == 3) {
		MI_INFO("%s  OA(page)    = 0x%llx\n", prefix,
			pte & 0x0000fffffffff000ULL);
	} else {
		MI_INFO("%s  NextLevel   = 0x%llx\n", prefix,
			pte & 0x0000fffffffff000ULL);
	}
	MI_INFO("%s  Contiguous[52] = %llu\n", prefix, BIT_VAL(pte, 52));
	MI_INFO("%s  PXN[53]     = %llu\n", prefix, BIT_VAL(pte, 53));
	MI_INFO("%s  UXN[54]     = %llu\n", prefix, BIT_VAL(pte, 54));
}

#endif /* _MM_INSPECT_COMMON_H */
