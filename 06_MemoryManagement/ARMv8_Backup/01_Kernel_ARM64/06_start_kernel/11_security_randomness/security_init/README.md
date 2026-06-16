# `security_init()` — Linux Security Module Framework

## Purpose

Initializes the Linux Security Module (LSM) framework, calling the `init()` function for each compiled-in security module. LSMs provide mandatory access control (MAC), auditing, and integrity enforcement by inserting hooks throughout the kernel.

## Source File

`security/security.c`

```c
void __init security_init(void)
{
    struct lsm_info *lsm;
    
    init_debug("LSM initializing\n");
    
    // Initialize the "major" LSM first (e.g., SELinux or AppArmor)
    // Selected at build time via CONFIG_DEFAULT_SECURITY
    for_each_lsm(lsm) {
        if (!lsm->enabled || !(lsm->flags & LSM_FLAG_LEGACY_MAJOR))
            continue;
        init_debug("  initializing %s\n", lsm->name);
        lsm->init();
    }
    
    // Initialize remaining LSMs
    for_each_lsm(lsm) {
        if (!lsm->enabled || (lsm->flags & LSM_FLAG_LEGACY_MAJOR))
            continue;
        init_debug("  initializing %s\n", lsm->name);
        lsm->init();
    }
    
    // Note: early LSMs (lockdown, yama) were initialized in Phase 2
    // by early_security_init()
}
```

## LSM Hook Architecture

LSMs work by registering callbacks for security-sensitive operations:

```c
// Hook structure (simplified):
struct security_hook_list {
    struct hlist_node list;
    struct hlist_head *head;    // Which hook point
    union security_list_options hook;  // The callback
    const char *lsm;
};

// Example hook registration by SELinux:
static struct security_hook_list selinux_hooks[] = {
    LSM_HOOK_INIT(file_permission, selinux_file_permission),
    LSM_HOOK_INIT(inode_permission, selinux_inode_permission),
    LSM_HOOK_INIT(task_kill, selinux_task_kill),
    LSM_HOOK_INIT(socket_connect, selinux_socket_connect),
    // ... hundreds of hooks ...
};
```

## Major LSMs

### SELinux (Security Enhanced Linux)

- Policy-based MAC system developed by NSA
- Labels every process, file, and resource with a "security context"
- Enforces a policy that defines allowed interactions
- Default on RHEL/CentOS/Fedora/Android

```bash
# Check SELinux status:
getenforce    # Enforcing / Permissive / Disabled

# View context of a process:
ps -Z -p 1    # z_u:z_r:z_t:s0

# View context of a file:
ls -Z /etc/passwd    # system_u:object_r:passwd_file_t:s0
```

### AppArmor

- Path-based MAC system
- Simpler than SELinux — policies based on file paths
- Default on Ubuntu/Debian/openSUSE

```bash
# Check AppArmor status:
aa-status

# Profile for an application (example):
/usr/bin/firefox {
    /proc/*/status r,
    /usr/share/fonts/** r,
    deny /etc/shadow r,
}
```

### Other LSMs

| LSM | Purpose |
|-----|---------|
| `capabilities` | POSIX capabilities (always active) |
| `yama` | Ptrace restrictions |
| `lockdown` | Restrict kernel lockdown features |
| `tomoyo` | Path-based MAC (Japanese origin) |
| `smack` | Simplified MAC |
| `integrity/IMA` | File integrity measurement |

## LSM Stacking

Modern kernels support running multiple LSMs simultaneously:

```
File access check:
    capabilities check (always)
    → SELinux check (if enabled)
    → AppArmor check (if enabled)
    → yama check (if enabled)
    
All must pass. Any denial = access denied.
```

## Performance Impact

LSM hooks are on every security-sensitive path. Performance impact:
- `capabilities`: ~0 overhead (simple bit check)
- `SELinux`: ~2-5% overhead for typical workloads
- `AppArmor`: ~1-3% overhead

## Cross-references

- [Phase overview](../README.md)
- `early_security_init()`: [../../02_arch_setup/early_security_init/README.md](../../02_arch_setup/early_security_init/README.md)
