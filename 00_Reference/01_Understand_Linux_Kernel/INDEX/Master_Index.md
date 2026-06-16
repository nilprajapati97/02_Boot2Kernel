# Master Index — Linux Kernel Development (Robert Love, 3rd Ed.)

> Alphabetical index of all concepts across all chapters.

---

## A

| Concept | Chapter | File |
|---------|---------|------|
| address_space struct | Ch15 | [15_Page_Cache_And_Page_Writeback/02_address_space.md](../15_Page_Cache_And_Page_Writeback/02_address_space.md) |
| address_space_operations | Ch15 | [15_Page_Cache_And_Page_Writeback/02_address_space.md](../15_Page_Cache_And_Page_Writeback/02_address_space.md) |
| alloc_pages() | Ch11 | [11_Memory_Management/02_Buddy_Allocator.md](../11_Memory_Management/02_Buddy_Allocator.md) |
| alloc_percpu() | Ch11 | [11_Memory_Management/06_Per_CPU_Allocations.md](../11_Memory_Management/06_Per_CPU_Allocations.md) |
| atomic_t | Ch09 | [09_Kernel_Synchronization_Methods/01_Atomic_Operations.md](../09_Kernel_Synchronization_Methods/01_Atomic_Operations.md) |
| atomic operations | Ch09 | [09_Kernel_Synchronization_Methods/01_Atomic_Operations.md](../09_Kernel_Synchronization_Methods/01_Atomic_Operations.md) |

---

## B

| Concept | Chapter | File |
|---------|---------|------|
| BFQ I/O scheduler | Ch13 | [13_Block_IO_Layer/03_IO_Schedulers.md](../13_Block_IO_Layer/03_IO_Schedulers.md) |
| bio structure | Ch13 | [13_Block_IO_Layer/02_Bio_Structure.md](../13_Block_IO_Layer/02_Bio_Structure.md) |
| bio_vec (scatter-gather) | Ch13 | [13_Block_IO_Layer/02_Bio_Structure.md](../13_Block_IO_Layer/02_Bio_Structure.md) |
| block device | Ch13 | [13_Block_IO_Layer/01_Block_Devices.md](../13_Block_IO_Layer/01_Block_Devices.md) |
| block driver interface | Ch13 | [13_Block_IO_Layer/05_Block_Driver_Interface.md](../13_Block_IO_Layer/05_Block_Driver_Interface.md) |
| bottom halves | Ch07 | [07_Bottom_Halves_And_Deferring_Work/01_Why_Bottom_Halves.md](../07_Bottom_Halves_And_Deferring_Work/01_Why_Bottom_Halves.md) |
| brk / sbrk (heap) | Ch14 | [14_Process_Address_Space/05_mmap.md](../14_Process_Address_Space/05_mmap.md) |
| buddy allocator | Ch11 | [11_Memory_Management/02_Buddy_Allocator.md](../11_Memory_Management/02_Buddy_Allocator.md) |
| bus_type | Ch16 | [16_Devices_And_Modules/01_Device_Model.md](../16_Devices_And_Modules/01_Device_Model.md) |

---

## C

| Concept | Chapter | File |
|---------|---------|------|
| CFS (Completely Fair Scheduler) | Ch03 | [03_Process_Scheduling/02_CFS_Completely_Fair_Scheduler.md](../03_Process_Scheduling/02_CFS_Completely_Fair_Scheduler.md) |
| clone() | Ch02 | [02_Process_Management/03_Process_Creation_fork_clone.md](../02_Process_Management/03_Process_Creation_fork_clone.md) |
| clockevents | Ch10 | [10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md](../10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md) |
| clocksource | Ch10 | [10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md](../10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md) |
| completion variables | Ch09 | [09_Kernel_Synchronization_Methods/06_Completion_Variables.md](../09_Kernel_Synchronization_Methods/06_Completion_Variables.md) |
| concurrency in kernel | Ch08 | [08_Intro_To_Kernel_Synchronization/05_Concurrency_In_Kernel.md](../08_Intro_To_Kernel_Synchronization/05_Concurrency_In_Kernel.md) |
| container_of macro | Ch05 | [05_Kernel_Data_Structures/01_Linked_Lists.md](../05_Kernel_Data_Structures/01_Linked_Lists.md) |
| copy_from_user / copy_to_user | Ch04 | [04_System_Calls/05_Parameter_Passing.md](../04_System_Calls/05_Parameter_Passing.md) |
| copy-on-write (COW) | Ch02 | [02_Process_Management/04_Copy_On_Write.md](../02_Process_Management/04_Copy_On_Write.md) |
| COW page fault | Ch14 | [14_Process_Address_Space/04_Page_Faults.md](../14_Process_Address_Space/04_Page_Faults.md) |
| critical region | Ch08 | [08_Intro_To_Kernel_Synchronization/01_Critical_Regions.md](../08_Intro_To_Kernel_Synchronization/01_Critical_Regions.md) |

---

## D

| Concept | Chapter | File |
|---------|---------|------|
| dcache (dentry cache) | Ch12 | [12_Virtual_Filesystem/06_Directory_Entry_Cache.md](../12_Virtual_Filesystem/06_Directory_Entry_Cache.md) |
| deadlocks | Ch08 | [08_Intro_To_Kernel_Synchronization/04_Deadlocks.md](../08_Intro_To_Kernel_Synchronization/04_Deadlocks.md) |
| demand paging | Ch14 | [14_Process_Address_Space/04_Page_Faults.md](../14_Process_Address_Space/04_Page_Faults.md) |
| dentry struct | Ch12 | [12_Virtual_Filesystem/04_Dentry.md](../12_Virtual_Filesystem/04_Dentry.md) |
| dentry_operations | Ch12 | [12_Virtual_Filesystem/04_Dentry.md](../12_Virtual_Filesystem/04_Dentry.md) |
| devm_* managed resources | Ch16 | [16_Devices_And_Modules/01_Device_Model.md](../16_Devices_And_Modules/01_Device_Model.md) |
| device class | Ch16 | [16_Devices_And_Modules/07_Device_Classes.md](../16_Devices_And_Modules/07_Device_Classes.md) |
| device driver | Ch16 | [16_Devices_And_Modules/01_Device_Model.md](../16_Devices_And_Modules/01_Device_Model.md) |
| device model | Ch16 | [16_Devices_And_Modules/01_Device_Model.md](../16_Devices_And_Modules/01_Device_Model.md) |
| device tree (DT) | Ch16 | [16_Devices_And_Modules/04_Platform_Devices.md](../16_Devices_And_Modules/04_Platform_Devices.md) |
| dirty pages | Ch15 | [15_Page_Cache_And_Page_Writeback/04_Dirty_Page_Tracking.md](../15_Page_Cache_And_Page_Writeback/04_Dirty_Page_Tracking.md) |
| dynamic debug (dyndbg) | Ch17 | [17_Debugging/06_Dynamic_Debug.md](../17_Debugging/06_Dynamic_Debug.md) |

---

## E

| Concept | Chapter | File |
|---------|---------|------|
| ELF binary format | Ch02 | [02_Process_Management/01_What_Is_A_Process.md](../02_Process_Management/01_What_Is_A_Process.md) |
| EXPORT_SYMBOL | Ch16 | [16_Devices_And_Modules/05_Module_Loading.md](../16_Devices_And_Modules/05_Module_Loading.md) |

---

## F

| Concept | Chapter | File |
|---------|---------|------|
| fdtable / file descriptors | Ch12 | [12_Virtual_Filesystem/05_File_Object.md](../12_Virtual_Filesystem/05_File_Object.md) |
| file_operations | Ch12 | [12_Virtual_Filesystem/05_File_Object.md](../12_Virtual_Filesystem/05_File_Object.md) |
| file object (struct file) | Ch12 | [12_Virtual_Filesystem/05_File_Object.md](../12_Virtual_Filesystem/05_File_Object.md) |
| fork() | Ch02 | [02_Process_Management/03_Process_Creation_fork_clone.md](../02_Process_Management/03_Process_Creation_fork_clone.md) |
| free_area (buddy) | Ch11 | [11_Memory_Management/02_Buddy_Allocator.md](../11_Memory_Management/02_Buddy_Allocator.md) |
| free_irq() | Ch06 | [06_Interrupts_And_Interrupt_Handlers/03_Registering_An_Interrupt_Handler.md](../06_Interrupts_And_Interrupt_Handlers/03_Registering_An_Interrupt_Handler.md) |
| fsync() | Ch15 | [15_Page_Cache_And_Page_Writeback/03_Writeback_Mechanism.md](../15_Page_Cache_And_Page_Writeback/03_Writeback_Mechanism.md) |
| ftrace | Ch17 | [17_Debugging/04_ftrace.md](../17_Debugging/04_ftrace.md) |

---

## G

| Concept | Chapter | File |
|---------|---------|------|
| gendisk | Ch13 | [13_Block_IO_Layer/01_Block_Devices.md](../13_Block_IO_Layer/01_Block_Devices.md) |
| GFP flags | Ch11 | [11_Memory_Management/05_GFP_Flags.md](../11_Memory_Management/05_GFP_Flags.md) |
| GFP_ATOMIC | Ch11 | [11_Memory_Management/05_GFP_Flags.md](../11_Memory_Management/05_GFP_Flags.md) |
| GFP_KERNEL | Ch11 | [11_Memory_Management/05_GFP_Flags.md](../11_Memory_Management/05_GFP_Flags.md) |
| get_cpu_var() | Ch11 | [11_Memory_Management/06_Per_CPU_Allocations.md](../11_Memory_Management/06_Per_CPU_Allocations.md) |

---

## H

| Concept | Chapter | File |
|---------|---------|------|
| hlist (hash list) | Ch05 | [05_Kernel_Data_Structures/01_Linked_Lists.md](../05_Kernel_Data_Structures/01_Linked_Lists.md) |
| hrtimer | Ch10 | [10_Timers_And_Time_Management/03_High_Resolution_Timers.md](../10_Timers_And_Time_Management/03_High_Resolution_Timers.md) |
| huge pages | Ch14 | [14_Process_Address_Space/03_Page_Tables.md](../14_Process_Address_Space/03_Page_Tables.md) |

---

## I

| Concept | Chapter | File |
|---------|---------|------|
| IDR (ID radix tree) | Ch05 | [05_Kernel_Data_Structures/03_Maps_idr.md](../05_Kernel_Data_Structures/03_Maps_idr.md) |
| IDT (interrupt descriptor table) | Ch06 | [06_Interrupts_And_Interrupt_Handlers/01_Interrupt_Basics.md](../06_Interrupts_And_Interrupt_Handlers/01_Interrupt_Basics.md) |
| inode struct | Ch12 | [12_Virtual_Filesystem/03_Inode.md](../12_Virtual_Filesystem/03_Inode.md) |
| inode_operations | Ch12 | [12_Virtual_Filesystem/03_Inode.md](../12_Virtual_Filesystem/03_Inode.md) |
| interrupt handler | Ch06 | [06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md](../06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md) |
| interrupt control | Ch06 | [06_Interrupts_And_Interrupt_Handlers/05_Interrupt_Control.md](../06_Interrupts_And_Interrupt_Handlers/05_Interrupt_Control.md) |
| IRQ stacks | Ch06 | [06_Interrupts_And_Interrupt_Handlers/04_IRQ_Stacks.md](../06_Interrupts_And_Interrupt_Handlers/04_IRQ_Stacks.md) |
| irqreturn_t | Ch06 | [06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md](../06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md) |

---

## J

| Concept | Chapter | File |
|---------|---------|------|
| jiffies | Ch10 | [10_Timers_And_Time_Management/01_Jiffies_And_HZ.md](../10_Timers_And_Time_Management/01_Jiffies_And_HZ.md) |

---

## K

| Concept | Chapter | File |
|---------|---------|------|
| KASAN | Ch17 | [17_Debugging/03_KASAN_KCSAN.md](../17_Debugging/03_KASAN_KCSAN.md) |
| kcalloc() | Ch11 | [11_Memory_Management/04_kmalloc_And_vmalloc.md](../11_Memory_Management/04_kmalloc_And_vmalloc.md) |
| KCSAN | Ch17 | [17_Debugging/03_KASAN_KCSAN.md](../17_Debugging/03_KASAN_KCSAN.md) |
| KFENCE | Ch17 | [17_Debugging/03_KASAN_KCSAN.md](../17_Debugging/03_KASAN_KCSAN.md) |
| KGDB | Ch17 | [17_Debugging/02_KGDB.md](../17_Debugging/02_KGDB.md) |
| kfifo | Ch05 | [05_Kernel_Data_Structures/02_Queues_kfifo.md](../05_Kernel_Data_Structures/02_Queues_kfifo.md) |
| kmalloc() | Ch11 | [11_Memory_Management/04_kmalloc_And_vmalloc.md](../11_Memory_Management/04_kmalloc_And_vmalloc.md) |
| kmem_cache | Ch11 | [11_Memory_Management/03_Slab_Allocator.md](../11_Memory_Management/03_Slab_Allocator.md) |
| kmem_cache_alloc() | Ch11 | [11_Memory_Management/03_Slab_Allocator.md](../11_Memory_Management/03_Slab_Allocator.md) |
| kmem_cache_create() | Ch11 | [11_Memory_Management/03_Slab_Allocator.md](../11_Memory_Management/03_Slab_Allocator.md) |
| kobject | Ch16 | [16_Devices_And_Modules/02_kobject.md](../16_Devices_And_Modules/02_kobject.md) |
| kref | Ch16 | [16_Devices_And_Modules/02_kobject.md](../16_Devices_And_Modules/02_kobject.md) |
| krealloc() | Ch11 | [11_Memory_Management/04_kmalloc_And_vmalloc.md](../11_Memory_Management/04_kmalloc_And_vmalloc.md) |
| kset | Ch16 | [16_Devices_And_Modules/02_kobject.md](../16_Devices_And_Modules/02_kobject.md) |
| ktime_t | Ch10 | [10_Timers_And_Time_Management/03_High_Resolution_Timers.md](../10_Timers_And_Time_Management/03_High_Resolution_Timers.md) |
| kworker/flush | Ch15 | [15_Page_Cache_And_Page_Writeback/05_pdflush_kworker.md](../15_Page_Cache_And_Page_Writeback/05_pdflush_kworker.md) |
| kzalloc() | Ch11 | [11_Memory_Management/04_kmalloc_And_vmalloc.md](../11_Memory_Management/04_kmalloc_And_vmalloc.md) |

---

## L

| Concept | Chapter | File |
|---------|---------|------|
| list_head | Ch05 | [05_Kernel_Data_Structures/01_Linked_Lists.md](../05_Kernel_Data_Structures/01_Linked_Lists.md) |
| list_for_each_entry | Ch05 | [05_Kernel_Data_Structures/01_Linked_Lists.md](../05_Kernel_Data_Structures/01_Linked_Lists.md) |
| load balancing | Ch03 | [03_Process_Scheduling/07_Load_Balancing.md](../03_Process_Scheduling/07_Load_Balancing.md) |
| lockdep | Ch08 | [08_Intro_To_Kernel_Synchronization/04_Deadlocks.md](../08_Intro_To_Kernel_Synchronization/04_Deadlocks.md) |
| LRU (page eviction) | Ch15 | [15_Page_Cache_And_Page_Writeback/01_Page_Cache_Overview.md](../15_Page_Cache_And_Page_Writeback/01_Page_Cache_Overview.md) |

---

## M

| Concept | Chapter | File |
|---------|---------|------|
| memory barriers | Ch09 | [09_Kernel_Synchronization_Methods/09_Memory_Ordering.md](../09_Kernel_Synchronization_Methods/09_Memory_Ordering.md) |
| memory ordering | Ch09 | [09_Kernel_Synchronization_Methods/09_Memory_Ordering.md](../09_Kernel_Synchronization_Methods/09_Memory_Ordering.md) |
| mmap() | Ch14 | [14_Process_Address_Space/05_mmap.md](../14_Process_Address_Space/05_mmap.md) |
| mm_struct | Ch14 | [14_Process_Address_Space/01_mm_struct.md](../14_Process_Address_Space/01_mm_struct.md) |
| module loading | Ch16 | [16_Devices_And_Modules/05_Module_Loading.md](../16_Devices_And_Modules/05_Module_Loading.md) |
| module parameters | Ch16 | [16_Devices_And_Modules/06_Module_Parameters.md](../16_Devices_And_Modules/06_Module_Parameters.md) |
| module_init / module_exit | Ch16 | [16_Devices_And_Modules/05_Module_Loading.md](../16_Devices_And_Modules/05_Module_Loading.md) |
| mq-deadline scheduler | Ch13 | [13_Block_IO_Layer/03_IO_Schedulers.md](../13_Block_IO_Layer/03_IO_Schedulers.md) |
| mutex | Ch09 | [09_Kernel_Synchronization_Methods/05_Mutex.md](../09_Kernel_Synchronization_Methods/05_Mutex.md) |

---

## N

| Concept | Chapter | File |
|---------|---------|------|
| NUMA nodes | Ch11 | [11_Memory_Management/01_Pages_And_Zones.md](../11_Memory_Management/01_Pages_And_Zones.md) |
| NO_HZ / tickless | Ch10 | [10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md](../10_Timers_And_Time_Management/05_Clocksource_And_Clockevents.md) |

---

## P

| Concept | Chapter | File |
|---------|---------|------|
| page cache | Ch15 | [15_Page_Cache_And_Page_Writeback/01_Page_Cache_Overview.md](../15_Page_Cache_And_Page_Writeback/01_Page_Cache_Overview.md) |
| page faults | Ch14 | [14_Process_Address_Space/04_Page_Faults.md](../14_Process_Address_Space/04_Page_Faults.md) |
| page tables | Ch14 | [14_Process_Address_Space/03_Page_Tables.md](../14_Process_Address_Space/03_Page_Tables.md) |
| perf tool | Ch17 | [17_Debugging/05_perf.md](../17_Debugging/05_perf.md) |
| per-CPU allocations | Ch11 | [11_Memory_Management/06_Per_CPU_Allocations.md](../11_Memory_Management/06_Per_CPU_Allocations.md) |
| pglist_data (NUMA) | Ch11 | [11_Memory_Management/01_Pages_And_Zones.md](../11_Memory_Management/01_Pages_And_Zones.md) |
| platform device | Ch16 | [16_Devices_And_Modules/04_Platform_Devices.md](../16_Devices_And_Modules/04_Platform_Devices.md) |
| platform driver | Ch16 | [16_Devices_And_Modules/04_Platform_Devices.md](../16_Devices_And_Modules/04_Platform_Devices.md) |
| preemption | Ch03 | [03_Process_Scheduling/05_Preemption.md](../03_Process_Scheduling/05_Preemption.md) |
| printk | Ch17 | [17_Debugging/01_printk.md](../17_Debugging/01_printk.md) |
| process descriptor (task_struct) | Ch02 | [02_Process_Management/02_Process_Descriptor_task_struct.md](../02_Process_Management/02_Process_Descriptor_task_struct.md) |
| process termination | Ch02 | [02_Process_Management/05_Process_Termination.md](../02_Process_Management/05_Process_Termination.md) |

---

## R

| Concept | Chapter | File |
|---------|---------|------|
| race conditions | Ch08 | [08_Intro_To_Kernel_Synchronization/02_Race_Conditions.md](../08_Intro_To_Kernel_Synchronization/02_Race_Conditions.md) |
| radix trees | Ch05 | [05_Kernel_Data_Structures/05_Radix_Trees.md](../05_Kernel_Data_Structures/05_Radix_Trees.md) |
| rb_tree (red-black) | Ch05 | [05_Kernel_Data_Structures/04_Red_Black_Trees.md](../05_Kernel_Data_Structures/04_Red_Black_Trees.md) |
| RCU (read-copy-update) | Ch09 | [09_Kernel_Synchronization_Methods/08_RCU.md](../09_Kernel_Synchronization_Methods/08_RCU.md) |
| real-time scheduling | Ch03 | [03_Process_Scheduling/06_Real_Time_Scheduling.md](../03_Process_Scheduling/06_Real_Time_Scheduling.md) |
| refcount_t | Ch09 | [09_Kernel_Synchronization_Methods/01_Atomic_Operations.md](../09_Kernel_Synchronization_Methods/01_Atomic_Operations.md) |
| request_queue | Ch13 | [13_Block_IO_Layer/04_Request_Queue.md](../13_Block_IO_Layer/04_Request_Queue.md) |
| request_irq() | Ch06 | [06_Interrupts_And_Interrupt_Handlers/03_Registering_An_Interrupt_Handler.md](../06_Interrupts_And_Interrupt_Handlers/03_Registering_An_Interrupt_Handler.md) |
| run queue | Ch03 | [03_Process_Scheduling/03_Run_Queue_And_Red_Black_Tree.md](../03_Process_Scheduling/03_Run_Queue_And_Red_Black_Tree.md) |
| rwlock_t (reader-writer spinlock) | Ch09 | [09_Kernel_Synchronization_Methods/03_Reader_Writer_Spin_Locks.md](../09_Kernel_Synchronization_Methods/03_Reader_Writer_Spin_Locks.md) |
| rwsem (rw semaphore) | Ch09 | [09_Kernel_Synchronization_Methods/03_Reader_Writer_Spin_Locks.md](../09_Kernel_Synchronization_Methods/03_Reader_Writer_Spin_Locks.md) |

---

## S

| Concept | Chapter | File |
|---------|---------|------|
| scheduling policy | Ch03 | [03_Process_Scheduling/01_Scheduling_Policy_And_Priority.md](../03_Process_Scheduling/01_Scheduling_Policy_And_Priority.md) |
| semaphore | Ch09 | [09_Kernel_Synchronization_Methods/04_Semaphores.md](../09_Kernel_Synchronization_Methods/04_Semaphores.md) |
| seq_lock / seqcount | Ch09 | [09_Kernel_Synchronization_Methods/07_Seq_Locks.md](../09_Kernel_Synchronization_Methods/07_Seq_Locks.md) |
| slab allocator | Ch11 | [11_Memory_Management/03_Slab_Allocator.md](../11_Memory_Management/03_Slab_Allocator.md) |
| SLUB allocator | Ch11 | [11_Memory_Management/03_Slab_Allocator.md](../11_Memory_Management/03_Slab_Allocator.md) |
| softirqs | Ch07 | [07_Bottom_Halves_And_Deferring_Work/02_Softirqs.md](../07_Bottom_Halves_And_Deferring_Work/02_Softirqs.md) |
| spin_lock | Ch09 | [09_Kernel_Synchronization_Methods/02_Spin_Locks.md](../09_Kernel_Synchronization_Methods/02_Spin_Locks.md) |
| spin_lock_irqsave | Ch09 | [09_Kernel_Synchronization_Methods/02_Spin_Locks.md](../09_Kernel_Synchronization_Methods/02_Spin_Locks.md) |
| struct page | Ch11 | [11_Memory_Management/01_Pages_And_Zones.md](../11_Memory_Management/01_Pages_And_Zones.md) |
| super_block | Ch12 | [12_Virtual_Filesystem/02_Superblock.md](../12_Virtual_Filesystem/02_Superblock.md) |
| super_operations | Ch12 | [12_Virtual_Filesystem/02_Superblock.md](../12_Virtual_Filesystem/02_Superblock.md) |
| SYSCALL_DEFINE | Ch04 | [04_System_Calls/04_Adding_A_New_System_Call.md](../04_System_Calls/04_Adding_A_New_System_Call.md) |
| system call handler | Ch04 | [04_System_Calls/02_System_Call_Handler.md](../04_System_Calls/02_System_Call_Handler.md) |
| system call table | Ch04 | [04_System_Calls/03_System_Call_Table.md](../04_System_Calls/03_System_Call_Table.md) |
| sysfs | Ch16 | [16_Devices_And_Modules/03_sysfs.md](../16_Devices_And_Modules/03_sysfs.md) |

---

## T

| Concept | Chapter | File |
|---------|---------|------|
| task_struct | Ch02 | [02_Process_Management/02_Process_Descriptor_task_struct.md](../02_Process_Management/02_Process_Descriptor_task_struct.md) |
| tasklet | Ch07 | [07_Bottom_Halves_And_Deferring_Work/03_Tasklets.md](../07_Bottom_Halves_And_Deferring_Work/03_Tasklets.md) |
| this_cpu_inc / this_cpu_* | Ch11 | [11_Memory_Management/06_Per_CPU_Allocations.md](../11_Memory_Management/06_Per_CPU_Allocations.md) |
| threads in Linux | Ch02 | [02_Process_Management/06_Threads_In_Linux.md](../02_Process_Management/06_Threads_In_Linux.md) |
| timer_list (kernel timer) | Ch10 | [10_Timers_And_Time_Management/02_Kernel_Timers.md](../10_Timers_And_Time_Management/02_Kernel_Timers.md) |
| TLB (translation lookaside buffer) | Ch14 | [14_Process_Address_Space/03_Page_Tables.md](../14_Process_Address_Space/03_Page_Tables.md) |
| TOCTOU attack | Ch04 | [04_System_Calls/05_Parameter_Passing.md](../04_System_Calls/05_Parameter_Passing.md) |
| tracepoints | Ch17 | [17_Debugging/04_ftrace.md](../17_Debugging/04_ftrace.md) |
| threaded IRQ | Ch06 | [06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md](../06_Interrupts_And_Interrupt_Handlers/02_Interrupt_Handlers.md) |

---

## U

| Concept | Chapter | File |
|---------|---------|------|
| uevent (hotplug) | Ch16 | [16_Devices_And_Modules/03_sysfs.md](../16_Devices_And_Modules/03_sysfs.md) |

---

## V

| Concept | Chapter | File |
|---------|---------|------|
| VFS (virtual filesystem) | Ch12 | [12_Virtual_Filesystem/01_VFS_Overview.md](../12_Virtual_Filesystem/01_VFS_Overview.md) |
| virtual memory areas (VMA) | Ch14 | [14_Process_Address_Space/02_Virtual_Memory_Areas.md](../14_Process_Address_Space/02_Virtual_Memory_Areas.md) |
| vm_area_struct | Ch14 | [14_Process_Address_Space/02_Virtual_Memory_Areas.md](../14_Process_Address_Space/02_Virtual_Memory_Areas.md) |
| vm_flags | Ch14 | [14_Process_Address_Space/02_Virtual_Memory_Areas.md](../14_Process_Address_Space/02_Virtual_Memory_Areas.md) |
| vmalloc() | Ch11 | [11_Memory_Management/04_kmalloc_And_vmalloc.md](../11_Memory_Management/04_kmalloc_And_vmalloc.md) |

---

## W

| Concept | Chapter | File |
|---------|---------|------|
| workqueue | Ch07 | [07_Bottom_Halves_And_Deferring_Work/04_Work_Queues.md](../07_Bottom_Halves_And_Deferring_Work/04_Work_Queues.md) |
| WRITE_ONCE / READ_ONCE | Ch09 | [09_Kernel_Synchronization_Methods/09_Memory_Ordering.md](../09_Kernel_Synchronization_Methods/09_Memory_Ordering.md) |
| writeback | Ch15 | [15_Page_Cache_And_Page_Writeback/03_Writeback_Mechanism.md](../15_Page_Cache_And_Page_Writeback/03_Writeback_Mechanism.md) |

---

## X

| Concept | Chapter | File |
|---------|---------|------|
| XArray | Ch05 | [05_Kernel_Data_Structures/03_Maps_idr.md](../05_Kernel_Data_Structures/03_Maps_idr.md) |

---

## Z

| Concept | Chapter | File |
|---------|---------|------|
| zones (ZONE_DMA, ZONE_NORMAL, etc.) | Ch11 | [11_Memory_Management/01_Pages_And_Zones.md](../11_Memory_Management/01_Pages_And_Zones.md) |
