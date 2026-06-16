#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "ioctl_dev"
#define IOCTL_MAGIC 'k'
#define IOCTL_CLEAR_BUFFER _IO(IOCTL_MAGIC, 0)
#define IOCTL_GET_BUF_LEN  _IOR(IOCTL_MAGIC, 1, int)
#define IOCTL_SET_VALUE    _IOW(IOCTL_MAGIC, 2, int)

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *dev_class;
static char kernel_buf[256];
static int buf_len = 0;
static int stored_value = 0;

static int dev_open(struct inode *inode, struct file *filp) {
    pr_info("%s: Device opened\n", DEVICE_NAME);
    return 0;
}

static int dev_release(struct inode *inode, struct file *filp) {
    pr_info("%s: Device closed\n", DEVICE_NAME);
    return 0;
}

static ssize_t dev_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    int to_copy = min(len, (size_t)buf_len);
    if (to_copy == 0)
        return 0;
    if (copy_to_user(buf, kernel_buf, to_copy))
        return -EFAULT;
    return to_copy;
}

static ssize_t dev_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    int to_copy = min(len, sizeof(kernel_buf));
    if (copy_from_user(kernel_buf, buf, to_copy))
        return -EFAULT;
    buf_len = to_copy;
    return to_copy;
}

static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    int ret = 0;
    
    switch (cmd) {
    case IOCTL_CLEAR_BUFFER:
        memset(kernel_buf, 0, sizeof(kernel_buf));
        buf_len = 0;
        pr_info("%s: Buffer cleared\n", DEVICE_NAME);
        break;
        
    case IOCTL_GET_BUF_LEN:
        if (copy_to_user((int __user *)arg, &buf_len, sizeof(buf_len)))
            return -EFAULT;
        pr_info("%s: Buffer length = %d\n", DEVICE_NAME, buf_len);
        break;
        
    case IOCTL_SET_VALUE:
        if (copy_from_user(&stored_value, (int __user *)arg, sizeof(stored_value)))
            return -EFAULT;
        pr_info("%s: Value set to %d\n", DEVICE_NAME, stored_value);
        break;
        
    default:
        ret = -EINVAL;
    }
    
    return ret;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .read = dev_read,
    .write = dev_write,
    .unlocked_ioctl = dev_ioctl,
};

static int __init ioctl_init(void) {
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        pr_err("%s: Failed to allocate device number\n", DEVICE_NAME);
        return -1;
    }
    
    cdev_init(&my_cdev, &fops);
    if (cdev_add(&my_cdev, dev_num, 1) < 0) {
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    
    dev_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(dev_class, NULL, dev_num, NULL, DEVICE_NAME);
    
    pr_info("%s: Driver loaded\n", DEVICE_NAME);
    return 0;
}

static void __exit ioctl_exit(void) {
    device_destroy(dev_class, dev_num);
    class_destroy(dev_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("%s: Driver unloaded\n", DEVICE_NAME);
}

module_init(ioctl_init);
module_exit(ioctl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil");
MODULE_DESCRIPTION("Basic IOCTL Char Driver");
