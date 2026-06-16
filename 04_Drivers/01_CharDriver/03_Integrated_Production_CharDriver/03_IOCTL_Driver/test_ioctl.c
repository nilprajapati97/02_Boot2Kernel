#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define IOCTL_MAGIC 'k'
#define IOCTL_CLEAR_BUFFER _IO(IOCTL_MAGIC, 0)
#define IOCTL_GET_BUF_LEN  _IOR(IOCTL_MAGIC, 1, int)
#define IOCTL_SET_VALUE    _IOW(IOCTL_MAGIC, 2, int)

int main() {
    int fd = open("/dev/ioctl_dev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    
    // Write data
    write(fd, "Hello IOCTL", 11);
    
    // Get buffer length
    int len;
    ioctl(fd, IOCTL_GET_BUF_LEN, &len);
    printf("Buffer length: %d\n", len);
    
    // Set value
    int val = 42;
    ioctl(fd, IOCTL_SET_VALUE, &val);
    printf("Value set to: %d\n", val);
    
    // Clear buffer
    ioctl(fd, IOCTL_CLEAR_BUFFER);
    printf("Buffer cleared\n");
    
    // Check length after clear
    ioctl(fd, IOCTL_GET_BUF_LEN, &len);
    printf("Buffer length after clear: %d\n", len);
    
    close(fd);
    return 0;
}
