# Chapter 13 — Block I/O Layer

## Overview

The **Block I/O Layer** manages all I/O to block devices (HDDs, SSDs, NVMe, etc.).

```mermaid
graph TD
    VFS["VFS / Page Cache"] --> BIO["Block I/O Layer\n(struct bio)"]
    BIO --> IOS["I/O Scheduler\n(mq-deadline, bfq, kyber, none)"]
    IOS --> RQ["Request Queue\n(struct request_queue)"]
    RQ --> Driver["Block Device Driver\n(SCSI, NVMe, virtio-blk)"]
    Driver --> HW["Physical Device"]
```

## Topics

1. [01_Block_Devices.md](./01_Block_Devices.md)
2. [02_Bio_Structure.md](./02_Bio_Structure.md)
3. [03_IO_Schedulers.md](./03_IO_Schedulers.md)
4. [04_Request_Queue.md](./04_Request_Queue.md)
5. [05_Block_Driver_Interface.md](./05_Block_Driver_Interface.md)
