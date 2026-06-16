What do these two Linux commands have in common?

cat my_document.txt
cat /sys/class/thermal/thermal_zone0/temp

One reads a text file from your disk. The other reads your CPU's live temperature from a hardware sensor. Yet the same cat command works for both. 

This is Linux's "everything is a file" philosophy in action—but how do identical commands work for both a text document and a temperature sensor?

The answer lies in the Linux kernel's Virtual Filesystem (VFS) layer, an abstraction that acts as a universal translator between user applications and the underlying hardware. 

When you open any "file," VFS intercepts the request and routes it to the appropriate handler:

1. Regular file → ext4/xfs filesystem → block device → disk/SSD
2. Temperature sensor → sysfs → thermal subsystem → hardware sensor 
3. Serial port → devtmpfs → TTY driver → UART hardware
4. Process info → procfs → kernel memory structures

Below are the key components of the VFS:

1. Superblock: Represents each mounted filesystem and contains metadata about how to interact with it.
2. Inode: The fundamental object representing any file-like entity—whether it's a regular file, device, or virtual data source.
3. Dentry (Directory Entry): Maps human-readable paths to inodes, with intelligent caching to speed up repeated lookups.
4. File Operations: Structure containing function pointers that define how to read, write, open, and close each type of "file."

Here's what happens when you run cat /sys/class/thermal/thermal_zone0/temp:

1. VFS resolves the path through the dentry cache
2. Finds the inode representing the temperature sensor
3. Calls the appropriate read operation from sysfs
4. sysfs forwards the request to the thermal subsystem
5. The thermal driver reads the actual hardware sensor
6. Data flows back up the stack to your terminal

The same process applies to regular files, with different handlers at each layer.

The diagram below shows the complete data flow when applications perform file operations, demonstrating how VFS routes requests through different kernel subsystems to reach the appropriate backend layers.