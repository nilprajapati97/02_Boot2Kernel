🧠 Linux Process Memory Layout
=================================================================================================
When a program runs on Linux, the OS organizes its memory into well-defined segments to ensure efficient and safe usage of RAM.

📌 Memory Segments (from low to high address)
-------------------------------------------------------------------

🔹 Text Segment
    Contains the executable machine code.
    Read-only to prevent accidental modification.
    Shared among processes running the same binary.

🔹 Data Segment
    Stores initialized global and static variables.Read-write.

🔹 BSS Segment (Block Started by Symbol)
    Holds uninitialized global/static variables.
    Automatically zero-initialized by the OS at startup.

🔹 Heap
    Used for dynamic memory allocation (via malloc, new, etc.).
    Grows upward (towards higher memory addresses) as more memory is requested.

🔹 Memory Mappings
    Includes memory-mapped files, shared memory, or anonymous mappings (via mmap).
    Can appear in flexible locations between heap and stack.

🔹 Shared Libraries
    Dynamically linked libraries (e.g., .so files).
    Mapped into memory usually between heap and stack.

🔹 Stack
    Stores function call frames, local variables, and return addresses.
    Grows downward (towards lower memory addresses).
    Each thread has its own stack.

🔹 Kernel Space
    Resides at the top of the address space.
    Inaccessible to user-space processes.

Used by the kernel for managing hardware, system calls, etc.

🖥️ Address Space Layout
====================================================================================================
32-bit Systems:
    4GB total address space.
    ~3GB for user space, top ~1GB reserved for the kernel.

64-bit Systems:
    Vast addressable space (e.g., 128TB user space).
    More flexibility in layout; memory regions spaced farther apart.

🔒 Security Features
====================================================================================================
ASLR (Address Space Layout Randomization):
Randomizes memory layout of segments (stack, heap, libraries) on each execution.
Makes memory-based attacks more difficult.

🛠️ Inspecting Process Memory
====================================================================================================
Use the following tools:
    cat /proc/<pid>/maps – Displays segment mappings.
    pmap <pid> – Visualizes memory layout.

Example: Running a simple C program and inspecting /proc/<pid>/maps reveals the layout from text 
            at low addresses to stack at high addresses.

This layout ensures that processes run efficiently and securely without interfering with one another.