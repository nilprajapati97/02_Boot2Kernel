Let's walk **end-to-end** through creating a **minimal Linux kernel module (`.ko` file)** and demonstrate:

> ✅ Writing code
> ✅ Compiling with `Makefile`
> ✅ Loading with `insmod`
> ✅ Inspecting with `dmesg`, `/sys/module`, and `readelf`

---

## ✅ Step 1: Write the Kernel Module Code

📁 **Directory structure:**

```
my_module/
├── Makefile
└── hello.c
```

📄 `hello.c`

```c
#include <linux/module.h>
#include <linux/kernel.h>

static int __init hello_init(void)
{
    pr_info("✅ Hello kernel module loaded!\n");
    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("🧹 Hello kernel module unloaded.\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("A minimal example module");
```

---

## ✅ Step 2: Write the `Makefile`

📄 `Makefile`

```make
obj-m += hello.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

---

## ✅ Step 3: Build the Module

```bash
$ make
```

This generates:

```
hello.ko        ← kernel module
hello.o         ← object file
```

---

## ✅ Step 4: Insert into Kernel

```bash
$ sudo insmod hello.ko
```

✅ **Check Logs:**

```bash
$ dmesg | tail
```

You should see:

```
[ 1234.567 ] ✅ Hello kernel module loaded!
```

---

## ✅ Step 5: Inspect the Loaded Module

### 🔍 View in `/sys/module/`

```bash
$ ls /sys/module/hello/
sections/  initstate  refcnt  ...
```

### 🔍 Show Section Addresses:

```bash
$ cat /sys/module/hello/sections/.text
0xffffffffc02a0000
```

### 🔍 Check with `lsmod`:

```bash
$ lsmod | grep hello
hello       16384  0
```

---

## ✅ Step 6: Remove the Module

```bash
$ sudo rmmod hello
$ dmesg | tail
```

You’ll see:

```
🧹 Hello kernel module unloaded.
```

---

## ✅ Step 7: Inspect with `readelf` and `objdump`

```bash
$ readelf -S hello.ko
$ objdump -h hello.ko
```

Look for:

* `.text`: contains `hello_init` and `hello_exit`
* `.modinfo`: contains metadata like license/author
* `.rela.text`: for relocations

---

## ✅ Step 8: Check Symbols (Optional)

```bash
$ nm hello.ko
```

---

## 🧠 Summary

| Task           | Command                    |        |
| -------------- | -------------------------- | ------ |
| Build          | `make`                     |        |
| Load           | `sudo insmod hello.ko`     |        |
| View logs      | \`dmesg                    | tail\` |
| Inspect module | `/sys/module/hello/`       |        |
| Unload         | `sudo rmmod hello`         |        |
| ELF inspection | `readelf`, `objdump`, `nm` |        |

---

Would you like to:

* 🔧 Add parameters to the module?
* 📦 Add device file support?
* 🧪 Auto-load from `udev` or modprobe?

Let me know — I can extend this with GPIO or I2C sensor integration too.
