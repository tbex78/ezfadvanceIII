# EZF Advance III 0.8.0 — Team Review Brief

**Repository:** `tbex78/ezfadvanceIII`  
**Review date:** 2026-08-26  
**Previous fully approved runtime checkpoint:** `d7d987f986351495f4ced3b84341f4792be3fafe` (`0.7.29`)  
**Current HEAD reviewed:** `902242124746d34de7b2e50dc521bae8b0139553` (`0.8.0`)

---

## Executive verdict

**Do not mark 0.8.0 as the new fully approved runtime checkpoint yet.**

The factorization work is broadly good and should be kept.

There is one blocking evidence-policy regression in the multi-ROM save-selection change:

```text
1211be4d  fix: require explicit multi-ROM save selection
```

The current implementation allows `--rom N` to identify a selected catalog entry, but no corresponding hardware save-slot switch is performed before the capture-proven `0x0900` save read.

Therefore `--rom N` currently acts as an application-level label, not as a proven hardware save selector.

Until this is corrected, keep:

```text
0.7.29 / d7d987f9
```

as the last fully approved runtime checkpoint.

---

## Commit review

Current 0.8.0 chain:

```text
e0ba9544  refactor: share EZ3 catalog parsing
1211be4d  fix: require explicit multi-ROM save selection
2bbc8a43  refactor: extract cartridge image builder
90224212  refactor: extract card writer workflow for 0.8.0
```

### Approval matrix

```text
e0ba9544  shared EZ3 catalog parser          APPROVE
1211be4d  multi-ROM save selection           FIX REQUIRED
2bbc8a43  CartridgeImageBuilder extraction   APPROVE
90224212  CardWriter workflow extraction     APPROVE structurally
```

---

# 1. Shared EZ3 catalog parser

## Status

**APPROVED**

The new shared catalog parser is the correct factorization boundary.

It centralizes structural interpretation of:

```text
single-ROM catalog layout
multi-ROM catalog layout
entry parsing
entry plausibility
structural slot count
allocation boundaries
```

while allowing each tool to retain its own evidence policy.

That separation must remain:

```text
shared format interpretation
        !=
tool-specific support authorization
```

Examples:

```text
Card reader
    may structurally parse beyond currently hardware-proven active counts
    must report the proof boundary accurately

Save reader
    remains restricted to its narrower capture-proven save-reading scope
```

No tool-specific evidence policy should move into the shared parser.

---

# 2. CartridgeImageBuilder extraction

## Status

**APPROVED**

The writer image-construction refactor is a good architectural change.

The extracted component now owns pure image-format behavior such as:

```text
ROM ordering
ROM placement
catalog construction
loader assets
loader placement
loader relocation
ROM-1 entry patch
constructed/programmed image extent
```

The component does **not** own:

```text
write authorization
USB device handling
erase commands
program commands
verification selection
destructive session flow
```

This is the correct boundary.

## Regression coverage

The new builder tests include whole-image hash fixtures for representative layouts.

That is a strong regression mechanism.

Recommended later improvement:

Add a small set of durable golden fixtures based on previously validated real ROM layouts or pre-refactor dry-run output, not only synthetic byte-pattern ROMs.

This is **not a blocker** for 0.8.0.

---

# 3. CardWriter workflow extraction

## Status

**STRUCTURALLY APPROVED**

The new `CardWriter` coordinator preserves the high-level destructive workflow:

```text
preflight
    ↓
bridge initialization
    ↓
global write preparation
    ↓
window-0 erase selection
    ↓
erase
    ↓
flash-state finalization
    ↓
window-0 program selection
    ↓
program
    ↓
evidence-bounded verification dispatch
```

The refactor does **not** collapse the individual verification modes into one generic selector-driven routine.

That is important.

The backend still exposes separately named evidence-backed methods such as:

```text
verifyPartialFirstWindow
verifyExact8MiB
verifyPartial12MiB
verifyExact16MiB
verifyTinyTailAbove16MiB
verifyPartial20MiB
verifyExact24MiB
verifyPartial28MiB
verifyExact32MiB
```

Unsupported higher-partial geometries remain verification-skipped after conservative cleanup.

No experimental higher-window mapping is introduced.

## Recommended additional tests

Add workflow-dispatch coverage for every verification mode, not only representative modes.

Recommended matrix:

```text
<8 MiB partial first window
exact 8 MiB
exact 12 MiB path
exact 16 MiB
tiny tail above 16 MiB
exact 20 MiB path
exact 24 MiB
exact 28 MiB path
exact 32 MiB
unsupported higher partial
--skip-verify
preflight failure
erase failure
program failure
verification failure
```

This is desirable but not the current blocker.

## Hardware qualification recommendation

Because this refactor moved destructive workflow orchestration, perform at least one real-hardware write/verify regression before calling 0.8.0 fully qualified.

Prefer a small already-proven case first.

For example:

```text
known <8 MiB or 2-MiB source ROM
→ build
→ write
→ full read-back verify
→ real GBA boot
```

A second higher-window case would give stronger confidence.

---

# 4. Blocking issue — multi-ROM save selection

## Status

**FIX REQUIRED**

The current save-reader behavior requires:

```sh
--rom N
```

for multi-ROM layouts.

That is a good user-interface rule by itself.

The problem is that the selected ROM number does not correspond to any proven hardware save-slot switching command.

Current effective flow is:

```text
user chooses --rom N
        ↓
software selects catalog entry N
        ↓
software checks ROM N for SRAM_V111
        ↓
software performs the same 0x0900 save read
```

No additional hardware command is sent to select save slot N.

The project evidence currently says that the known multi-ROM save capture contains **no additional ROM/save-slot selection command before the `0x0900` read**.

Therefore:

```text
--rom N
```

cannot currently be treated as proof that the returned bytes belong to ROM N.

---

## Why this matters

Consider:

```text
ROM 1  SRAM_V111
ROM 2  SRAM_V111
```

The user could request either:

```sh
--rom 1
```

or:

```sh
--rom 2
```

but both requests would ultimately issue the same hardware save read.

The application could then label identical hardware behavior as two different logical saves without evidence that the hardware actually changed save source.

That violates the project's evidence-bounded rule.

---

# 5. Required fix

For multi-ROM save extraction, restore a uniqueness requirement.

Recommended behavior:

```text
scan catalog ROMs
        ↓
find capture-proven SRAM_V111 candidates
        ↓
exactly one candidate?
        ├─ NO  → refuse
        └─ YES
             ↓
       require --rom N if desired
             ↓
       N must equal unique candidate
             ↓
       perform unchanged 0x0900 read
```

Conceptually:

```cpp
if (multi_rom) {
    find all capture_proven_32k candidates;

    if (candidates.size() != 1)
        refuse("No proven hardware save-slot switching mechanism.");

    if (!requested_rom)
        refuse("--rom N required.");

    if (*requested_rom - 1 != candidates.front())
        refuse("Selected ROM is not the uniquely supported save-bearing entry.");
}
```

This keeps explicit user selection while preserving the evidence boundary.

---

# 6. Required negative tests

Add tests covering at least:

```text
single ROM + supported SRAM_V111
    → auto-select ROM 1

multi ROM + no --rom
    → refuse

multi ROM + exactly one SRAM_V111 + matching --rom
    → allow

multi ROM + exactly one SRAM_V111 + non-matching --rom
    → refuse

multi ROM + zero SRAM_V111 candidates
    → refuse

multi ROM + two SRAM_V111 candidates
    → refuse

multi ROM + unsupported save signature
    → refuse

out-of-range --rom
    → refuse
```

Most importantly:

> The tests must encode that `--rom N` does not authorize a hardware save-source switch that has not been captured.

---

# 7. Save-reader scan-boundary check

Also add a regression ensuring save-signature scanning cannot accidentally cross beyond the save reader's currently proven read boundary.

The shared catalog parser may use full cartridge size-class geometry for structural interpretation.

That is acceptable.

The save reader's actual scanning/reading policy must remain separately bounded by its own capture evidence.

Maintain this distinction:

```text
catalog size-class geometry
        !=
save-reader authorized read range
```

---

# 8. Protocol boundaries that must remain unchanged

Do not use the 0.8.0 cleanup as an opportunity to generalize protocol behavior.

Keep these rules:

```text
No arbitrary higher-partial verification mappings.
No inferred EEPROM map-4/map-5 discriminator.
No invented save-slot switch.
No merged wipe/write protocol state machine without direct evidence.
No interpolation of selectors or delays.
```

Existing explicit verification paths should remain separately named and reviewable.

---

# 9. CI status

The current 0.8.0 HEAD CI run passes.

This confirms the code builds and the offline test suite succeeds.

It does **not** resolve the save-reader semantic evidence issue described above.

The blocker is not a compile/test failure.

It is an evidence-policy correctness problem.

---

# 10. Re-review gate for 0.8.0

Before approving 0.8.0 as the new runtime checkpoint:

```text
[ ] Fix multi-ROM save selection semantics.
[ ] Restore unique supported save-bearing-ROM requirement.
[ ] Add negative tests for multiple/no/mismatched candidates.
[ ] Add scan-boundary regression for save-reader evidence limits.
[ ] Keep shared parser policy-neutral.
[ ] Keep verification paths explicit.
[ ] Run make.
[ ] Run make check.
[ ] Run make test.
[ ] Run git diff --check.
[ ] Confirm CI passes.
[ ] Perform at least one real-hardware writer regression after CardWriter refactor.
```

If these pass, re-review 0.8.0 rather than reverting the factorization.

---

# Team directive

**Keep the factorization. Fix the save-reader evidence policy.**

Do not revert:

```text
Ez3CatalogParser
CartridgeImageBuilder
CardWriter
```

They are useful architectural improvements.

Correct only the multi-ROM save-selection logic so that explicit `--rom N` cannot imply a hardware save-slot switch that has never been proven.

Until then:

```text
LAST FULLY APPROVED RUNTIME CHECKPOINT
0.7.29
d7d987f986351495f4ced3b84341f4792be3fafe
```

Current 0.8.0 status:

```text
architecture/factorization   GOOD
offline CI                   PASS
protocol generalization      NONE FOUND
writer orchestration         STRUCTURALLY SOUND
save-selection evidence      BLOCKING FIX REQUIRED

0.8.0 APPROVAL STATUS        HOLD
```
