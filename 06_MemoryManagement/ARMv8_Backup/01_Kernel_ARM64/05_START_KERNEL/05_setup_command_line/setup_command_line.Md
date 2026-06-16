Inspecting ARM32/ARM64 `setup_arch()` implementations and command-line handling.

Searched for regex `void __init setup_arch\(|setup_arch\(char \*\*cmdline_p\)` (`**/arch/arm*/**/*.{c,S}`), 2 results

Searched for regex `boot_command_line|command_line` (`**/arch/arm*/**/*.{c,h,S}`), 25 results

Found ARM32 and ARM64 `setup_arch()` entry points. Now reading the relevant implementation details.

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/setup.c#250-250), lines 250 to 320

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm/kernel/setup.c#1080-1080), lines 1080 to 1160

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm/kernel/atags_parse.c#180-180), lines 180 to 231

## What `setup_command_line(command_line);` does

In `linux/init/main.c`, the call is part of early kernel boot:

- `setup_arch(&command_line);`
- `setup_boot_config();`
- `setup_command_line(command_line);`

`setup_arch()` is architecture-specific and fills in the raw boot command line.
`command_line` is then the pointer to the string the kernel will use for later parameter parsing.

`setup_command_line()` does this:

- allocates `saved_command_line` and `static_command_line`
- merges any `extra_command_line` from boot config
- keeps an immutable “saved” copy for `/proc/cmdline` and logging
- prepares a mutable parse buffer used by `parse_args()`

---

## ARM64 design

In setup.c:

- `setup_arch(char **cmdline_p)` directly does:
  - `*cmdline_p = boot_command_line;`

That means:

- ARM64 uses `boot_command_line` as the active parse buffer
- `command_line` passed into `setup_command_line()` is the same pointer
- no additional copy is needed before command-line parsing

ARM64 command-line flow:

1. bootloader/firmware provides initrd/FDT
2. `setup_machine_fdt()` and ARM64 setup code initialize machine state
3. `boot_command_line` holds the incoming kernel boot params
4. `setup_command_line()` duplicates and preserves it into kernel-managed memory

---

## ARM32 design

In setup.c:

- `setup_arch(char **cmdline_p)` does:
  - `strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);`
  - `*cmdline_p = cmd_line;`

So ARM32:

- preserves the original raw `boot_command_line`
- uses a separate writable `cmd_line[]` buffer for parsing
- passes the copy to `setup_command_line()`

Why this matters:

- `boot_command_line` is the untouched bootloader-provided string
- `cmd_line[]` is a working copy that can be modified without losing the raw data
- this is important because `parse_args()` modifies its input in-place

---

## Key difference between ARM32 and ARM64

- ARM64: `command_line == boot_command_line`
- ARM32: `command_line == cmd_line` and `boot_command_line` is preserved separately

So for ARM32, `setup_command_line()` gets a copy of the raw boot parameters.
For ARM64, it gets the raw boot parameters directly.

---

## What `setup_command_line()` actually stores

In main.c, `setup_command_line(char *command_line)`:

- computes `xlen` from `extra_command_line`
- computes `ilen` from `extra_init_args` (adds 4 for `" -- "`)
- allocates:
  - `saved_command_line` with room for extra args
  - `static_command_line` for parsing
- copies:
  - `saved_command_line = [extra_command_line?] + boot_command_line [+ extra_init_args]`
  - `static_command_line = [extra_command_line?] + command_line`
- stores `saved_command_line_len`

Effect:

- `saved_command_line` is the full canonical command line visible to userspace and `/proc/cmdline`
- `static_command_line` is the parse-time copy that may be modified
- `boot_command_line` remains the immutable raw command line from the architecture layer

---

## ARM command-line source specifics

ARM32 uses ATAGs/FDT and atags_parse.c:

- parses ATAGs / device tree
- fills `boot_command_line` from parsed tags or default `CONFIG_CMDLINE`

ARM64 uses FDT via `setup_machine_fdt()` and firmware-provided DTB pointer.

---

## Practical impact

- On ARM32, the kernel preserves the raw bootloader command line separately from the working parse buffer.
- On ARM64, the kernel is simpler here because the architecture code is already happy to use `boot_command_line` directly.
- In both cases, `setup_command_line()` is the point where early boot command-line data is frozen into kernel-owned memory for later use.

If you want, I can also map the exact ARM32/ARM64 code paths for `boot_command_line` population before `setup_arch()` returns.