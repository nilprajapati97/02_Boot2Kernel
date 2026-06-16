# Day 27 — `devfs`: Device Nodes & Special Files

> **Goal**: Introduce a tiny `devfs` filesystem mounted at `/dev` containing `console`, `null`, `zero`, `random`, `tty`, and `vda` (the block device). Each node has custom `file_operations`. The shell can `cat /dev/zero | head` (figuratively) and writing to `/dev/null` silently succeeds.
>
> **Why today**: Cleanly separates device access from regular files, unblocks `cat`/`tee`-style tools, and prepares for future driver model.

---

## 1. Background

### 1.1 Major / minor numbers
Linux encodes `dev_t` as a packed `(major, minor)`. We adopt the same in a single `u32`:
```c
#define MKDEV(maj,min)   (((maj)<<20) | (min))
#define MAJOR(d) ((d) >> 20)
#define MINOR(d) ((d) & 0xFFFFF)
```
Major numbers we use:
| Major | Device |
|---|---|
| 1 | mem family (`null`, `zero`, `random`) |
| 4 | tty / console |
| 254 | virtio block |

### 1.2 Character vs block
For now everything in `/dev` is **character** (we go through `f_op->read/write` directly). Block access uses dedicated VFS path; `/dev/vda` for raw reads is a thin character wrapper around `block_read/write`.

---

## 2. Design

### 2.1 Files
```
fs/devfs/devfs.c
drivers/char/mem.c          (null, zero, random)
drivers/char/console_chr.c  (/dev/console, /dev/tty)
drivers/block/vda_chr.c     (/dev/vda character view)
```

### 2.2 Registration table
```c
struct devfs_node {
    const char *name;
    u16 mode;                         /* S_IFCHR | 0666 */
    const struct file_operations *fops;
};

static const struct devfs_node devfs_nodes[] = {
    { "console", S_IFCHR | 0600, &console_chr_fops },
    { "tty",     S_IFCHR | 0666, &console_chr_fops },
    { "null",    S_IFCHR | 0666, &null_fops },
    { "zero",    S_IFCHR | 0666, &zero_fops },
    { "random",  S_IFCHR | 0444, &random_fops },
    { "vda",     S_IFCHR | 0600, &vda_chr_fops },
};
```

### 2.3 `devfs` superblock
Pre-creates one inode per entry at mount time and stores them in a fixed-size table. No mkdir/create after mount.

---

## 3. Implementation

### 3.1 Per-device `file_operations`
```c
/* /dev/null */
static long null_read (struct file*f,char __user*b,size_t n,u64*p){ return 0; }
static long null_write(struct file*f,const char __user*b,size_t n,u64*p){ return n; }
const struct file_operations null_fops = {.read=null_read,.write=null_write};

/* /dev/zero */
static long zero_read(struct file*f,char __user*b,size_t n,u64*p){
    char z[64] = {0}; size_t done = 0;
    while (done < n) {
        size_t c = n-done > sizeof z ? sizeof z : n-done;
        if (copy_to_user(b+done, z, c)) return -14;
        done += c;
    }
    return n;
}
const struct file_operations zero_fops = {.read=zero_read,.write=null_write};

/* /dev/random (CNTPCT_EL0-mixed PRNG) */
static u64 rand_state;
static u64 rand_next(void){
    u64 t; asm volatile("mrs %0, cntpct_el0":"=r"(t));
    rand_state ^= t + 0x9E3779B97F4A7C15ULL;
    rand_state = (rand_state * 6364136223846793005ULL) + 1442695040888963407ULL;
    return rand_state;
}
static long random_read(struct file*f,char __user*b,size_t n,u64*p){
    size_t done = 0;
    while (done < n) {
        u64 v = rand_next();
        size_t c = n-done > 8 ? 8 : n-done;
        if (copy_to_user(b+done, &v, c)) return -14;
        done += c;
    }
    return n;
}
const struct file_operations random_fops = {.read=random_read};

/* /dev/console (and /dev/tty) */
static long con_read (struct file*f,char __user*b,size_t n,u64*p){ return uart_read_user(b,n); }
static long con_write(struct file*f,const char __user*b,size_t n,u64*p){ return uart_write_user(b,n); }
const struct file_operations console_chr_fops = {.read=con_read,.write=con_write};

/* /dev/vda character view: pos is byte offset; aligned to 512 */
static long vda_read(struct file*f,char __user*b,size_t n,u64*pos){
    if ((*pos & 511) || (n & 511)) return -22;
    u32 nsec = n / 512;
    void *tmp = kmalloc(n, 0);
    if (block_read(*pos/512, nsec, tmp)) { kfree(tmp); return -5; }
    if (copy_to_user(b, tmp, n)) { kfree(tmp); return -14; }
    kfree(tmp); *pos += n; return n;
}
static long vda_write(struct file*f,const char __user*b,size_t n,u64*pos){
    if ((*pos & 511) || (n & 511)) return -22;
    void *tmp = kmalloc(n, 0);
    if (copy_from_user(tmp, b, n)) { kfree(tmp); return -14; }
    if (block_write(*pos/512, n/512, tmp)) { kfree(tmp); return -5; }
    kfree(tmp); *pos += n; return n;
}
const struct file_operations vda_chr_fops = {.read=vda_read,.write=vda_write};
```

### 3.2 devfs mount
```c
struct dev_inode_priv { const struct devfs_node *node; };

static struct inode *devfs_make_inode(struct super_block *sb, const struct devfs_node *n)
{
    struct inode *i = kzalloc(sizeof *i, 0);
    static u32 ino = 100;
    i->i_ino = ino++; i->i_mode = n->mode; i->i_sb = sb;
    i->i_fop = n->fops;
    i->i_op = &devfs_iops;
    struct dev_inode_priv *p = kzalloc(sizeof *p, 0);
    p->node = n; i->i_private = p;
    return i;
}

static struct dentry *devfs_lookup(struct inode *dir, const char *name)
{
    struct super_block *sb = dir->i_sb;
    struct devfs_node *table = sb->s_fs_info;
    int n = sb->s_fs_info_count;
    for (int k = 0; k < n; k++) {
        if (!strcmp(table[k].name, name)) {
            struct dentry *d = kzalloc(sizeof *d, 0);
            strncpy(d->d_name, name, 63);
            d->d_inode = devfs_make_inode(sb, &table[k]);
            return d;
        }
    }
    return NULL;
}

const struct inode_operations devfs_iops = { .lookup = devfs_lookup };

int devfs_mount(struct file_system_type *t, const void *data, struct super_block **out)
{
    struct super_block *sb = kzalloc(sizeof *sb, 0);
    sb->s_fs_info = (void *)devfs_nodes;
    sb->s_fs_info_count = sizeof(devfs_nodes)/sizeof(devfs_nodes[0]);
    struct inode *root = kzalloc(sizeof *root, 0);
    root->i_ino = 1; root->i_mode = S_IFDIR | 0755; root->i_sb = sb;
    root->i_op = &devfs_iops; root->i_fop = &devfs_dir_fops;
    sb->s_root_inode = root;
    *out = sb;
    return 0;
}

struct file_system_type devfs_type = { .name = "devfs", .mount = devfs_mount };
```

### 3.3 `readdir` for `ls /dev`
```c
static int devfs_readdir(struct file *f, void *ctx,
                         int (*emit)(void*,const char*,int,u64))
{
    struct super_block *sb = f->f_inode->i_sb;
    const struct devfs_node *tab = sb->s_fs_info;
    int n = sb->s_fs_info_count;
    int start = f->f_pos;
    for (int k = start; k < n; k++) {
        if (emit(ctx, tab[k].name, strlen(tab[k].name), 100+k)) break;
        f->f_pos = k + 1;
    }
    return 0;
}
const struct file_operations devfs_dir_fops = { .readdir = devfs_readdir };
```
A `getdents64` syscall (which the user `ls` calls) is left as stretch — small but mechanical.

### 3.4 Mount during init
Kernel `kmain` after the root mount:
```c
register_filesystem(&devfs_type);
mkdir_p(root_dentry, "/dev");           /* in ramfs root */
do_mount_at("devfs", NULL, "/dev");
```
`do_mount_at` is a tiny helper that locks the mountpoint dentry to the new SB's root inode.

### 3.5 Tie `/dev/console` to fd 0/1/2 in user init
Replace the kernel hack `setup_console_fds(t)` with userspace doing:
```c
int fd = open("/dev/console", O_RDWR);
dup3(fd, 0, 0); dup3(fd, 1, 0); dup3(fd, 2, 0); close(fd);
```
in `init.c`.

---

## 4. Pitfalls

1. **Sub-mounts**: mounting devfs over an existing ramfs `/dev` requires a real mount stack. For day 27 we keep one global root + one mounted subtree (`mount[]` array of 1).
2. **`/dev/tty` semantics**: real Linux opens the controlling tty. We alias to console; document.
3. **Random PRNG quality**: do **not** use `/dev/random` for crypto. Mark as "non-cryptographic". Real seeding needs interrupt timing pool — stretch.
4. **`/dev/vda` alignment**: any unaligned write trashes adjacent sectors. We enforce 512-byte alignment with `-EINVAL`.
5. **Inode reuse**: re-`lookup` on a node creates a new inode each time. Add a tiny cache (`node_index → inode`) to avoid leaks.

---

## 5. Verification

```
$ ls /dev
console  tty  null  zero  random  vda
$ cat /dev/null
$ echo > /dev/null
$ /bin/cat /dev/zero | /bin/head    # once pipes are implemented (Day 26 stretch)
$ /bin/cat /dev/random > /tmp/rand
$ /bin/cat /dev/vda > /tmp/raw.img  # binary dump of disk
```

GDB: at `lookup("/dev/null")`, verify `i_fop == &null_fops`.

---

## 6. Stretch

- `getdents64(2)` syscall so user `ls` works without hard-coding.
- `mknod(2)` + extension of `devfs` to be writable (register at runtime).
- Sysfs-style `/sys` with kernel knobs (loglevel, sched stats).
- Real `/dev/random` entropy pool with timer-interrupt jitter.

---

## 7. References

- Linux Documentation/admin-guide/devices.txt (major/minor list).
- `mknod(2)` man page.
- Plan9 `devfs` design — the spiritual ancestor.
