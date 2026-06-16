You can say this in interview:

`smp_setup_processor_id()` is an early boot function that lets Linux identify the boot CPU on SMP systems.
On ARMv8, it reads the core’s physical ID from `MPIDR_EL1`, masks valid affinity bits, and stores that in the kernel CPU mapping table.
This creates the logical-to-physical CPU identity foundation before scheduler, per-CPU memory, and interrupt setup proceed.
It runs very early in main.c, right after entering `start_kernel()`.
In short: hardware gives each core a unique ID, and this function is Linux’s first step to build consistent CPU topology.

If you want a 15-second version:
“`smp_setup_processor_id()` reads ARMv8 `MPIDR_EL1` on the boot core and initializes Linux CPU mapping. That mapping is required for per-CPU data, IRQ routing, and SMP bring-up.”
