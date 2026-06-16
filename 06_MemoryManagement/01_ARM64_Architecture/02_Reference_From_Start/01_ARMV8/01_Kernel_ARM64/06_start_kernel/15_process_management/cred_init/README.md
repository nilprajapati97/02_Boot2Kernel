# `cred_init()` — Credentials Slab Cache

## Purpose

Allocates the slab cache for `struct cred`, which holds a process's security identity: user/group IDs, Linux capabilities, and security module labels. Every process has a set of credentials.

## Source File

`kernel/cred.c`

```c
void __init cred_init(void)
{
    cred_jar = kmem_cache_create("cred_jar", sizeof(struct cred), 0,
                                  SLAB_HWCACHE_ALIGN | SLAB_PANIC |
                                  SLAB_ACCOUNT, NULL);
}
```

## `struct cred`

```c
struct cred {
    atomic_t        usage;
    kuid_t          uid;        // Real UID
    kgid_t          gid;        // Real GID
    kuid_t          suid;       // Saved UID
    kgid_t          sgid;       // Saved GID
    kuid_t          euid;       // Effective UID (used for permission checks)
    kgid_t          egid;       // Effective GID
    kuid_t          fsuid;      // Filesystem UID
    kgid_t          fsgid;      // Filesystem GID
    
    unsigned        securebits; // SUID/SGID behavior flags
    
    kernel_cap_t    cap_inheritable;  // Capabilities inherited across exec
    kernel_cap_t    cap_permitted;    // Capabilities available
    kernel_cap_t    cap_effective;    // Currently active capabilities
    kernel_cap_t    cap_bset;         // Capability bounding set
    kernel_cap_t    cap_ambient;      // Ambient capabilities (exec inheritance)
    
    unsigned char   jit_keyring;      // Default keyring for JIT keys
    struct key      *session_keyring; // Keyring inherited over fork
    struct key      *process_keyring; // Keyring for this process
    struct key      *thread_keyring;  // Keyring for this thread
    struct key      *request_key_auth;// Assume request_key() authorisation
    
    void            *security;       // LSM security blob
    struct user_struct *user;        // Per-user resource counters
    struct user_namespace *user_ns;  // User namespace
    
    struct group_info *group_info;   // Supplementary groups
    struct rcu_head  rcu;
};
```

## Copy-on-Write Semantics

Credentials are immutable once set:

```c
// To change credentials:
new = prepare_creds();  // COW: copy current creds
new->euid = target_uid; // Modify the copy
commit_creds(new);      // Atomically replace current->cred
```

This ensures other threads or kernel code reading `current->cred` never see a partially-updated credential structure.

## Capability Sets

```
cap_permitted  ⊇  cap_effective  (effective must be subset of permitted)
cap_effective  ⊇  cap_inheritable is NOT required

cap_permitted ∩ cap_inheritable → caps passed across exec()
cap_ambient → inherited even by unprivileged exec()
```

### Common Capabilities

| Capability | Meaning |
|------------|---------|
| `CAP_SYS_ADMIN` | Everything (the "super" capability) |
| `CAP_NET_BIND_SERVICE` | Bind to ports < 1024 |
| `CAP_KILL` | Send signals to any process |
| `CAP_SETUID` | Change UID |
| `CAP_DAC_OVERRIDE` | Bypass file permission checks |
| `CAP_SYS_PTRACE` | ptrace any process |
| `CAP_SYS_RAWIO` | Raw I/O, `/dev/mem` access |

## setuid Binaries

```bash
# ping needs CAP_NET_RAW — traditionally setuid root:
ls -l /bin/ping
# -rwsr-xr-x 1 root root ... /bin/ping
#    ↑ setuid bit

# Modern kernels use file capabilities instead:
getcap /bin/ping
# /bin/ping = cap_net_raw+ep
```

## Cross-references

- [Phase overview](../README.md)
- `security_init()`: [../../11_security_randomness/security_init/README.md](../../11_security_randomness/security_init/README.md) — LSM fills `cred->security`
- `key_init()`: [../../11_security_randomness/key_init/README.md](../../11_security_randomness/key_init/README.md) — keyrings in cred
