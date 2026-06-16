# Day 21 — initramfs (cpio newc) & `ramfs`

> **Goal**: Load a `newc` cpio archive provided to QEMU via `-initrd`, expand it into an in-memory `ramfs`, and mount it as the initial root `/`. The archive contains `/init`, `/bin/hello`, `/dev/console` (placeholder), and any other early userspace.
>
> **Why today**: Day 22+ touch a real block device; until then we need a real filesystem hierarchy to test syscalls. `execve("/init")` is the kernel's last act before handing control to userland.

---

## 1. Background

### 1.1 newc cpio format
Each member:
```
110-byte ASCII header:
    "070701"                 magic
    8 hex chars: ino
    8 hex chars: mode
    8 hex chars: uid
    8 hex chars: gid
    8 hex chars: nlink
    8 hex chars: mtime
    8 hex chars: filesize
    8 hex chars: devmajor
    8 hex chars: devminor
    8 hex chars: rdevmajor
    8 hex chars: rdevminor
    8 hex chars: namesize  (incl. NUL)
    8 hex chars: check     (0)
Name bytes (namesize), padded to 4-byte boundary
File data (filesize), padded to 4-byte boundary
```
Archive ends with member whose name is `"TRAILER!!!"`.

### 1.2 ramfs
Backing store = kernel slab. Each file's data is a list of pages owned by its inode's `i_private`. Directories store children in a linked list of `(name, inode*)`.

### 1.3 QEMU plumbing
```
-initrd build/initramfs.cpio
```
The bootloader (or QEMU `virt`) puts initrd address + size in the FDT under `/chosen/linux,initrd-start` and `linux,initrd-end`. We parsed `/chosen` already on Day 3.

---

## 2. Design

### 2.1 Files
```
fs/ramfs/ramfs.c
fs/ramfs/cpio.c
mk/initramfs.mk                 (build the cpio image)
```

### 2.2 ramfs in-memory layout
```c
struct ramfs_inode {
    u16 mode;
    u64 size;
    /* directory */
    struct ramfs_dirent *children;     /* singly-linked */
    /* regular */
    void *data;                        /* contiguous kmalloc/vmalloc */
};

struct ramfs_dirent {
    char name[64];
    struct inode *inode;
    struct ramfs_dirent *next;
};
```

### 2.3 cpio expander API
```c
int cpio_populate(struct super_block *sb, const void *image, u64 size);
```

---

## 3. Implementation

### 3.1 cpio parser
```c
struct cpio_newc_hdr {
    char magic[6], ino[8], mode[8], uid[8], gid[8], nlink[8], mtime[8],
         filesize[8], devmajor[8], devminor[8], rdevmajor[8], rdevminor[8],
         namesize[8], check[8];
} __packed;

static u32 hex(const char s[8]) {
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v <<= 4;
        v |= (c <= '9') ? c-'0' : (c|32) - 'a' + 10;
    }
    return v;
}

int cpio_populate(struct super_block *sb, const void *image, u64 size)
{
    const u8 *p = image, *end = p + size;
    while (p + sizeof(struct cpio_newc_hdr) < end) {
        const struct cpio_newc_hdr *h = (void *)p;
        if (memcmp(h->magic, "070701", 6)) return -22;
        u32 nsz   = hex(h->namesize);
        u32 fsz   = hex(h->filesize);
        u32 mode  = hex(h->mode);
        const char *name = (const char *)(h + 1);
        if (!strcmp(name, "TRAILER!!!")) return 0;

        u64 name_end = (u64)(name - (const char *)image) + nsz;
        u64 data_off = (name_end + 3) & ~3ULL;
        const u8 *data = (const u8 *)image + data_off;

        if (S_ISDIR(mode))      ramfs_mkdir_p(sb, name, mode);
        else if (S_ISREG(mode)) ramfs_create_file(sb, name, mode, data, fsz);
        else if (S_ISLNK(mode)) ramfs_symlink(sb, name, (const char*)data, fsz);
        /* devices: we synthesize /dev/console on Day 27 */

        p = data + ((fsz + 3) & ~3ULL);
    }
    return 0;
}
```

### 3.2 ramfs helpers
```c
static struct inode *ramfs_new_inode(struct super_block *sb, u16 mode)
{
    struct inode *i = kzalloc(sizeof *i, 0);
    static u32 next_ino = 2;
    i->i_ino = next_ino++; i->i_mode = mode; i->i_sb = sb;
    i->i_op  = &ramfs_iops;
    i->i_fop = S_ISDIR(mode) ? &ramfs_dir_fops : &ramfs_file_fops;
    i->i_private = kzalloc(sizeof(struct ramfs_inode), 0);
    ((struct ramfs_inode*)i->i_private)->mode = mode;
    return i;
}

static void ramfs_add_child(struct inode *dir, const char *name, struct inode *child)
{
    struct ramfs_inode *rd = dir->i_private;
    struct ramfs_dirent *de = kzalloc(sizeof *de, 0);
    strncpy(de->name, name, 63);
    de->inode = child;
    de->next = rd->children;
    rd->children = de;
}
```

### 3.3 lookup + read
```c
static struct dentry *ramfs_lookup(struct inode *dir, const char *name)
{
    struct ramfs_inode *rd = dir->i_private;
    for (struct ramfs_dirent *de = rd->children; de; de = de->next) {
        if (!strcmp(de->name, name)) {
            struct dentry *d = kzalloc(sizeof *d, 0);
            strncpy(d->d_name, name, 63);
            d->d_inode = de->inode;
            return d;
        }
    }
    return NULL;
}

static long ramfs_read(struct file *f, char __user *buf, size_t n, u64 *pos)
{
    struct ramfs_inode *ri = f->f_inode->i_private;
    if (*pos >= ri->size) return 0;
    if (*pos + n > ri->size) n = ri->size - *pos;
    if (copy_to_user(buf, (u8*)ri->data + *pos, n)) return -14;
    *pos += n;
    return n;
}

static long ramfs_write(struct file *f, const char __user *buf, size_t n, u64 *pos)
{
    struct ramfs_inode *ri = f->f_inode->i_private;
    if (*pos + n > ri->size) {
        void *nb = krealloc(ri->data, *pos + n, 0);
        if (!nb) return -12;
        ri->data = nb; ri->size = *pos + n;
        f->f_inode->i_size = ri->size;
    }
    if (copy_from_user((u8*)ri->data + *pos, buf, n)) return -14;
    *pos += n;
    return n;
}
```

### 3.4 Path creation (`mkdir -p` style)
```c
static struct inode *resolve_or_create_dir(struct super_block *sb, const char *path)
{
    /* Walk components; for each missing component, create dir inode */
    struct inode *cur = sb->s_root_inode;
    const char *p = path[0]=='/' ? path+1 : path;
    while (*p) {
        char name[64]; int n = 0;
        while (*p && *p != '/' && n < 63) name[n++] = *p++;
        name[n] = 0;
        if (*p == '/') p++;
        if (!name[0]) break;
        struct dentry *d = ramfs_lookup(cur, name);
        if (d) { cur = d->d_inode; kfree(d); continue; }
        struct inode *nd = ramfs_new_inode(sb, S_IFDIR | 0755);
        ramfs_add_child(cur, name, nd);
        cur = nd;
    }
    return cur;
}
```

### 3.5 Mount glue
```c
int ramfs_mount(struct file_system_type *t, const void *data, struct super_block **out)
{
    struct super_block *sb = kzalloc(sizeof *sb, 0);
    sb->s_root_inode = ramfs_new_inode(sb, S_IFDIR | 0755);
    *out = sb;
    return 0;
}

struct file_system_type ramfs_type = { .name = "ramfs", .mount = ramfs_mount };
```

### 3.6 Boot sequence (in `kmain` after MM ready)
```c
register_filesystem(&ramfs_type);
do_mount("ramfs", NULL);
u64 ird_pa, ird_sz;
if (fdt_get_initrd(&ird_pa, &ird_sz) == 0) {
    void *ird_va = (void *)(ird_pa + 0xffff000000000000UL);
    cpio_populate(root_sb, ird_va, ird_sz);
    printk("initramfs: %llu bytes unpacked\n", ird_sz);
}
/* Spawn init */
struct task *t = kthread_create(do_init, NULL);
sched_add(t);
```
Where `do_init` does `execve("/init", ...)`.

### 3.7 Building the cpio image
`mk/initramfs.mk`:
```
INITRAMFS_FILES := init bin/hello
INITRAMFS_STAGE := build/initramfs_root

build/initramfs.cpio: $(INITRAMFS_FILES:%=$(INITRAMFS_STAGE)/%)
	cd $(INITRAMFS_STAGE) && find . -print0 | \
	    cpio --null -ov --format=newc > $(abspath $@)

$(INITRAMFS_STAGE)/init: user/init.elf
	@mkdir -p $(@D); cp $< $@; chmod +x $@
```

QEMU invocation:
```
qemu-system-aarch64 ... -kernel build/kernel.img -initrd build/initramfs.cpio
```

---

## 4. Pitfalls

1. **4-byte alignment** of each header and each data block — bug if you forget.
2. **Directory creation order**: cpio guarantees order if you `find . | cpio`, but be defensive: create parents on demand (`mkdir_p`).
3. **Hex parser case**: `newc` uses lowercase hex, but tolerate uppercase.
4. **Symlinks**: store target string in `i_private->data`; resolution can wait until you need them.
5. **Initrd not present**: fall back to a tiny built-in (compile a default `init` that just panics with message).

---

## 5. Verification

```
$ ls build/initramfs_root
init  bin/  etc/
$ qemu-system-aarch64 ... -initrd build/initramfs.cpio
[INFO] initramfs: 8192 bytes unpacked
[INFO] init: exec /init
hello from nkernel
```

Add a tiny `ls`-style debug in the kernel that walks `/` and prints every inode.

---

## 6. Stretch

- Replace cpio with the `initrd` being a raw tarball — keep cpio for compatibility.
- Compress with gzip (`-initrd build/initramfs.cpio.gz`): kernel inflates with embedded inflate.
- `tmpfs` (page-backed) — same code with backing data behind page allocator + page cache.

---

## 7. References

- `cpio(5)` man page, `newc` header description.
- Linux `init/initramfs.c`.
- QEMU `virt` board: how `-initrd` is exposed via FDT `/chosen`.
