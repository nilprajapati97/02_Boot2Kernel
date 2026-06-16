Nice — let’s trace **exactly** what `insmod` does *end-to-end*, from the user command to the kernel text being executed. I’ll keep it tight but thorough, and point to the kernel source/docs for the important steps so you can dive in.

---

# TL;DR (one-paragraph)

`insmod` (or `modprobe`) hands a `.ko` file to the kernel via a syscall (`init_module()` historically, `finit_module()` in modern kernels). The kernel copies the ELF image, verifies signature/vermagic, parses ELF sections, allocates executable (code) and writable (data) memory for the module, applies architecture-specific relocations by resolving symbols against the kernel’s exported-symbol namespace (and already-loaded modules), sets final page protections, registers exported symbols and sysfs entries, calls the module’s `init` routine, and — on success — possibly frees `__init` memory and marks the module live. On any failure the loader rolls back and frees what it allocated. ([man7.org][1], [GitHub][2], [USENIX][3])

---

# End-to-end step-by-step (from scratch)

### 1) `insmod` → syscall

* `insmod` is a user utility (part of `kmod`) that opens the `.ko` file and invokes the kernel to perform the load. Historically that was `init_module(void *image, size_t len, const char *params)`. Newer code prefers `finit_module(int fd, const char *params, int flags)` (safer: passes an fd). Either syscall is the entry point the kernel uses to start loading. ([man7.org][1], [LWN.net][4])

### 2) syscall handler → kernel loader entry (`load_module`)

* The syscall implementation (e.g. `sys_finit_module` / `sys_init_module`) hands the module image to the kernel loader routine (actual function commonly named `load_module()` in `kernel/module/*` in the kernel tree). That loader is the heavy lifter: it does format checks, security checks, memory setup, relocations and finally runs your module's init. Inspect `kernel/module/main.c` to follow the real code path. ([GitHub][2])

### 3) Early checks & security

* Kernel performs several quick checks **before** doing heavy work:

  * Permission check (must be privileged).
  * ELF sanity checks (valid ELF, required sections present).
  * **Signature verification** if module-signing is enabled (`module_sig_check()`).
  * **Vermagic** / module-versioning check: the module contains a `vermagic` string to ensure it was built for a compatible kernel (or CRCs if `CONFIG_MODVERSIONS` is enabled). Failing these rejects the module. ([GitHub][2], [Linux Kernel Documentation][5])

### 4) Parse ELF sections and build load metadata

* The loader reads the `.text`, `.rodata`, `.data`, `.bss`, relocation sections (`.rela.*` / `.rel.*`), symbol table, and `.modinfo` / `.note` sections.
* It organizes those into an internal `load_info` structure describing sizes/offsets and relocation lists. (See `setup_load_info()` / `rewrite_section_headers()` in the kernel code.) ([GitHub][2])

### 5) Allocate kernel memory for the module image (code + data)

* The kernel must place module code in kernel virtual address space and ensure code pages can be executable while data pages writable. The loader typically uses `module_alloc()` (which maps VM area suitable for module text and makes it executable on platforms that require special handling) or `vmalloc`-style allocations for larger modules. Data (.data/.bss) gets separate writable allocations. After copying & relocation, protections are tightened (text RX, rodata R, data RW). ([GitHub][2], [Linux Kernel Mailing List][6])

### 6) Copy sections & zero `.bss`

* The ELF section bytes are copied into the allocated kernel buffers; `.bss` is zeroed.

### 7) Relocations (the actual “link” step)

* This is the heart of the loader:

  * For every relocation entry the loader finds, it looks up the relocation’s symbol name.
  * Symbol resolution searches the kernel’s exported-symbol namespace (symbols exported by the core kernel and by previously-loaded modules — symbols made visible with `EXPORT_SYMBOL()` / variants). If a symbol is not found (or the symbol’s version/CRC mismatches under modversions), the load fails with an “Unresolved symbol” error.
  * The loader then applies the architecture-specific relocation formula to patch instruction/data bytes (this is arch code — you’ll find arch-specific relocator helpers).
* Relocation code and the symbol export/lookup mechanism live in the kernel module codepath and arch-specific files. ([GitHub][2], [Linux Kernel Documentation][7])

### 8) Register exports and bookkeeping

* Any symbols the module itself `EXPORT_SYMBOL()`s are added to the kernel’s symbol table so later modules can resolve them.
* The kernel creates runtime bookkeeping: `struct module`, `/proc/modules` entry, `/sys/module/<name>` kobject and parameter entries, module lists, and module state flags. This is how `lsmod` and `/sys/module` get populated. ([GitHub][2], [Kernel.org][8])

### 9) Set final page protections

* After relocations (which may require writable text for patching), the loader sets final protections so text pages are not writable and data pages are not executable. This is part of kernel self-protection practices (kernel requires executable areas not be writable). Some platforms must do special steps to get executable vmalloc regions. ([Kernel.org][9], [Linux Kernel Mailing List][6])

### 10) Call the module init function

* The loader calls the module’s init function (the symbol registered by `module_init()` / default `init_module()`), in kernel context. That function registers device interfaces, allocates runtime resources, registers IRQs, etc.
* If the init returns non-zero, the kernel treats it as failure and begins rollback/cleanup. If init succeeds the module is usually marked `LIVE`. ([Kernel.org][8], [GitHub][2])

### 11) Free `__init` memory (optional)

* Code or data marked `__init` is only needed during initialization. After successful init, the kernel may free the `.init` sections so RAM is reclaimed. Modules benefit from this if they tagged cold-start helpers with `__init`. The loader coordinates that freeing. ([GitHub][2])

### 12) On failure: rollback

* Any stage that fails (signature, vermagic, ELF invalid, unresolved symbols, relocation failure, init() returned error) triggers a cleanup path: exported symbols are unregistered, sysfs entries removed, memory freed, and `init` is not considered run. The syscall returns failure; `dmesg` usually prints the reason. (This staged cleanup is why partial loads do not leave garbage.) ([USENIX][3], [GitHub][2])

### 13) Unloading later (`rmmod` / `delete_module`)

* Removing a module is done via `delete_module()` (invoked by `rmmod`/`modprobe -r`). Kernel checks dependencies and the module refcount; if safe, it calls the module’s `exit` function (registered via `module_exit()`), unregisters symbols, removes sysfs entries and frees the module memory. If the module is busy (non-zero refcount) the call fails unless forced (and forced unload is unsafe). ([man7.org][10], [GitHub][2])

---

# Mini pseudo-flow (simplified)

```c
/* user */
fd = open("mymod.ko", O_RDONLY);
finit_module(fd, params, 0);

/* kernel (very simplified) */
int sys_finit_module(fd, params, flags) {
    image = read_fd_into_kernel_buffer(fd);
    if (!valid_elf(image)) return -EINVAL;
    if (signature_bad(image)) return -EPERM;
    info = parse_elf(image);
    if (!setup_load_info(info)) return -EINVAL;
    mod = create_struct_module(info->name);
    mem = module_alloc(info->size_text);   // code/rodata
    data = kmalloc(info->size_data);       // data + bss
    copy_sections(mem, data, info);
    if (apply_relocations(mod, info) != 0) { cleanup_and_return(); }
    register_exported_symbols(mod);
    create_sysfs_entries(mod);
    if (call_init(mod->init) != 0) {
        cleanup_and_return();
    }
    free_init_sections_if_any(mod);
    mark_module_live(mod);
    return 0;
}
```

---

# Key kernel names & places to read (where to dig in source)

* Syscalls: `init_module` / `finit_module` (see manual page). ([man7.org][1])
* Module loader implementation: `kernel/module/main.c` (functions like `load_module`, `setup_load_info`, `apply_relocations`, `module_alloc`, `module_finalize_init`). See kernel source tree. ([GitHub][2])
* Module init/exit docs (driver API): kernel docs `module_init()` / `module_exit()`. ([Kernel.org][8])
* Module loading stages summary & security: research papers and articles give helpful breakdowns of stages. ([USENIX][3], [LWN.net][4])

---

# Quick debugging checklist (practical)

* `dmesg | tail` — most common errors (bad vermagic, unresolved symbols, signature failures).
* `modinfo mymod.ko` — vermagic, dependencies, parameters.
* `readelf -S mymod.ko` / `readelf -r mymod.ko` — see sections & relocations.
* `cat /proc/modules` or `lsmod` and `ls /sys/module/<name>/sections/` — see where sections are mapped.
* If you get “Unresolved symbol: foo” — search `/proc/kallsyms` or existing modules for the exported symbol. ([Linux Kernel Documentation][5], [lkw.readthedocs.io][11])

---

# A couple of important gotchas & notes

* **Vermagic / MODVERSIONS**: modules usually must match kernel build options; `CONFIG_MODVERSIONS` enables CRC-based checks and may cause loads to be rejected if mismatched. ([Linux Kernel Documentation][5])
* **Signing**: many distros enable module signature verification; unsigned modules will be rejected unless the kernel allows it. ([GitHub][2])
* **Memory protections**: kernel enforces non-writable text areas — the loader temporarily makes text writable to relocate then re-protects it. Some architectures need special handling to guarantee executable vmalloc pages. ([Kernel.org][9], [Linux Kernel Mailing List][6])

---

If you want, I can next:

* Walk through the *actual lines* in `kernel/module/main.c` from your kernel version (I can show which functions do `elf_validity_check()`, `module_sig_check()` and `apply_relocations()`), or
* Show a concrete `readelf -r` of a sample `.ko` and map relocation entries to what the kernel resolves (practical debugging), or
* Show typical `dmesg` messages for common failures (unresolved symbol, vermagic mismatch, signature failure).

Which follow-up would help most?

[1]: https://man7.org/linux/man-pages/man2/init_module.2.html?utm_source=chatgpt.com "init_module(2) - Linux manual page - man7.org"
[2]: https://github.com/torvalds/linux/blob/master/kernel/module/main.c?utm_source=chatgpt.com "linux/kernel/module/main.c at master - GitHub"
[3]: https://www.usenix.org/system/files/sec23summer_190-angelakopoulos-prepub.pdf?utm_source=chatgpt.com "[PDF] Enabling dynamic analysis of binary Linux-based IoT kernel modules"
[4]: https://lwn.net/Articles/519010/?utm_source=chatgpt.com "Loading modules from file descriptors - LWN.net"
[5]: https://docs.kernel.org/kbuild/modules.html?utm_source=chatgpt.com "Building External Modules - The Linux Kernel documentation"
[6]: https://lkml.iu.edu/hypermail/linux/kernel/2212.1/01652.html?utm_source=chatgpt.com "Re: [PATCH] Added ability to vmalloc executable memory"
[7]: https://docs.kernel.org/core-api/symbol-namespaces.html?utm_source=chatgpt.com "Symbol Namespaces - The Linux Kernel documentation"
[8]: https://www.kernel.org/doc/html/next/driver-api/basics.html?utm_source=chatgpt.com "Driver Basics — The Linux Kernel documentation"
[9]: https://www.kernel.org/doc/html/v4.14/security/self-protection.html?utm_source=chatgpt.com "Kernel Self-Protection"
[10]: https://man7.org/linux/man-pages/man2/delete_module.2.html?utm_source=chatgpt.com "delete_module(2) - Linux manual page - man7.org"
[11]: https://lkw.readthedocs.io/en/latest/doc/04_exporting_symbols.html?utm_source=chatgpt.com "5. EXPORT_SYMBOL — Linux Kernel Workbook 1.0 documentation"
