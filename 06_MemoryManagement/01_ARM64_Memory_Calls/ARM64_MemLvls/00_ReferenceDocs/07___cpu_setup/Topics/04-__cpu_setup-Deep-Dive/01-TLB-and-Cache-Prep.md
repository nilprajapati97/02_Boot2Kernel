# TLB and Early Control Prep

The first part of `__cpu_setup` clears stale translation state and resets selected privileged-access settings.

## Step 1: `tlbi vmalle1`

This invalidates local EL1 stage-1 TLB entries.

Why:
- old translations may no longer match the new translation policy that Linux is about to install
- early boot wants deterministic local translation-cache state

## Step 2: `dsb nsh`

This barrier ensures the TLB invalidation is complete before later steps depend on it.

## Step 3: reset control and debug-related access state

The function resets `CPACR_EL1` and loads `MDSCR_EL1` with `MDSCR_EL1_TDCC`.

Meaning:
- begin from a known access-control state
- disable certain debug channel access from EL0

## Step 4: disable PMU and AMU access from EL0 if present

The helper macros check whether those hardware blocks exist and then disable user access.

This is part policy and part hygiene:
- early kernel boot wants controlled privilege boundaries
- Linux should not inherit unpredictable user-facing hardware-access state at this stage
