# `uts_ns_init()` — UTS Namespace Initialization

## Purpose

Initializes the UTS (Unix Time Sharing) namespace, which holds the system's hostname and NIS domain name. Each container or namespace can have a different hostname.

## Source File

`kernel/utsname.c`

```c
static struct ctl_table uts_kern_table[] = {
    {
        .procname   = "hostname",
        .data       = init_uts_ns.name.nodename,
        .maxlen     = __NEW_UTS_LEN + 1,
        .mode       = 0644,
        .proc_handler = proc_do_uts_string,
    },
    // ...
};

static int __init utsns_cache_init(void)
{
    uts_ns_cache = KMEM_CACHE(uts_namespace,
                               SLAB_PANIC | SLAB_ACCOUNT);
    return 0;
}
```

## `struct uts_namespace`

```c
struct uts_namespace {
    struct new_utsname  name;    // The actual names
    struct user_namespace *user_ns;
    struct ucounts      *ucounts;
    struct ns_common    ns;      // Namespace generic fields
};

struct new_utsname {
    char sysname[__NEW_UTS_LEN + 1];    // "Linux"
    char nodename[__NEW_UTS_LEN + 1];   // hostname (up to 64 chars)
    char release[__NEW_UTS_LEN + 1];    // kernel release "6.x.y"
    char version[__NEW_UTS_LEN + 1];    // compile version "#1 SMP ..."
    char machine[__NEW_UTS_LEN + 1];    // "x86_64"
    char domainname[__NEW_UTS_LEN + 1]; // NIS domain name
};
```

## Syscalls

```c
// Get hostname:
int uname(struct utsname *buf);

// Set hostname (requires CAP_SYS_ADMIN):
int sethostname(const char *name, size_t len);

// Set NIS domain name:
int setdomainname(const char *name, size_t len);
```

## UTS Namespace and Containers

With `CLONE_NEWUTS`, each container can have its own hostname:

```bash
# In a container:
hostname mycontainer

# Host still sees its own hostname:
# (in root namespace)
hostname  # → "myhost.example.com"

# In container namespace:
hostname  # → "mycontainer"
```

## `/proc/sys/kernel/hostname`

```bash
cat /proc/sys/kernel/hostname      # Read hostname
echo "newhost" > /proc/sys/kernel/hostname  # Set hostname (root only)
```

## Cross-references

- [Phase overview](../README.md)
- Namespace namespaces: [../../19_rest_init/README.md](../../19_rest_init/README.md) — full namespace discussion in rest_init
