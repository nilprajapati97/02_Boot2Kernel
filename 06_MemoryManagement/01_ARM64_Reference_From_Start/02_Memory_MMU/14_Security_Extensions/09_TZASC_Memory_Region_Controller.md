# TZASC and TrustZone Memory Region Control Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), TrustZone-enabled SoCs

---

## 1. Concept Foundation

TZASC (TrustZone Address Space Controller) enforces secure versus non-secure access policy on memory transactions.

Problem solved:
- CPU privilege levels alone are not enough if bus masters (GPU, DMA, accelerators) can issue memory transactions directly.
- Need a fabric-level gatekeeper that labels and filters accesses based on security state.

TZASC role:
- Defines DRAM regions with security attributes.
- Allows secure-only, non-secure-only, or filtered access policies.
- Blocks illegal accesses before they reach memory controller.

Why interviewers care:
- Demonstrates understanding of system-wide security, not only CPU MMU settings.

---

## 2. ARM64 Hardware Detail

### 2.1 Region model

Typical TZASC implementations (for example TZC-400 family) provide:
- Base region attributes plus multiple programmable regions.
- Region size as power-of-two aligned windows.
- Access permission bits for secure and non-secure masters.

Conceptual region fields:
- REGION_BASE_LOW/HIGH
- REGION_TOP_LOW/HIGH or size encoding
- REGION_ATTR
- REGION_ID_ACCESS or filter masks

Common policy forms:
- Secure read/write only
- Non-secure read/write allowed
- Mixed filtering by bus master ID

### 2.2 Security tagging on interconnect

Transactions carry security metadata from source master:
- Secure transaction
- Non-secure transaction

TZASC compares:
1. Address hits region
2. Security attribute of request
3. Master filter policy

If denied:
- Transaction aborted
- Error response returned to master
- Optional interrupt/status update for fault logging

### 2.3 Interaction with DDR controller and SMMU

SMMU translates IOVA to PA but does not replace TZASC policy.
- SMMU decides address translation and stage permissions.
- TZASC still enforces secure-domain policy at memory boundary.

Defense in depth:
- MMU permissions
- SMMU/IOMMU permissions
- TZASC memory-domain gate

---

## 3. Linux and Firmware Implementation

### 3.1 Configuration ownership

Most TZASC programming happens in secure firmware:
- BL2 or BL31 configures protected regions before non-secure boot.
- Normal-world Linux usually cannot modify secure controller registers.

Typical secure allocations protected by TZASC:
- BL31 runtime region
- OP-TEE core and secure heap
- Secure key storage buffers

### 3.2 Device tree and reserved memory implications

Linux sees related effects via reserved memory nodes:
- secure-memory regions marked no-map
- OP-TEE shared memory explicitly carved out and controlled

Normal-world behavior:
- Accesses to secure-only DRAM ranges fault or bus-error.
- Drivers must only DMA into approved non-secure memory pools.

### 3.3 Fault handling and diagnostics

Platform-specific secure logs may report:
- violating master ID
- offending physical address
- region index and policy bits

Linux side may only observe:
- SError
- external abort
- device DMA timeout if access blocked

---

## 4. Hardware-Software Interaction

Example secure boot memory protection flow:
1. TF-A configures TZASC regions.
2. Marks secure firmware and TEE DRAM as secure-only.
3. Boots non-secure BL33 and Linux.
4. Linux page tables may still map full DRAM ranges conceptually, but secure-only ranges are blocked by fabric policy.
5. Any non-secure CPU or DMA access into secure-only range is denied at TZASC.

DMA example:
1. Driver submits DMA buffer.
2. SMMU translates to PA.
3. PA lands inside secure-only TZASC region.
4. TZASC rejects transaction even if SMMU mapping exists.

---

## 5. Interview Q and A

Q1: Can Linux bypass TZASC with kernel privilege?
No. TZASC enforcement is below EL1 software at interconnect/memory-controller boundary.

Q2: If SMMU allows access, is that sufficient?
No. SMMU and TZASC enforce different dimensions. Access must pass both translation permissions and security-domain policy.

Q3: Why configure TZASC in TF-A instead of Linux?
Because secure policy must be established before normal world starts, and secure registers are often inaccessible to non-secure EL1.

Q4: What symptom appears when a driver DMA targets secure memory?
Typically DMA failure, external abort, or device timeout depending on platform error routing.

Q5: Does TZASC replace TrustZone NS bit in page tables?
No. NS-bit semantics and page-table controls define CPU translation/security context; TZASC enforces memory-region policy at fabric level.

Q6: How do you debug a suspected TZASC deny?
Correlate secure firmware logs, platform abort registers, and Linux crash traces with physical address and DMA master identity.

---

## 6. Pitfalls and Gotchas

- Misaligned or overlapping TZASC regions can accidentally expose secure memory.
- Forgetting to reserve secure ranges in Linux device tree leads to driver misuse.
- Assuming CPU access behavior equals DMA behavior is wrong; bus masters follow their own path.
- Updating firmware without synchronized memory map updates can break boot or TEE operation.
- Debug visibility is asymmetric; normal world may not see full TZASC fault details.

---

## 7. Quick Reference Table

| Layer | Responsibility |
|---|---|
| MMU (EL1/EL2) | Virtual to physical translation and permission checks |
| SMMU | Device DMA translation and access control |
| TZASC | Secure versus non-secure region enforcement at memory boundary |

| Typical protected regions | Owner |
|---|---|
| BL31 runtime memory | EL3 firmware |
| OP-TEE core and secure heap | Secure world |
| Non-secure Linux DRAM | Normal world |
