# Day 20 — Virtual File System (VFS) Core

> **Goal**: Add a tiny but real VFS layer: `struct inode`, `struct dentry`, `struct file`, `struct file_operations`, `struct super_block`, plus a per-task file descriptor table. Mountable filesystems register through `struct file_system_type`.
>
> **Why today**: Day 21 (initramfs) and Day 23/24 (ext2) plug into this. Day 17's `read`/`write` and Day 18's `execve` go through `fd → file → inode`.

---

## 1. Background

### 1.1 Four central objects (Linux model, slim)
```
super_block ── represents one mounted FS instance
   │
   ▼
inode  ── unique file identity (type, size, perms, ops)
   │
   ▼
dentry ── name within a directory; cached path components
   │
   ▼
file   ── an open handle (pos, flags, ops, points to inode)
```

### 1.2 File descriptor table
```c
struct files_struct {
    struct file *fd[OPEN_MAX];   /* OPEN_MAX = 32 to start */
    spinlock_t lock;
};
```
Per `task`. `fork` shares or duplicates depending on flags (we duplicate).

### 1.3 Path lookup (`namei`)
```
"/etc/hostname"
  → root dentry
  → dentry "etc" (lookup via parent dir's inode->i_op->lookup)
  → dentry "hostname"
  → its inode
```

---

## 2. Design

### 2.1 Files
```
include/kernel/fs.h
fs/super.c          mount, list of mounted FSes
fs/inode.c          alloc/free/cache
fs/dentry.c         hash table
fs/file.c           fd table, sys_open/close
fs/namei.c          path walker
fs/read_write.c     sys_read/write, lseek
```

### 2.2 Core structures
```c
struct super_block;
struct inode;
struct dentry;
struct file;

struct file_operations {
    long (*read) (struct file *, char __user *, size_t, u64 *pos);
    long (*write)(struct file *, const char __user *, size_t, u64 *pos);
    int  (*open) (struct inode *, struct file *);
    int  (*release)(struct inode *, struct file *);
    int  (*ioctl)(struct file *, unsigned cmd, unsigned long arg);
    long (*llseek)(struct file *, long off, int whence);
    int  (*readdir)(struct file *, void *ctx, int (*emit)(void*,const char*,int,u64));
};

struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, const char *name);
    int  (*create)(struct inode *dir, struct dentry *d, u16 mode);
    int  (*mkdir) (struct inode *dir, struct dentry *d, u16 mode);
    int  (*unlink)(struct inode *dir, struct dentry *d);
};

struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *);
    void (*destroy_inode)(struct inode *);
    int  (*statfs)(struct super_block *, struct kstatfs *);
};

struct file_system_type {
    const char *name;
    int (*mount)(struct file_system_type*, const void *data, struct super_block **out);
    struct file_system_type *next;
};
```

### 2.3 `struct inode`
```c
struct inode {
    u32  i_ino;
    u16  i_mode;          /* S_IFREG | 0755 etc. */
    u16  i_nlink;
    u32  i_uid, i_gid;
    u64  i_size;
    u64  i_atime, i_mtime, i_ctime;
    struct super_block *i_sb;
    const struct inode_operations *i_op;
    const struct file_operations  *i_fop;
    void *i_private;       /* fs-specific: e.g. ext2 inode struct */
    int   i_refcount;
};
```

### 2.4 `struct dentry` (tiny, cached path component)
```c
struct dentry {
    char  d_name[64];
    struct dentry *d_parent;
    struct dentry *d_child;     /* head of children */
    struct dentry *d_sibling;
    struct inode  *d_inode;
    int   d_refcount;
};
```

### 2.5 `struct file`
```c
struct file {
    struct dentry *f_dentry;
    struct inode  *f_inode;
    const struct file_operations *f_op;
    u64    f_pos;
    u32    f_flags;       /* O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_CREAT */
    int    f_refcount;
    void  *private_data;
};
```

---

## 3. Implementation skeletons

### 3.1 FS registration
```c
static struct file_system_type *fs_types;

int register_filesystem(struct file_system_type *fs)
{
    fs->next = fs_types; fs_types = fs; return 0;
}

struct file_system_type *find_filesystem(const char *name)
{
    for (struct file_system_type *p = fs_types; p; p = p->next)
        if (!strcmp(p->name, name)) return p;
    return NULL;
}
```

### 3.2 Mounting
```c
static struct super_block *root_sb;
static struct dentry      *root_dentry;

int do_mount(const char *fstype, const void *data)
{
    struct file_system_type *fs = find_filesystem(fstype);
    if (!fs) return -22;
    struct super_block *sb;
    int err = fs->mount(fs, data, &sb);
    if (err) return err;

    if (!root_sb) {
        root_sb = sb;
        root_dentry = kzalloc(sizeof(*root_dentry), 0);
        strcpy(root_dentry->d_name, "/");
        root_dentry->d_inode = sb->s_root_inode;
    }
    return 0;
}
```

### 3.3 Path walker
```c
struct dentry *path_lookup(const char *path)
{
    if (path[0] != '/') return NULL;
    struct dentry *cur = root_dentry;
    const char *p = path + 1;
    while (*p) {
        char name[64]; int n = 0;
        while (*p && *p != '/' && n < 63) name[n++] = *p++;
        name[n] = 0;
        if (*p == '/') p++;
        if (!cur->d_inode || !cur->d_inode->i_op->lookup) return NULL;
        struct dentry *child = cur->d_inode->i_op->lookup(cur->d_inode, name);
        if (!child) return NULL;
        child->d_parent = cur;
        cur = child;
    }
    return cur;
}
```

### 3.4 fd table + `open/close`
```c
static int fd_install(struct file *f)
{
    struct files_struct *fs = current()->files;
    for (int i = 0; i < OPEN_MAX; i++)
        if (!fs->fd[i]) { fs->fd[i] = f; return i; }
    return -24;            /* EMFILE */
}

long sys_openat(struct pt_regs *r)
{
    /* int dfd, const char *path, int flags, mode_t mode */
    int dfd = r->regs[0]; (void)dfd;       /* AT_FDCWD-only */
    char path[128];
    if (copy_from_user(path, (void*)r->regs[1], 128)) return -14;
    int flags = r->regs[2];
    u16 mode  = r->regs[3];

    struct dentry *d = path_lookup(path);
    if (!d) {
        if (!(flags & O_CREAT)) return -2;
        /* Split parent/last, call dir->i_op->create */
        /* ... omitted for brevity ... */
        return -2;
    }
    struct file *f = kzalloc(sizeof *f, 0);
    f->f_dentry = d; f->f_inode = d->d_inode;
    f->f_op = d->d_inode->i_fop; f->f_flags = flags;
    if (f->f_op->open) f->f_op->open(d->d_inode, f);
    int fd = fd_install(f);
    if (fd < 0) { kfree(f); return fd; }
    return fd;
}

long sys_close(struct pt_regs *r)
{
    int fd = r->regs[0];
    struct files_struct *fs = current()->files;
    if (fd < 0 || fd >= OPEN_MAX || !fs->fd[fd]) return -9;
    struct file *f = fs->fd[fd]; fs->fd[fd] = NULL;
    if (f->f_op->release) f->f_op->release(f->f_inode, f);
    kfree(f);
    return 0;
}
```

### 3.5 Generic `read`/`write` via fd
```c
long sys_read(struct pt_regs *r)
{
    int fd = r->regs[0]; void __user *buf = (void*)r->regs[1]; size_t n = r->regs[2];
    if (fd < 0 || fd >= OPEN_MAX) return -9;
    struct file *f = current()->files->fd[fd];
    if (!f || !f->f_op->read) return -9;
    return f->f_op->read(f, buf, n, &f->f_pos);
}
/* sys_write mirrors */
```

### 3.6 Default `console_fops` so fd 0/1/2 keep working
```c
static long con_read (struct file*f,char __user*b,size_t n,u64*p){ return uart_read_user(b,n); }
static long con_write(struct file*f,const char __user*b,size_t n,u64*p){ return uart_write_user(b,n); }
const struct file_operations console_fops = {.read=con_read,.write=con_write};

void setup_console_fds(struct task *t) {
    for (int i = 0; i < 3; i++) {
        struct file *f = kzalloc(sizeof *f,0);
        f->f_op = &console_fops; t->files->fd[i] = f;
    }
}
```

---

## 4. Pitfalls

1. **Refcounting**: every `dget`/`dput`, `iget`/`iput`. Cutting corners early causes UAF when dir entries evict. Use simple refcount + `i_refcount > 0` invariant; defer LRU cache.
2. **Path traversal bugs**: `..` and `.` not handled here — add explicitly or reject.
3. **Symlinks**: we just disable them (`S_ISLNK` returns `-EOPNOTSUPP`).
4. **`O_CLOEXEC`**: not implemented; document.
5. **Errno consistency**: use POSIX numbers (`ENOENT=2`, `EBADF=9`, `EFAULT=14`, `EINVAL=22`, `EMFILE=24`, `ENOSYS=38`).

---

## 5. Verification

A debug filesystem `ramfs` mounted as root (Day 21 makes this initramfs). Quick test:
```c
register_filesystem(&ramfs_type);
do_mount("ramfs", NULL);
struct dentry *d = path_lookup("/"); BUG_ON(!d);
```

User test:
```c
int fd = open("/hello", O_RDONLY);
char b[16]; int n = read(fd, b, 16);
write(1, b, n);
close(fd);
```

---

## 6. Stretch

- Dentry hash table (chained, 256 buckets) keyed by `(parent_ptr, name_hash)`.
- `struct file` ref-counted (`fget/fput`) for `dup`/`dup2`.
- `pollfds[]` precursor: `file->f_op->poll`.

---

## 7. References

- *Linux Kernel Development* (Love) ch.13 (VFS).
- Linux `include/linux/fs.h`.
- xv6 `fs.c`, `file.c` — minimalist reference.
