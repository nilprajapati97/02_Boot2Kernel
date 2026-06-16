# `nsfs_init()` — Namespace Filesystem

## Purpose

Registers the `nsfs` pseudo-filesystem, which provides file descriptors for Linux namespaces. These file descriptors can be opened, held, and passed to `setns()` to join a namespace or `unshare()` to create a new one.

## Source File

`fs/nsfs.c`

```c
static struct file_system_type nsfs = {
    .name     = "nsfs",
    .init_fs_context = nsfs_init_fs_context,
    .kill_sb  = kill_anon_super,
};

void __init nsfs_init(void)
{
    nsfs_mnt = kern_mount(&nsfs);
    if (IS_ERR(nsfs_mnt))
        panic("can't set nsfs up\n");
    nsfs_mnt->mnt_sb->s_flags &= ~SB_NOUSER;
}
```

## Linux Namespaces

Linux has 8 namespace types, each isolating a different resource:

| Namespace | Flag | Isolates |
|-----------|------|----------|
| `mnt` | `CLONE_NEWNS` | Mount points |
| `uts` | `CLONE_NEWUTS` | Hostname, domain name |
| `ipc` | `CLONE_NEWIPC` | SysV IPC, POSIX message queues |
| `pid` | `CLONE_NEWPID` | Process IDs |
| `net` | `CLONE_NEWNET` | Network interfaces, routing, ports |
| `user` | `CLONE_NEWUSER` | UIDs/GIDs mapping |
| `cgroup` | `CLONE_NEWCGROUP` | cgroup root |
| `time` | `CLONE_NEWTIME` | Boot and monotonic clock offsets |

## Namespace File Descriptors

Each running namespace is represented by a file in `/proc/PID/ns/`:

```bash
ls -la /proc/self/ns/
# lrwxrwxrwx 1 user user 0 Jan  1 net -> net:[4026531992]
# lrwxrwxrwx 1 user user 0 Jan  1 pid -> pid:[4026531836]
# lrwxrwxrwx 1 user user 0 Jan  1 mnt -> mnt:[4026531841]
# The number is the namespace's inode number in nsfs
```

The inode number uniquely identifies the namespace instance. Two processes in the same namespace have the same inode number for that namespace type.

## Keeping Namespaces Alive

```bash
# Keep a namespace alive even after all processes exit:
# Bind-mount the namespace file to a non-temporary location:
touch /run/mynetns
mount --bind /proc/1234/ns/net /run/mynetns

# Later, join that namespace:
nsenter --net=/run/mynetns ip addr
```

`nsfs` provides the inode backing these bind-mountable namespace files.

## setns() and unshare()

```c
// Join an existing namespace:
int fd = open("/proc/1234/ns/net", O_RDONLY);
setns(fd, CLONE_NEWNET);   // Current process joins that network namespace

// Create a new namespace for current process:
unshare(CLONE_NEWNS);   // Current process gets a private mount namespace
```

## Cross-references

- [Phase overview](../README.md)
- `uts_ns_init()`: [../../15_process_management/uts_ns_init/README.md](../../15_process_management/uts_ns_init/README.md)
- `net_ns_init()`: [../../16_networking/net_ns_init/README.md](../../16_networking/net_ns_init/README.md)
