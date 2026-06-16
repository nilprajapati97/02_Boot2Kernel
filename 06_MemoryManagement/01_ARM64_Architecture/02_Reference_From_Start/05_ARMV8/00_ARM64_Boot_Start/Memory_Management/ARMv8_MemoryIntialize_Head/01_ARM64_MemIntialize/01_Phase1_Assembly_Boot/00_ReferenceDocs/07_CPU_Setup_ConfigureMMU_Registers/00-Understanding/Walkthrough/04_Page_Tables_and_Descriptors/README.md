# 04 Page Tables And Descriptors

Page tables are tree-shaped data structures that map ranges of virtual addresses to physical addresses and associated attributes.

## What A Leaf Descriptor Carries

A stage-1 leaf descriptor does not directly embed a full memory-type policy. Instead, it carries fields such as:

- output address bits
- access permissions
- shareability information
- access flag state
- execute-never bits
- an `AttrIndx` value

That `AttrIndx` is only meaningful because `MAIR_EL1` maps index values to concrete memory attribute encodings.

## Why Control Registers And Descriptors Must Agree

The translation tables and the control registers form a contract.

Examples:

- the chosen granule in `TCR_EL1` determines table shape and walk rules
- the chosen address sizes determine how many levels exist and how virtual addresses are split into indices
- `MAIR_EL1` determines what a descriptor's memory-type index actually means

If the tables and the control registers disagree, translation may fault, behave unpredictably, or silently describe the wrong caching behavior.

## Linux Early Boot Angle

Linux has already created the tables it needs for the idmap and early kernel mapping. `__cpu_setup` is the step that makes the architectural interpretation of those tables correct for the current CPU.