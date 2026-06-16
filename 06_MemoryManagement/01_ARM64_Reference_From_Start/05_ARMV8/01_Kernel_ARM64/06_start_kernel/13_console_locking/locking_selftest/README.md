# `locking_selftest()` — Locking Primitive Validation

## Purpose

Runs a set of self-tests to verify that locking primitives (spinlocks, mutexes, rwlocks, rwsems) work correctly and that lockdep properly detects violations when compiled in.

## Source File

`lib/locking-selftest.c`

```c
void __init locking_selftest(void)
{
    // Reset the failure counter:
    failures = 0;
    
    printk("------------------------\n");
    printk("| Locking API testsuite |\n");
    printk("------------------------\n");
    
    // Test spinlock scenarios:
    dotest(irqsafe1, SUCCESS, LOCKTYPE_SPIN);
    dotest(irqsafe2A, SUCCESS, LOCKTYPE_SPIN);
    dotest(irqsafe2B, SUCCESS, LOCKTYPE_SPIN);
    dotest(irqsafe3, SUCCESS, LOCKTYPE_SPIN);
    dotest(irqsafe4, SUCCESS, LOCKTYPE_SPIN);
    dotest(irqsafe5, FAILURE, LOCKTYPE_SPIN);  // Should be detected!
    
    // Test rwlock scenarios:
    dotest(irqsafe1, SUCCESS, LOCKTYPE_RWLOCK);
    // ...
    
    // Test mutex scenarios:
    dotest(irqsafe1, SUCCESS, LOCKTYPE_MUTEX);
    // ...
    
    if (failures)
        printk("failures: %d\n", failures);
    else
        printk("  passed\n");
}
```

## What the Tests Cover

### IRQ Safety Tests

```
irqsafe1: Take lock, enable IRQ → should be OK (lock not IRQ-unsafe)
irqsafe2: Take IRQSAFE lock in IRQ context → should be OK
irqsafe3: Take IRQSAFE lock while IRQSAFE-incompatible held → should FAIL
irqsafe4: Correct ordering of IRQSAFE locks → should pass
irqsafe5: Reverse IRQ-safe ordering → should be detected by lockdep
```

### Hardirq/Softirq Context Tests

```
hardirq_test: Lock taken in hardirq must be safe to take everywhere
softirq_test: Lock taken in softirq is OK to take in process context,
              but not OK to take in hardirq context without IRQSAFE
```

### AA Deadlock Tests

```
AA: Try to take same lock twice (deadlock) → should be detected
```

### ABBA Tests

```
ABBA: CPU0: lock(A), lock(B)
      CPU1: lock(B), lock(A)
      → lockdep should detect circular dependency
```

## Test Outcome

If all tests pass:
```
------------------------
| Locking API testsuite |
------------------------
  passed
```

If a test that should fail passes (lockdep missed it):
```
FAILED: test_expected_failure
failures: 1
```

This would indicate a regression in lockdep itself.

## Only Meaningful with Lockdep

Without `CONFIG_PROVE_LOCKING`, most tests trivially "pass" because no validation occurs. The selftest is really a test of lockdep's detection capability.

## Cross-references

- [Phase overview](../README.md)
- `lockdep_init()`: [../lockdep_init/README.md](../lockdep_init/README.md)
