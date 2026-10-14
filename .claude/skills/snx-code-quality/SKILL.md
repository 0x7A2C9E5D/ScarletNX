---
name: snx-code-quality
description: "RFC #142's C rules: no redundant state, indices not pointer chains, one allocation per owner, atomics apart, no stored pointers. Use when writing or reviewing C in src/."
---

# Code quality (#142)

1. **No state another value already says.** A NULL pointer is absent: no `is_present` beside it.
   Not when NULL means something itself, or the flag says something else.
2. **Indices, not pointer chains.** Objects in one array, linked by `u32` index; 0 is none, so slot 0
   is never an object. Not when the index can outlive its array.
3. **One allocation per owner.** An owner's arrays come from one block carved by aligned offsets, sizes
   checked with `ckd_mul`/`ckd_add`, one `free`. Not for an array that grows on its own, or parts that
   live apart.
4. **Atomics apart.** Atomics that different threads write get a cache line each (`alignas(64)`).
   Not when one thread writes them all, or for many per-object atomics.
5. **No stored pointers in data.** Data refers to its parts by index or offset, so a copy or a file
   holds together. Handles (`FILE*`, SDL, Vulkan) stay in a runtime struct beside it.

Review: `python3 .claude/skills/snx-code-quality/check.py FILES` lists candidates for 1, 3 and 4 (a
candidate is not a finding until the code is read); read the diff for 2 and 5. Report `file:line`, the
rule and the fix. A rule that does not apply is not a finding. Wrong and right C for each rule:
[examples.md](examples.md).
