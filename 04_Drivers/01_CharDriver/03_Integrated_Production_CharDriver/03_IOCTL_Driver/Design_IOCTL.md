# Design: Basic IOCTL Char Driver
## Kernel Control Interface

---

## 1. What is IOCTL?

`ioctl` (I/O control) provides **out-of-band** communication with devices — operations beyond read/write.

```
User Space                  Kernel Space
   │
   ioctl(fd, cmd, arg)
   │
   └──> sys_ioctl()
           │
           └──> vfs_ioctl()
                   │
                   └──> dev_ioctl()  ← Your handler
```

---

## 2. IOCTL Command Encoding

Linux uses 32-bit command numbers with embedded metadata:

```c
_IO(magic, nr)           // No data transfer
_IOR(magic, nr, type)    // Kernel → User (Read)
_IOW(magic, nr, type)    // User → Kernel (Write)
_IOWR(magic, nr, type)   // Bidirectional
```

**Bit Layout:**
```
[31:30] Direction (00=none, 10=read, 01=write, 11=both)
[29:16] Data size
[15:8]  Magic number (driver ID)
[7:0]   Command number
```

---

## 3. Implementation

### Commands Defined

```c
#define IOCTL_MAGIC 'k'
#define IOCTL_CLEAR_BUFFER _IO(IOCTL_MAGIC, 0)      // Clear buffer
#define IOCTL_GET_BUF_LEN  _IOR(IOCTL_MAGIC, 1, int) // Get length
#define IOCTL_SET_VALUE    _IOW(IOCTL_MAGIC, 2, int) // Set value
```

### Handler Function

```c
static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case IOCTL_CLEAR_BUFFER:
        memset(kernel_buf, 0, sizeof(kernel_buf));
        buf_len = 0;
        break;
        
    case IOCTL_GET_BUF_LEN:
        copy_to_user((int __user *)arg, &buf_len, sizeof(buf_len));
        break;
        
    case IOCTL_SET_VALUE:
        copy_from_user(&stored_value, (int __user *)arg, sizeof(stored_value));
        break;
        
    default:
        return -EINVAL;
    }
    return 0;
}
```

---

## 4. Why unlocked_ioctl?

Old `ioctl` held the **Big Kernel Lock (BKL)** — removed for performance.

`unlocked_ioctl` = **you manage locking** (mutex/spinlock).

```c
static struct file_operations fops = {
    .unlocked_ioctl = dev_ioctl,  // Modern interface
};
```

---

## 5. Data Transfer

### Kernel → User (_IOR)

```c
copy_to_user((void __user *)arg, &kernel_data, size);
```

### User → Kernel (_IOW)

```c
copy_from_user(&kernel_data, (void __user *)arg, size);
```

Both return **0 on success**, non-zero on fault.

---

## 6. Build and Test

```bash
# Build driver
make

# Load module
sudo insmod ioctl_driver.ko

# Verify device
ls -l /dev/ioctl_dev

# Compile test app
gcc test_ioctl.c -o test_ioctl

# Run test
sudo ./test_ioctl

# Check kernel logs
dmesg | tail

# Unload
sudo rmmod ioctl_driver
```

---

## 7. Expected Output

```
Buffer length: 11
Value set to: 42
Buffer cleared
Buffer length after clear: 0
```

**Kernel logs:**
```
ioctl_dev: Device opened
ioctl_dev: Buffer length = 11
ioctl_dev: Value set to 42
ioctl_dev: Buffer cleared
ioctl_dev: Device closed
```

---

## 8. Key Concepts

| Concept | Purpose |
|---------|---------|
| `_IO` | Command with no data |
| `_IOR` | Read data from kernel |
| `_IOW` | Write data to kernel |
| `unlocked_ioctl` | Modern handler without BKL |
| `copy_to_user` | Safe kernel→user transfer |
| `copy_from_user` | Safe user→kernel transfer |

---

## Summary

IOCTL provides a **control channel** for device configuration without using read/write. Commands are encoded with direction and size metadata, and data transfer uses safe copy functions to prevent kernel memory corruption.
