# Day 18 — ELF64 Loader & `execve`

> **Goal**: Parse a static ELF64 (AArch64) binary, map each `PT_LOAD` segment into the current `mm` with correct protection, build the initial stack with `argc`, `argv`, `envp`, `auxv`, and `eret` to the ELF entry point.
>
> **Why today**: Day 26's shell needs `execve` to run programs from the filesystem. Today we load from a kernel-embedded blob; Day 21 wires it to initramfs.

---

## 1. Background

### 1.1 ELF64 layout (only what we need)
```
ELF header (64 B)
 ┣━ e_ident, e_type=ET_EXEC, e_machine=EM_AARCH64=0xb7
 ┣━ e_entry  (user PC)
 ┣━ e_phoff  (offset to program headers)
 ┣━ e_phnum  (count)
 ┗━ e_phentsize (56)

Program header (PHDR), repeated e_phnum times:
 ┣━ p_type    (PT_LOAD=1, PT_INTERP=3, PT_NOTE=4, PT_PHDR=6)
 ┣━ p_flags   (PF_X|PF_W|PF_R)
 ┣━ p_offset  (file offset)
 ┣━ p_vaddr   (target VA)
 ┣━ p_filesz  (bytes in file)
 ┣━ p_memsz   (bytes in memory; >filesz means zero-pad = BSS)
 ┗━ p_align
```

### 1.2 AArch64 SysV initial stack
Bottom of user stack at `sp` on entry:
```
sp ─► [ argc                 ]   (u64)
      [ argv[0] .. argv[N]   ]
      [ NULL                 ]
      [ envp[0] .. envp[M]   ]
      [ NULL                 ]
      [ auxv[]: pairs of u64 ]
      [ AT_NULL,0            ]
      ... strings & random ...
```

Mandatory auxv entries:
- `AT_PAGESZ` (6) = 4096
- `AT_PHDR`   (3) = VA of program headers (if mapped)
- `AT_PHENT`  (4) = 56
- `AT_PHNUM`  (5) = e_phnum
- `AT_ENTRY`  (9) = e_entry
- `AT_RANDOM` (25) = pointer to 16 random bytes (for stack canary)
- `AT_NULL`   (0)  terminator

### 1.3 PIE vs static
We support static binaries only (`-static`). PIE adds dynamic relocations; skip until Day 30 stretch.

---

## 2. Design

### 2.1 Files
```
kernel/exec.c                  (load_elf, execve)
include/kernel/elf.h
```

### 2.2 API
```c
int load_elf(const void *image, u64 size,
             const char *argv[], const char *envp[],
             struct pt_regs *r_out, struct mm_struct *mm);
long sys_execve(struct pt_regs *r);
```

---

## 3. Implementation

### 3.1 `include/kernel/elf.h`
```c
#define EI_NIDENT 16
struct elf64_ehdr {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ET_EXEC 2
#define EM_AARCH64 0xb7
```

### 3.2 `kernel/exec.c` — segment mapping
```c
#include <kernel/elf.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <asm-arm64/pgtable.h>

static u64 elf_prot_to_pte(u32 pf)
{
    u64 a = PTE_PAGE|PTE_VALID|PTE_AF|PTE_SH_INNER|PTE_ATTR(0)|PTE_NG;
    a |= (pf & PF_W) ? PTE_AP_RW_ANY : PTE_AP_RO_ANY;
    if (!(pf & PF_X)) a |= PTE_UXN;
    return a;
}

static int map_segment(struct mm_struct *mm, const u8 *image,
                       const struct elf64_phdr *p)
{
    u64 va_start = p->p_vaddr & PAGE_MASK;
    u64 va_end   = (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
    u64 attrs    = elf_prot_to_pte(p->p_flags);

    for (u64 va = va_start; va < va_end; va += PAGE_SIZE) {
        phys_addr_t pa = alloc_pages(0);
        if (!pa) return -12;
        u8 *kva = (u8 *)(pa + 0xffff000000000000UL);
        memset(kva, 0, PAGE_SIZE);

        /* Copy any portion of file that overlaps this page */
        u64 page_va_off = va - (p->p_vaddr & PAGE_MASK);
        u64 file_start  = p->p_offset + (page_va_off ? page_va_off - (p->p_vaddr & ~PAGE_MASK) : 0);
        /* Simpler: compute overlap [va, va+4096) ∩ [p->p_vaddr, p->p_vaddr+p->p_filesz) */
        u64 seg_va = va, seg_end = va + PAGE_SIZE;
        u64 copy_va = (p->p_vaddr > seg_va) ? p->p_vaddr : seg_va;
        u64 file_avail_end = p->p_vaddr + p->p_filesz;
        u64 copy_end = (file_avail_end < seg_end) ? file_avail_end : seg_end;
        if (copy_end > copy_va) {
            u64 off_in_file = p->p_offset + (copy_va - p->p_vaddr);
            u64 off_in_page = copy_va - va;
            memcpy(kva + off_in_page, image + off_in_file, copy_end - copy_va);
        }

        map_4k(mm->pgd, va, pa, attrs);

        struct vma *v = kmalloc(sizeof *v, 0);
        *v = (struct vma){.start=va, .end=va+PAGE_SIZE,
                          .prot=((p->p_flags & PF_R)?PROT_READ:0) |
                                ((p->p_flags & PF_W)?PROT_WRITE:0) |
                                ((p->p_flags & PF_X)?PROT_EXEC:0)};
        insert_vma(mm, v);
    }
    if (p->p_vaddr + p->p_memsz > mm->end_data)
        mm->end_data = mm->start_brk = mm->brk =
            (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
    return 0;
}
```

### 3.3 Stack construction
```c
static u64 build_user_stack(struct mm_struct *mm,
                            const char *argv[], const char *envp[],
                            u64 ehdr_entry, u16 phnum)
{
    /* Allocate 8 pages stack at high VA */
    u64 stack_top = 0x80000000UL;
    for (int i = 0; i < 8; i++) {
        phys_addr_t pa = alloc_pages(0);
        map_4k(mm->pgd, stack_top - (i+1)*PAGE_SIZE, pa,
               PTE_PAGE|PTE_VALID|PTE_AF|PTE_SH_INNER|PTE_ATTR(0)|
               PTE_AP_RW_ANY|PTE_NG|PTE_UXN);
    }
    struct vma *vs = kmalloc(sizeof *vs, 0);
    *vs = (struct vma){.start=stack_top-8*PAGE_SIZE, .end=stack_top, .prot=PROT_READ|PROT_WRITE};
    insert_vma(mm, vs);

    /* Write strings + auxv at top of stack */
    u64 *sp_kva_top = (u64*)(walk_pa(mm->pgd, stack_top - 8) + 0xffff000000000000UL + 8);
    /* For simplicity build the layout into a temp buffer and copy_to_user-style place it.
       Real impl: compute total size, place strings near top, point argv/envp into them. */

    /* Pseudocode summary:
       1. Count argc, envc.
       2. Concatenate argv[*], envp[*] strings to top, get their user VAs.
       3. Place auxv just below strings.
       4. Place envp pointers, NULL.
       5. Place argv pointers, NULL.
       6. Place argc.
       7. sp = address of argc.
    */
    extern u64 stack_layout_helper(u64 stack_top_va, struct mm_struct *mm,
                                   const char *argv[], const char *envp[],
                                   u64 entry, u16 phnum);
    return stack_layout_helper(stack_top, mm, argv, envp, ehdr_entry, phnum);
}
```

### 3.4 `load_elf` orchestrator
```c
int load_elf(const void *image, u64 size,
             const char *argv[], const char *envp[],
             struct pt_regs *r, struct mm_struct *mm)
{
    const struct elf64_ehdr *eh = image;
    if (memcmp(eh->e_ident, "\x7f""ELF", 4)) return -8;
    if (eh->e_machine != EM_AARCH64) return -8;
    if (eh->e_type != ET_EXEC) return -8;

    const struct elf64_phdr *ph = (void *)((u8*)image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (map_segment(mm, image, &ph[i]) < 0) return -12;
    }

    u64 sp = build_user_stack(mm, argv, envp, eh->e_entry, eh->e_phnum);

    memset(r, 0, sizeof *r);
    r->pc     = eh->e_entry;
    r->sp     = sp;
    r->pstate = 0;
    return 0;
}
```

### 3.5 `sys_execve`
```c
long sys_execve(struct pt_regs *r)
{
    const char __user *u_path = (void *)r->regs[0];
    char path[128];
    if (copy_from_user(path, u_path, 128)) return -14;
    path[127] = 0;

    /* TODO: real argv/envp copy from user; below uses empty for first user */
    const char *argv[] = { path, NULL };
    const char *envp[] = { NULL };

    /* Open + read file via VFS (Day 20). For now expect a kernel-side blob. */
    extern int load_file_from_initramfs(const char *p, void **buf, u64 *sz);
    void *image; u64 size;
    if (load_file_from_initramfs(path, &image, &size) < 0) return -2;  /* ENOENT */

    /* Reset mm */
    mm_free(current()->mm);
    current()->mm = mm_alloc();

    struct pt_regs new_regs;
    if (load_elf(image, size, argv, envp, &new_regs, current()->mm) < 0)
        return -8;
    *r = new_regs;
    return 0;
}
```

---

## 4. Pitfalls

1. **`p_vaddr` not page-aligned**: rare but legal. Round down for the first page; copy uses an offset within that page.
2. **`p_memsz > p_filesz`**: the slack is BSS — must be zero-filled. `memset` per page handles it because we zero before partial copy.
3. **W^X**: never set both `PTE_AP_RW_ANY` and missing `PTE_UXN`. Reject ELFs with `PF_W|PF_X` simultaneous (or strip exec).
4. **Stack auxv off by one**: ensure terminating `{AT_NULL, 0}` present; libc reads until that.
5. **Replacing current mm mid-syscall**: `execve` swaps mm; ensure the saved `pt_regs` written to the kernel stack belongs to the *new* mm context, and SP_EL0/TTBR0 reloaded before `eret`.

---

## 5. Verification

```
$ aarch64-linux-gnu-gcc -static -nostartfiles -nostdlib -e main hello.c -o hello.elf
$ make initramfs    # bundles hello.elf
$ make run
[INFO] init: exec /bin/hello
hello from nkernel
[INFO] task pid=2 exited 0
```

GDB: break at user `e_entry`; verify `sp` points at user stack containing argc=1.

---

## 6. Stretch

- `PT_INTERP` handling → dynamic linker (way out of scope, but stub the rejection cleanly).
- AUXV `AT_RANDOM` filled with 16 bytes from `CNTPCT_EL0` mixed.
- `mprotect()` syscall (#226) flipping page attrs in-place.

---

## 7. References

- *System V Application Binary Interface — AArch64* (sys-v ABI).
- ELF-64 spec (Sun/Oracle).
- Linux `fs/binfmt_elf.c`.
