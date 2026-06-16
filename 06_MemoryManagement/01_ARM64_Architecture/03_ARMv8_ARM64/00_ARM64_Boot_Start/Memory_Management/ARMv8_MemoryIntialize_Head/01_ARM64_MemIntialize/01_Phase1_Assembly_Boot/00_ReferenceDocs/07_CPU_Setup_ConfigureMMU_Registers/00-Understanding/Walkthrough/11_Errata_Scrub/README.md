# 11 Errata Scrub

After the initial `TCR_EL1` value is assembled, Linux calls `tcr_clear_errata_bits`.

## Why This Exists

Architecturally valid does not always mean safe on real shipping silicon. A core may implement a legal configuration that still triggers a known microarchitectural bug.

The Linux boot code therefore treats translation-control state as both:

- an architectural description of the regime
- a hardware-compatibility surface that may need CPU-specific cleanup

## What The Helper Does

The helper reads CPU identification state, checks for affected implementations, and clears the problematic `TCR_EL1` bits when necessary.

## Important Insight

This is one of the clearest examples of Linux policy layered on top of ARM architecture. The architecture says what is allowed. Linux narrows that to what is both allowed and safe on the actual CPU executing the code.

## Failure Mode If Ignored

You can hit hangs or incorrect behavior that are not caused by a logically wrong page-table design, but by a specific implementation quirk exposed by a certain control-bit combination.