#!/usr/bin/env python3
"""
register_decode.py — GDB Python script for bit-level ARM64 register decode.
Load in GDB: source gdb/register_decode.py
Usage: decode-sctlr, decode-tcr, decode-mair, decode-ttbr0, decode-ttbr1, decode-mmfr0
"""

import gdb


def read_sysreg(name):
    """Read an ARM64 system register via GDB."""
    try:
        val = gdb.parse_and_eval(f"${name}")
        return int(val)
    except gdb.error:
        # Try reading via info registers
        try:
            output = gdb.execute(f"info registers {name}", to_string=True)
            parts = output.split()
            for p in parts:
                if p.startswith("0x"):
                    return int(p, 16)
        except gdb.error:
            pass
    return None


def bit(val, pos):
    return (val >> pos) & 1


def bits(val, hi, lo):
    mask = (1 << (hi - lo + 1)) - 1
    return (val >> lo) & mask


class DecodeSCTLR(gdb.Command):
    """Decode SCTLR_EL1 bit-by-bit."""

    def __init__(self):
        super().__init__("decode-sctlr", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if arg:
            val = int(gdb.parse_and_eval(arg))
        else:
            val = read_sysreg("SCTLR_EL1")
            if val is None:
                print("Cannot read SCTLR_EL1. Provide value: decode-sctlr 0x...")
                return

        print(f"\n{'='*60}")
        print(f"SCTLR_EL1 = 0x{val:016x}")
        print(f"{'='*60}")

        fields = [
            (0, "M", "MMU Enable"),
            (1, "A", "Alignment check"),
            (2, "C", "Data cache enable"),
            (3, "SA", "SP Alignment check EL1"),
            (4, "SA0", "SP Alignment check EL0"),
            (5, "CP15BEN", "CP15 barrier enable"),
            (6, "nAA", "Non-aligned access"),
            (7, "ITD", "IT disable"),
            (8, "SED", "SETEND disable"),
            (9, "UMA", "User Mask Access"),
            (10, "EnRCTX", "Enable EL0 RCTX"),
            (11, "EOS", "Exception Option Set"),
            (12, "I", "Instruction cache enable"),
            (13, "EnDB", "Enable PAC DB"),
            (14, "DZE", "DC ZVA at EL0"),
            (15, "UCT", "CTR_EL0 access at EL0"),
            (16, "nTWI", "WFI non-trapping"),
            (18, "nTWE", "WFE non-trapping"),
            (19, "WXN", "Write-Execute Never"),
            (20, "TSCXT", "Tag check sync"),
            (21, "IESB", "Implicit Error Sync"),
            (22, "EIS", "Exception entry ISB"),
            (23, "SPAN", "Set PAN"),
            (24, "E0E", "EL0 Endianness"),
            (25, "EE", "EL1 Endianness"),
            (26, "UCI", "Cache maintenance EL0"),
            (27, "EnDA", "Enable PAC DA"),
            (28, "nTLSMD", "No Trap LD/ST Multiple"),
            (29, "LSMAOE", "LD/ST Multiple Atomicity"),
            (30, "EnIB", "Enable PAC IB"),
            (31, "EnIA", "Enable PAC IA"),
        ]

        for pos, name, desc in fields:
            v = bit(val, pos)
            state = "ON" if v else "OFF"
            print(f"  [{pos:2d}] {name:10s} = {v}  ({desc}: {state})")

        print(f"\n  Summary: MMU={'ON' if bit(val,0) else 'OFF'}, "
              f"D$={'ON' if bit(val,2) else 'OFF'}, "
              f"I$={'ON' if bit(val,12) else 'OFF'}, "
              f"Endian={'BE' if bit(val,25) else 'LE'}")


class DecodeTCR(gdb.Command):
    """Decode TCR_EL1 bit-by-bit."""

    def __init__(self):
        super().__init__("decode-tcr", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if arg:
            val = int(gdb.parse_and_eval(arg))
        else:
            val = read_sysreg("TCR_EL1")
            if val is None:
                print("Cannot read TCR_EL1. Provide value: decode-tcr 0x...")
                return

        print(f"\n{'='*60}")
        print(f"TCR_EL1 = 0x{val:016x}")
        print(f"{'='*60}")

        t0sz = bits(val, 5, 0)
        t1sz = bits(val, 21, 16)
        tg0 = bits(val, 15, 14)
        tg1 = bits(val, 31, 30)
        ips = bits(val, 34, 32)

        tg0_map = {0: "4KB", 1: "64KB", 2: "16KB", 3: "Reserved"}
        tg1_map = {0: "Invalid", 1: "16KB", 2: "4KB", 3: "64KB"}
        ips_map = {0: "32", 1: "36", 2: "40", 3: "42", 4: "44", 5: "48", 6: "52"}
        sh_map = {0: "Non-shareable", 1: "Reserved", 2: "Outer", 3: "Inner"}

        print(f"  T0SZ    [{5}:{0}]   = {t0sz}  → TTBR0 VA bits = {64-t0sz}")
        print(f"  EPD0    [7]       = {bit(val,7)}  (TTBR0 walk {'DISABLED' if bit(val,7) else 'enabled'})")
        print(f"  IRGN0   [9:8]     = {bits(val,9,8)}  (inner cache policy)")
        print(f"  ORGN0   [11:10]   = {bits(val,11,10)}  (outer cache policy)")
        print(f"  SH0     [13:12]   = {bits(val,13,12)}  ({sh_map.get(bits(val,13,12), '?')})")
        print(f"  TG0     [15:14]   = {tg0}  ({tg0_map.get(tg0, '?')})")
        print(f"  T1SZ    [21:16]   = {t1sz}  → TTBR1 VA bits = {64-t1sz}")
        print(f"  A1      [22]      = {bit(val,22)}  (ASID from TTBR{bit(val,22)})")
        print(f"  EPD1    [23]      = {bit(val,23)}  (TTBR1 walk {'DISABLED' if bit(val,23) else 'enabled'})")
        print(f"  IRGN1   [25:24]   = {bits(val,25,24)}  (inner cache policy)")
        print(f"  ORGN1   [27:26]   = {bits(val,27,26)}  (outer cache policy)")
        print(f"  SH1     [29:28]   = {bits(val,29,28)}  ({sh_map.get(bits(val,29,28), '?')})")
        print(f"  TG1     [31:30]   = {tg1}  ({tg1_map.get(tg1, '?')})")
        print(f"  IPS     [34:32]   = {ips}  ({ips_map.get(ips, '?')} bits PA)")
        print(f"  AS      [36]      = {bit(val,36)}  ({'16' if bit(val,36) else '8'}-bit ASID)")
        print(f"  TBI0    [37]      = {bit(val,37)}  (Top Byte Ignore TTBR0)")
        print(f"  TBI1    [38]      = {bit(val,38)}  (Top Byte Ignore TTBR1)")
        print(f"  HA      [39]      = {bit(val,39)}  (HW Access Flag {'ON' if bit(val,39) else 'OFF'})")
        print(f"  HD      [40]      = {bit(val,40)}  (HW Dirty Bit {'ON' if bit(val,40) else 'OFF'})")
        print(f"  HPD0    [41]      = {bit(val,41)}  (Hierarchical Perm Disable 0)")
        print(f"  HPD1    [42]      = {bit(val,42)}  (Hierarchical Perm Disable 1)")
        print(f"  TBID0   [51]      = {bit(val,51)}  (TBI for Data only 0)")
        print(f"  TBID1   [52]      = {bit(val,52)}  (TBI for Data only 1)")
        print(f"  DS      [59]      = {bit(val,59)}  (Descriptor Size / LPA2)")

        print(f"\n  Summary: VA0={64-t0sz}b, VA1={64-t1sz}b, "
              f"PA={ips_map.get(ips,'?')}b, "
              f"Gran0={tg0_map.get(tg0,'?')}, Gran1={tg1_map.get(tg1,'?')}")


class DecodeMAIR(gdb.Command):
    """Decode MAIR_EL1 — all 8 memory attribute slots."""

    def __init__(self):
        super().__init__("decode-mair", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if arg:
            val = int(gdb.parse_and_eval(arg))
        else:
            val = read_sysreg("MAIR_EL1")
            if val is None:
                print("Cannot read MAIR_EL1. Provide value: decode-mair 0x...")
                return

        print(f"\n{'='*60}")
        print(f"MAIR_EL1 = 0x{val:016x}")
        print(f"{'='*60}")

        known_attrs = {
            0x00: "Device-nGnRnE (MT_DEVICE_nGnRnE)",
            0x04: "Device-nGnRE (MT_DEVICE_nGnRE)",
            0x0c: "Device-GRE (MT_DEVICE_GRE)",
            0x44: "Normal Non-Cacheable (MT_NORMAL_NC)",
            0xff: "Normal WB Read/Write-Alloc (MT_NORMAL)",
            0xbb: "Normal Write-Through (MT_NORMAL_WT)",
            0xf0: "Normal Tagged (MT_NORMAL_TAG)",
        }

        for i in range(8):
            attr = (val >> (i * 8)) & 0xff
            desc = known_attrs.get(attr, f"Custom (outer=0x{(attr>>4)&0xf:x} inner=0x{attr&0xf:x})")
            print(f"  Attr{i} [idx {i}] = 0x{attr:02x}  {desc}")


class DecodeMMFR0(gdb.Command):
    """Decode ID_AA64MMFR0_EL1 — memory model feature register."""

    def __init__(self):
        super().__init__("decode-mmfr0", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if arg:
            val = int(gdb.parse_and_eval(arg))
        else:
            val = read_sysreg("ID_AA64MMFR0_EL1")
            if val is None:
                print("Cannot read ID_AA64MMFR0_EL1.")
                return

        print(f"\n{'='*60}")
        print(f"ID_AA64MMFR0_EL1 = 0x{val:016x}")
        print(f"{'='*60}")

        pa_map = {0: "32", 1: "36", 2: "40", 3: "42", 4: "44", 5: "48", 6: "52"}
        pa = bits(val, 3, 0)
        print(f"  PARange    [3:0]   = {pa}  ({pa_map.get(pa,'?')} bits physical address)")
        print(f"  ASIDBits   [7:4]   = {bits(val,7,4)}  ({'16' if bits(val,7,4)==2 else '8'}-bit ASID)")
        print(f"  BigEnd     [11:8]  = {bits(val,11,8)}")
        print(f"  SNSMem     [15:12] = {bits(val,15,12)}")
        print(f"  BigEndEL0  [19:16] = {bits(val,19,16)}")
        tg16 = bits(val, 23, 20)
        tg64 = bits(val, 27, 24)
        tg4 = bits(val, 31, 28)
        print(f"  TGran16    [23:20] = {tg16}  ({'supported' if tg16 in [1,2] else 'not supported'})")
        print(f"  TGran64    [27:24] = {tg64}  ({'supported' if tg64 == 0 else 'not supported'})")
        print(f"  TGran4     [31:28] = {tg4}  ({'supported' if tg4 in [0,1] else 'not supported'})")
        print(f"  TGran16_2  [35:32] = {bits(val,35,32)}")
        print(f"  TGran64_2  [39:36] = {bits(val,39,36)}")
        print(f"  TGran4_2   [43:40] = {bits(val,43,40)}")
        print(f"  ExS        [47:44] = {bits(val,47,44)}")


# Register commands
DecodeSCTLR()
DecodeTCR()
DecodeMAIR()
DecodeMMFR0()

print("ARM64 register decoders loaded: decode-sctlr, decode-tcr, decode-mair, decode-mmfr0")
