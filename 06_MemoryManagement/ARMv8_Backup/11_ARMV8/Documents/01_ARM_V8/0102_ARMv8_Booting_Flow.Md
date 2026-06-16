```mermaid
sequenceDiagram
    participant CPU0 as Primary Core (CPU0)
    participant ROM as BootROM
    participant BL12 as BL1/BL2
    participant BL31 as EL3 Firmware (PSCI)
    participant BL as Bootloader (U-Boot)
    participant K as Linux Kernel
    participant CPUX as Secondary Cores (CPU1..N)
    participant US as User Space (init/systemd)

    CPU0->>ROM: Reset Vector
    ROM->>BL12: Load BL1
    BL12->>BL12: Init minimal HW (SRAM, stack)
    BL12->>BL31: Load BL31 (EL3 runtime FW)
    BL31->>BL31: Init PSCI, keep secondary cores OFF

    BL31->>BL: Jump to Bootloader (EL2/EL1)
    BL->>BL: Load Kernel + DTB + Initrd
    BL->>K: Jump to start_kernel()

    K->>K: setup_arch()
    K->>K: smp_init()

    K->>BL31: SMC: psci_cpu_on(cpu, entry)
    Note right of BL31: Runs in EL3\nHandles power control

    BL31->>CPUX: Power ON secondary core
    BL31->>CPUX: Set entry = secondary_entry
    BL31->>CPUX: SEV (wake event)

    CPUX->>K: secondary_entry (assembly)
    CPUX->>K: secondary_start_kernel()
    CPUX->>K: cpu_startup_entry()
    CPUX-->>K: CPU online

    K->>K: rest_init()
    K->>K: kernel_init()
    K->>US: run_init_process("/sbin/init")

    US->>US: init (systemd) starts
    US->>US: Launch services
    US->>US: Start application 🎯
