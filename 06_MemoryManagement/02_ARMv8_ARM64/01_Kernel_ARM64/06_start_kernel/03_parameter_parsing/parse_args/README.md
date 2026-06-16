# `parse_args()` — Kernel Argument Parsing

## Purpose

Parses a command line string and calls the appropriate handler for each `name=value` parameter. Called multiple times in `start_kernel()` with different scopes: kernel parameters, init arguments, and bootconfig init args.

## Source File

`kernel/params.c`

## Signature

```c
char *parse_args(const char *doing,        // Description for error messages
                 char *args,               // String to parse (modified in-place!)
                 const struct kernel_param *params,  // param table (or NULL)
                 unsigned num,             // Number of params in table
                 s16 min_level,            // Minimum initcall level (-1 = all)
                 s16 max_level,            // Maximum initcall level (-1 = all)
                 void *arg,               // Passed to unknown_param handler
                 parse_unknown_fn unknown); // Handler for unknown params
```

Returns:
- `NULL` if all parameters were consumed
- Pointer to the `--` separator if found (caller uses it for init args)
- `ERR_PTR(-errno)` on error

## The Three Calls in `start_kernel()`

### Call 1: Kernel Parameters (line ~906)

```c
after_dashes = parse_args("Booting kernel",
                          static_command_line,
                          __start___param,
                          __stop___param - __start___param,
                          -1, -1, NULL,
                          &unknown_bootoption);
```

- Parses `static_command_line`
- Looks up parameters in the `__param` section (registered with `module_param()`, `core_param()`)
- Unknown parameters go to `unknown_bootoption()` → eventually to init
- Returns pointer past `--` if found

### Call 2: Init Arguments (line ~911, conditional)

```c
if (!IS_ERR_OR_NULL(after_dashes))
    parse_args("Setting init args", after_dashes, NULL, 0,
               -1, -1, NULL, set_init_arg);
```

- Parses everything after `--`
- All parameters call `set_init_arg()` → appends to `argv_init[]`

### Call 3: Extra Init Args (line ~913, conditional)

```c
if (extra_init_args)
    parse_args("Setting extra init args", extra_init_args,
               NULL, 0, -1, -1, NULL, set_init_arg);
```

- Parses bootconfig `init.*` parameters
- Same handler: appends to `argv_init[]`

## The `__param` Section

Parameters are registered with:

```c
// In a driver:
module_param(debug, bool, 0644);

// In core kernel:
core_param(initcall_debug, initcall_debug, bool, 0644);
```

These emit a `struct kernel_param` into the `__param` ELF section:

```c
struct kernel_param {
    const char *name;          // "debug", "initcall_debug", etc.
    struct module *mod;        // NULL for built-in
    const struct kernel_param_ops *ops; // set/get functions
    void *arg;                 // Pointer to the variable
    /* ... flags, level ... */
};
```

`parse_args()` iterates `__start___param` to `__stop___param` and calls `ops->set()` for matching parameters.

## How Parsing Works

For the input `"console=ttyS0,115200 root=/dev/sda1"`:

1. Tokenize on spaces (respecting quotes)
2. For `"console=ttyS0,115200"`: split at `=` → name=`"console"`, val=`"ttyS0,115200"`
3. Search `__param` section for entry with `name == "console"`
4. Call `kernel_param.ops->set("ttyS0,115200", &kernel_param)`
5. Repeat for next token

## IRQ State

IRQs **disabled** for the calls in `start_kernel()`. (Later `parse_args()` calls in `do_initcall_level()` have IRQs enabled.)

## Cross-references

- [Phase overview](../README.md)
- `parse_early_param()` — earlier, simpler parsing: [../parse_early_param/README.md](../parse_early_param/README.md)
