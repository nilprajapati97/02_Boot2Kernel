Treat this as your interview-grade mental model for the line early_boot_irqs_disabled = true, not just code trivia.

**Core idea**
This flag is a software phase marker:
The kernel is declaring, “we are in the special early-boot regime where interrupts are expected to be disabled, and some boot-only rules are allowed.”

It does not disable interrupts by itself.
Hardware masking is done separately by local_irq_disable.
This flag tells the rest of the kernel how to interpret behavior during that phase.

**If interviewer asks in one line**
This boolean is a boot-mode contract bit that gates debug checks and boot-only fast paths until the kernel is ready to run with normal interrupt and concurrency semantics.

**State machine you should speak out loud**
1. Enter early boot critical phase.
2. Disable local hardware IRQ delivery.
3. Set early_boot_irqs_disabled = true to announce boot-only invariants.
4. Run fragile initialization that assumes no asynchronous interrupt interleaving.
5. Complete IRQ/timekeeping/core setup.
6. Set early_boot_irqs_disabled = false to end boot-only exceptions.
7. Re-enable interrupts and switch to normal runtime rules.

That ordering is deliberate. Hardware first, then software flag. On exit, software flag first, then hardware unmask.

**Why this exists at system-design level**
1. Early boot has different invariants than runtime.
2. Many validators and synchronization assumptions are runtime-oriented.
3. Without a phase marker, runtime checks would either:
1. falsely panic during valid boot behavior, or
2. be globally weakened and miss real bugs later.
4. This flag isolates exceptional policy to a bounded window.

This is a textbook reliability pattern: explicit phase transition plus strict handoff.

**What this flag enables safely**
1. Lock/debug subsystems can treat early boot as a known special case.
2. Architecture-specific patching and low-level memory setup can use early-safe paths.
3. Code can assert “this must only happen before full IRQ/runtime model is live.”

**What this flag does not do**
1. It does not mask CPU interrupts.
2. It does not serialize memory globally.
3. It does not protect shared data across CPUs by itself.
4. It is not a lock.
5. It is not a memory barrier.

Interview trap: people confuse policy flag with hardware state.

**Scenarios interviewers love**

1. Normal boot scenario
The flag is true while the kernel builds core subsystems under IRQ-off assumptions, then cleared right before normal IRQ-on runtime. Everything deterministic.

2. Bug: interrupts turned on too early
If some code accidentally enables IRQs in the early window, you want warnings and correction paths to fire. The flag helps detect policy violation relative to phase.

3. Debugging/lock validation scenario
Runtime lock rules may reject things that are acceptable only during one-CPU early boot. The flag prevents false positives there, without weakening runtime lock checking forever.

4. Early text patching scenario
Some architectures patch instructions differently in boot vs runtime. During early boot, simpler synchronization can be valid because no real parallel execution yet. Flag selects that mode.

5. Low-level memory map surgery scenario
Certain page-table/page-structure transformations are only safe when nobody can race with them. The flag is used as a “must still be in early regime” sanity check.

6. Virtualization/paravirt boot scenario
On some boot paths under hypervisors, code relies on interrupts being initially off and uses this same phase marker to align early assumptions before normal bring-up.

7. SMP bring-up scenario
Before secondary CPUs and full scheduling/interrupt activity, assumptions are tighter and simpler. After flag goes false, system must obey full multicore concurrency model.

8. Hardening/reliability scenario
This flag creates a narrow trusted window. Security and correctness improve when exceptional behavior is tightly scoped and auditable.

**Failure modes if mishandled**
1. Left true too long:
Runtime could accidentally keep using boot exceptions, hiding real locking/IRQ bugs.
2. Cleared too early:
Valid early operations may trigger false alarms or take unsafe runtime paths before system is ready.
3. Set true without actual IRQ-off discipline:
Software believes it is in protected phase while hardware can still interrupt, causing race-driven corruption.
4. Used as synchronization primitive:
Leads to subtle SMP data races because flag is policy, not concurrency control.

**How to answer “why both local_irq_disable and this flag?”**
1. local_irq_disable controls hardware behavior on this CPU.
2. early_boot_irqs_disabled controls kernel policy interpretation globally for early-boot code paths.
3. One is mechanism, one is mode declaration.

Mechanism without declaration causes ambiguous behavior. Declaration without mechanism is unsafe.

**Senior-level interview phrasing**
This is an explicit phase-boundary variable implementing a two-regime boot model:
Regime A: deterministic IRQ-off bootstrap with relaxed but controlled assumptions.
Regime B: full runtime concurrency and strict lock/IRQ semantics.
The flag is the handoff contract between A and B.

**What interviewers evaluate from your answer**
1. Can you separate hardware state from software invariants?
2. Can you explain phase transitions and ordering requirements?
3. Do you understand why temporary exceptions must be bounded?
4. Can you reason about failure when state machine order is wrong?
5. Can you connect low-level kernel details to production reliability outcomes?

**30-second polished answer**
early_boot_irqs_disabled is not interrupt control; it is an early-boot phase marker. The kernel sets it after masking interrupts to declare a special single-CPU bootstrap regime, where some operations and checks follow boot-only rules. It is cleared before re-enabling interrupts so runtime semantics become strict again. This explicit state machine prevents both false debug failures during boot and silent correctness regressions after boot.
