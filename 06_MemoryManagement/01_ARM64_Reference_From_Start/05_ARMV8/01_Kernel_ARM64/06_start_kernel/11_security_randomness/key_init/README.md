# `key_init()` — Key Management Service

## Purpose

Initializes the Linux key retention service (keyrings). This subsystem stores and manages cryptographic keys, passwords, authentication tokens, and other small secrets in kernel memory, accessible to processes via the `keyctl()` family of syscalls.

## Source File

`security/keys/key.c`

```c
void __init key_init(void)
{
    // Create slab caches for key types:
    key_jar = kmem_cache_create("key_jar", sizeof(struct key), 0,
                                SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
    
    // Set up the root user's keyrings:
    key_user_table[0].uid = KUIDT_INIT(0);
    key_user_table[0].usage = REFCOUNT_INIT(1);
    atomic_set(&key_user_table[0].nkeys, 0);
    
    // Create system keyrings:
    session_keyring = keyring_alloc(".session_keyring", ...);
    process_keyring = keyring_alloc(".process_keyring", ...);
    
    // Register built-in key types:
    register_key_type(&key_type_keyring);
    register_key_type(&key_type_user);
    register_key_type(&key_type_logon);
}
```

## Key Concepts

### Key Types

Keys can hold different types of data:

| Type | Description |
|------|-------------|
| `keyring` | Container for other keys |
| `user` | Arbitrary data, readable by userspace |
| `logon` | Secrets not readable by userspace (password-like) |
| `asymmetric` | Public/private key pairs |
| `dns_resolver` | DNS lookup results |
| `rxrpc_s` | RxRPC server keys |

### Keyring Hierarchy

```
Thread keyring
    ↓ (fallback)
Process keyring
    ↓ (fallback)
Session keyring (inherited at login)
    ↓ (fallback)
User keyring (per-UID)
    ↓ (fallback)
User session keyring
    ↓ (fallback)
System keyring (kernel-global)
```

## System Keyring Uses

### Secure Boot / Module Signing

The kernel's module signing infrastructure uses a keyring:

```bash
# View kernel's trusted keys:
cat /proc/keys | grep .builtin_trusted_keys
```

When loading a kernel module, its signature is verified against keys in the `.builtin_trusted_keys` keyring.

### dm-verity / IMA

Integrity Measurement Architecture (IMA) uses keyrings to store keys for verifying file hashes.

## Userspace API

```c
// Add a key:
key_serial_t add_key(const char *type, const char *desc,
                     const void *payload, size_t plen,
                     key_serial_t ringid);

// Request a key (search + instantiate):
key_serial_t request_key(const char *type, const char *desc,
                          const char *callout_info, key_serial_t ringid);

// Key control operations:
long keyctl(int operation, ...);
// Operations: KEYCTL_DESCRIBE, KEYCTL_READ, KEYCTL_UPDATE,
//             KEYCTL_LINK, KEYCTL_SEARCH, KEYCTL_SETPERM, ...
```

## Security

Keys are protected by:
- UID/GID ownership checks
- Permission bits (possessor/owner/group/other, read/write/search/link/setattr)
- SELinux/AppArmor labels
- Expiry times

## Cross-references

- [Phase overview](../README.md)
- `security_init()`: [../security_init/README.md](../security_init/README.md) — LSM uses keyrings
