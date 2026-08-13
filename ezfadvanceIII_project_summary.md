# EZF Advance III Reverse-Engineering Project — Technical Summary

## Project goal

This project reconstructs the behavior of the original Windows **EZ3Manager** software for the **EZ-Flash Advance III** GBA flash cartridge, with the goal of reproducing its cartridge image-building rules and USB flash-management protocol on Unix-like systems using C++17 and libusb. The native project scope is macOS, Linux, and BSD; Windows users are expected to use a Linux VM with USB passthrough.

The project is intentionally **evidence-driven**:

- USBPcap captures from original EZ3Manager are treated as protocol fixtures.
- Real GBA console behavior is considered more authoritative than USB success alone.
- Generic structural rules are preferred over title-specific hacks.
- Unproven read/write mappings are not guessed.
- The writer never silently patches ROM save routines.

Current writer version covered by this summary: **0.5.12**.

---

## Hardware target

USB interface:

```text
VID:       0x0E6A
PID:       0x5088
Interface: 0
OUT EP:    0x02
IN EP:     0x81
```

Tested cartridge geometry:

```text
Physical capacity: 32 MiB = 256 Mbit
Program block:     0x10000 = 64 KiB
Flash window:      0x800000 = 8 MiB
Windows:           4
```

Physical windows:

```text
0x0000000–0x07FFFFF  window 0
0x0800000–0x0FFFFFF  window 1
0x1000000–0x17FFFFF  window 2
0x1800000–0x1FFFFFF  window 3
```

Window setup values recovered from captures:

```text
window 0: default/base
window 1: 0x0040
window 2: 0x0080
window 3: 0x00C0
```

---

## Bridge startup and readiness

The original-manager startup sequence includes:

```text
0x97 -> 00
0x98 -> 01
0x99 with parameter 01 -> 13-byte echo
```

`0x98 -> 01` is used as the cartridge readiness gate.

The project deliberately replays the full startup/probe state because early experiments that omitted parts of the initialization could erase/read successfully but failed to transfer complete 64-KiB program blocks.

---

## Main USB operations

The core command families recovered so far are:

```text
0x91  ROM/readback
0x92  flash command / program transaction
0x96  erase sector
0x97  startup
0x98  readiness
0x99  bridge initialization/large-transfer state
```

Programming is performed in **64-KiB blocks**.

A program transaction uses a `0x92` command containing:

- local word address;
- transfer length;
- program marker;

followed by the ROM data payload and a completion response.

Program addresses restart from local zero when switching to a new 8-MiB flash window.

---

## Flash-window state machine

Program-window selection and readback mapping are **not the same state machine**.

This was one of the most important discoveries in the project.

An earlier experimental verifier assumed that selecting a higher flash window for programming would also make local `0x91` reads address that same physical window. Hardware disproved this: reads still returned lower-window data.

As a result, current verification behavior is deliberately conservative.

---

## Verification policy

Capture-supported full readback verification exists for:

```text
<= 8 MiB
exact 16 MiB
exact 24 MiB
exact 32 MiB
Fire-Emblem-style tiny tail immediately above 16 MiB
```

Arbitrary other partial images in the 16–32 MiB region are programmed normally, but full verification is skipped unless a capture-proven read mapping exists.

This avoids false failures caused by invented window-selection rules.

The user can explicitly disable post-write readback with:

```bash
--skip-verify
```

This skips ROM comparison but still performs final non-readback status/reset cleanup.

---

## Cartridge image architecture

A generated card image contains:

1. one or more GBA ROM images;
2. an EZ3 loader/menu;
3. catalog entries embedded inside the loader;
4. a patched branch in physical ROM #1;
5. reused erased `0xFF` regions where possible.

The entire image is constructed **in memory**. No intermediate `.bin` file is written by the current writer.

Only physical/catalog ROM #1 has its first ARM instruction replaced with a branch to the EZ3 loader. Later ROMs retain their original first instruction.

---

## Loader templates

### Single-ROM loader

```text
Original base:       0x0000D3B0
Length:              0x660
Relocation entries:  13
Catalog entry:       offset 0x4F8
```

### Multi-ROM loader

```text
Original base:       0x0004A880
Nominal length:      0x7080
Relocation entries:  125
Header offset:       0x475E
Catalog offset:      0x476E
```

A 28-byte area at multi-loader offset `0x5764` was missing from USBPcap payloads because captures truncated 64-KiB transfers. Those bytes were recovered from a cartridge written by original EZ3Manager and are restored explicitly.

Loader relocation scans aligned 32-bit words that point into the original loader address range and adds:

```text
new_loader_start - original_loader_base
```

The expected relocation counts are checked so template corruption or incorrect relocation assumptions fail loudly.

---

## Multi-ROM loader variants

The underlying multi-ROM template remains the same across every captured count from 2 through 8 entries. It is the same `0x7080` loader with 125 relocation sites; only small serialization details vary by count.

### Two ROMs

```text
loader extent: 0x6F80
```

The tail remains erased and the unused third catalog slot uses a specific sentinel representation.

### Three ROMs

Uses the full `0x7080`; the final 26 bytes remain `FF`.

### Four or more ROMs

Independent 4-, 5-, 6-, 7-, and 8-ROM captures use the full `0x7080` loader and:

```text
offsets 0x7066..0x707F = 00
```

No new loader blob is needed. The same `kMultiTemplateB64`, 125 relocations, and 28-byte catalog-entry format continue to match the captures.

The repeated catalog region from `0x476E` to `0x548E` contains **120 structural slots**. This is a safety bound, not a proven 120-ROM menu limit.

Current 0.5.12 policy therefore has **no small fixed ROM-count limit**. It requires:

```text
total input ROM bytes <= 32 MiB / 256 Mbit
packed image + loader <= 32 MiB
entry count <= 120 structural catalog slots
```

Original EZ3Manager captures currently prove up to **8 active entries**.

---

## Catalog entry format

Each entry is exactly **28 bytes**:

```text
Offset  Size  Meaning
0x00    16    display name
0x10     4    type field
0x14     4    packed ROM start + mapping flag
0x18     4    first ROM: original ARM entry target
               later ROMs: physical ROM start
```

### Type field

```text
type_field = entry_type << 24
```

The key discovery is that **type is a ROM size class**, not a save type.

Current rule:

| ROM size class | Type |
|---:|---:|
| 32 MiB | 0 |
| 16 MiB | 1 |
| 8 MiB | 2 |
| 4 MiB | 3 |
| 2 MiB | 4 |
| 1 MiB | 5 |
| 512 KiB | 6 |
| 256 KiB | 7 |
| 128 KiB | 8 |
| 64 KiB | 9 |

Conceptually:

```text
type = log2(32 MiB / ROM_size)
```

Non-power-of-two files are rounded up to the next power-of-two size class.

### Packed start / mapping field

```text
packed_start = ((rom_start / 2) << 8) | mapping_flag
```

Examples:

```text
start 0x0000000, map 6 -> 0x00000006
start 0x0800000, map 6 -> 0x40000006
start 0x1000000, map 3 -> 0x80000003
start 0x1800000, map 3 -> 0xC0000003
```

---

## EZ3 map/configuration and embedded save-library signatures

Catalog `map` is separate from ROM size. Current evidence links it to metadata classes observed in the ROM, but does not prove the ROM's active runtime save implementation:

```text
SRAM / SRAM_F / ordinary non-FLASH metadata -> map 3
EEPROM_V metadata                           -> map 5
FLASH-family metadata                       -> map 6
```

Capture examples:

```text
F-Zero             SRAM_V111      -> map 3
MegaManZ           SRAM_V112      -> map 3
Fire Emblem        SRAM_F_V102    -> map 3
Kingdom Hearts     SRAM_F_V103    -> map 3

Advance Wars       FLASH_V121     -> map 6
Mario Kart         FLASH_V124     -> map 6
Advance Wars 2     FLASH_V126     -> map 6
FFTA                FLASH512_V130  -> map 6

Tales of Phantasia EEPROM_V124     -> map 5
```

### Critical FFTA paired-control result

The project now has an unpatched FFTA control plus the exact SRAM-patched ROM pair.

Unpatched FFTA:

```text
ROM size:       16 MiB
FLASH512_V130:  offset 0x370820
EZ3 type/map:   1 / 6
loader:         0xD97730
```

Direct comparison with the SRAM-patched ROM shows only **94 changed bytes**, grouped into five localized regions from `0x1454D8` through `0x145B4D`. The patch redirects save reads/writes to SRAM at `0x0E000000`, bypasses FLASH-management code, and leaves `FLASH512_V130` unchanged at `0x370820`.

Earlier EZ3Manager evidence for the SRAM-patched FFTA also produced `type 1 / map 6` with loader `0xD97730`.

Therefore:

```text
embedded save-library signature != guaranteed active runtime save method
map 6                           != proof of active FLASH saving
SRAM patch state                != a reason to change this ROM to map 3
```

The current signature-associated map classifier should therefore remain metadata-faithful rather than attempting runtime save-code detection.

`FLASH1M_V... -> map 6` remains a generic metadata-based implementation rule that still deserves dedicated capture validation.

`EEPROM_V124 -> map 5` is directly capture-proven with unpatched Tales of Phantasia; universality across other EEPROM revisions, capacities, and separately SRAM-patched EEPROM ROMs remains open.

---

## Save-signature warnings

The writer does **not** patch or convert save routines.

It currently scans for embedded markers:

```text
FLASH_V...
FLASH512_V...
FLASH1M_V...
EEPROM_V...
```

and asks:

```text
Continue anyway? [y/N]:
```

Only explicit `y` / `yes` continues.

This warning is **conservative and signature-based**. An already SRAM-patched ROM may still trigger it if the old FLASH/EEPROM marker remains in the file. FFTA is now byte-level proof: 94 executable bytes are changed to redirect save handling to SRAM while `FLASH512_V130` remains untouched.

ROM save conversion, when desired, is a separate manual operation performed with a dedicated patching tool before writing. This matches the original Windows workflow.

---

## ROM ordering

Original EZ3Manager does not always preserve insertion order.

Capture-supported ordering rule:

> **Stable descending ROM file size**

Therefore:

- larger ROMs move before smaller ROMs;
- equal-size ROMs keep their original relative order.

Examples:

```text
Input:  Piano 64 KiB, MegaManZ 8 MiB
Final:  MegaManZ, Piano
```

```text
Input:  Advance Wars 8 MiB, MegaManZ 8 MiB, Fire Emblem 16 MiB
Final:  Fire Emblem, Advance Wars, MegaManZ
```

Four equal-size 8-MiB ROMs preserve their original order.

---

## Multi-ROM placement

After sorting, each ROM is aligned to its size class:

```text
alignment = next power-of-two >= max(64 KiB, ROM file size)
```

The allocator does **not** blindly advance by full file size. It tracks:

```text
meaningful_end = one byte after the final non-FF byte
```

Trailing `0xFF` is treated as reusable erased space.

A later ROM may overlap an earlier ROM file only where the earlier bytes are `FF`. Any overlap between meaningful non-FF data causes the build to abort.

This is how original EZ3Manager can pack nominally oversized input combinations into a smaller physical image.

---

## Loader placement

The loader is not simply appended after ROM data.

### Multi-ROM

The builder searches the packed image for the first sufficiently large 16-byte-aligned `FF` run.

If found, the loader is embedded there.

If not, it is appended at the next 16-byte boundary if capacity permits.

If neither is possible, the build fails rather than overwrite ROM data.

### Single-ROM

Single-ROM behavior is more specialized. Although the copied single loader is only `0x660` bytes, captures show that the manager can reserve a larger multi-loader-sized FF footprint while choosing a slot.

Important examples:

```text
Advance Wars loader:       0x5C2360
FFTA loader:               0xD97730
Tales of Phantasia loader: 0xD49020
```

Fire Emblem, a full 16-MiB ROM with no suitable internal gap, uses the capture-proven fallback just above 16 MiB:

```text
loader = 0x01000010
```

---

## Critical validated layouts

### MegaManZ + Piano

```text
MegaManZ @ 0x000000
Piano    @ 0x7F0000
loader   @ 0x2CC420
end      @ 0x800000
```

This proves compact packing using trailing and internal `FF` space.

### F-Zero + Mario Kart

```text
F-Zero       @ 0x000000  type 3 / map 3
Mario Kart   @ 0x400000  type 3 / map 6
loader       @ 0x5E9C30
end          @ 0x800000
```

This case proved that the 4-MiB geometry was correct and that the earlier launch failure came from wrong catalog metadata.

### Advance Wars 2 + Mario Kart + F-Zero

```text
Advance Wars 2 @ 0x0000000  type 2 / map 6
Mario Kart     @ 0x0800000  type 3 / map 6
F-Zero         @ 0x0C00000  type 3 / map 3
loader         @ 0x0617EF0
end            @ 0x1000000
```

This is a strong mixed-size validation of the current generic classifier and allocator.

### Fire Emblem + Advance Wars + MegaManZ

```text
Fire Emblem  @ 0x0000000
Advance Wars @ 0x1000000
MegaManZ     @ 0x1800000
loader       @ 0x15C2360
end          @ 0x2000000
```

### Four 8-MiB ROMs

```text
#1 @ 0x0000000
#2 @ 0x0800000
#3 @ 0x1000000
#4 @ 0x1800000
end @ 0x2000000
```

This is capture + hardware proven.

---

## Current validation matrix

| Geometry | Capture | Hardware |
|---|---|---|
| 64 KiB + 8 MiB | yes | yes |
| 4 MiB + 4 MiB | yes | yes |
| 8 MiB + 4 MiB + 4 MiB | yes | yes |
| 8 MiB + 8 MiB | historical/partial | yes |
| 16 MiB + 8 MiB + 8 MiB | yes | yes |
| 8 MiB + 8 MiB + 8 MiB + 8 MiB | yes | yes |
| single 8 MiB Advance Wars | yes | yes |
| single 16 MiB FFTA | yes | yes |
| single 16 MiB Fire Emblem | yes | yes |
| single 32 MiB Kingdom Hearts | yes | yes |
| single 16 MiB Tales / EEPROM_V124 | yes | save-cycle validation still useful |

A successful USB verify is **not** sufficient proof that menu/game launch behavior is correct.

---

## CLI behavior

### Dry run

```bash
ezfadvanceIII_multirom_writer rom1.gba rom2.gba
```

Builds the complete image in memory, prints layout/classification information, and exits without touching USB.

### Destructive write

```bash
ezfadvanceIII_multirom_writer \
  --yes-really-write \
  rom1.gba rom2.gba
```

Performs:

```text
startup/readiness
flash probe
erase
program
verify when supported
status/reset cleanup
```

### Write without readback

```bash
ezfadvanceIII_multirom_writer \
  --yes-really-write \
  --skip-verify \
  rom1.gba rom2.gba
```

### Manual catalog overrides

Supported for protocol experiments:

```text
--type1=N ... --type4=N
--map1=N  ... --map4=N
```

---

## Safety model

The writer intentionally keeps several conservative safeguards:

- destructive writing requires `--yes-really-write`;
- cartridge readiness must return the expected startup state;
- total image size must remain within 32 MiB;
- meaningful ROM data may not overlap;
- loader placement may not overwrite meaningful data;
- non-SRAM save families require explicit confirmation;
- unsupported readback geometries are not guessed;
- full-card wipe is optional and never run automatically.

A preventive full-card wipe has been observed to improve large-write reliability, but it is a separate destructive utility.

---

## Major lessons from failed experiments

### Program-window selection is not read-window selection

An early verifier generated false failures at higher flash windows because it assumed program-window state also controlled `0x91` readback.

That assumption was removed.

### ROM order and packing affect real GBA launch behavior

Piano-first + MegaManZ-second caused MegaManZ to straddle an 8-MiB hardware boundary and fail to launch, even though the loader/menu worked.

Original EZ3Manager reordered the games and compacted them into a valid 8-MiB image.

### Successful verification does not prove catalog correctness

The first custom F-Zero + Mario Kart image wrote and verified perfectly and displayed the chooser, but neither game launched.

The physical layout was correct. The catalog `type/map` metadata was wrong.

Correcting:

```text
F-Zero     -> type 3 / map 3
Mario Kart -> type 3 / map 6
```

fixed both games and led to the generic size-class plus signature-associated map classifier.

---

## Current open questions

### Higher menu counts

Original-manager captures now prove 1 through 8 active catalog entries. The loader contains 120 structural 28-byte slots, but menu/runtime behavior above 8 entries remains unproven.

### Other partial 16–32 MiB readback geometries

Exact 16-, 24-, and 32-MiB verification mappings are capture-proven, plus the tiny Fire-Emblem-style tail immediately above 16 MiB. Other arbitrary partial higher-window extents remain deliberately unproven.

### FLASH1M

`FLASH1M_V... -> map 6` is implemented generically but needs dedicated capture/save-cycle validation.

### EEPROM family coverage

`EEPROM_V124 -> map 5` is proven. Other EEPROM library versions and both EEPROM capacities still need additional evidence.

### Multi-ROM save-bank behavior

Catalog mapping is much better understood than the mechanism that associates multiple save regions with menu entries.

### Cartridge density probing

Four 8-MiB windows establish the tested 32-MiB geometry, but there is not yet a general density-identification algorithm for arbitrary EZ3 cartridge variants.

---

## Design principles

1. Prefer PCAP evidence over intuition.
2. Prefer real-console behavior over successful USB verification.
3. Avoid title-specific hacks when a structural rule exists.
4. Keep ROM-size classification separate from EZ3 map/configuration and from actual runtime save behavior.
5. Never silently patch ROM save routines.
6. Never invent a read-window mapping.
7. Reject meaningful-data overlap.
8. Keep destructive operations behind explicit user intent.
9. Preserve capture-derived timing unless controlled experiments disprove it.
10. Treat every recovered capture as a regression fixture.

---

## Current native platform scope

Version 0.5.12 keeps the writer on one C++17/libusb Unix-like codebase:

```text
macOS       target; 0.5.12 compilation verified on Apple Silicon/Homebrew
Linux       target; compile/hardware validation pending
FreeBSD     target; validation pending
OpenBSD     target; validation pending
NetBSD      target; validation pending
DragonFly   target; validation pending
Windows     no native mainline support; use a Linux VM with USB passthrough
```

The portability foundation remains the same; 0.5.11/0.5.12 add new capture-derived image/catalog/verification behavior without changing the platform policy.

## Current project status

At version **0.5.12**, the project has a mostly structural model of original EZ3Manager behavior:

- tested cartridge geometry is four 8-MiB windows / 32 MiB total;
- there is no small fixed ROM-count limit; 1–8 active entries are capture-proven and the loader exposes 120 structural catalog slots as a safety bound;
- total input ROM bytes may be up to and including 32 MiB / 256 Mbit, provided packing and loader placement still fit;
- ROM ordering is stable largest-first;
- placement is size-class based and reuses erased padding/internal `FF` runs;
- single and multi loaders are capture-derived and relocatable; the same `0x7080` multi-loader with 125 relocations matches captured 2–8 ROM configurations;
- catalog `type` is ROM-size based;
- catalog `map` uses capture-derived metadata associations (`3`, `5`, `6`) and is not treated as a definitive runtime save-mode field;
- exact 8-, 16-, 24-, and 32-MiB verification paths are known;
- unsafe verification guesses have been removed;
- paired FFTA evidence proves that non-SRAM save-library signatures may survive SRAM patching while runtime save code changes; the writer therefore keeps EZ3 metadata classification separate from runtime save-code analysis;
- native scope is macOS/Linux/BSD through libusb; Windows users should use a Linux VM with USB passthrough;
- multiple mixed-size and full-card layouts have been proven on original GBA hardware.

The remaining work is primarily **expanding evidence coverage**, not redesigning the core architecture.

## New multi-ROM evidence in 0.5.11 / 0.5.12

Recent original-manager PCAPs extend the proven menu/count model without requiring a new loader:

```text
5 ROMs: 8 + 4 + 4 + 4 + 4 MiB = 24 MiB
6 ROMs: 8 + 8 + 4 + 4 + 4 + 4 MiB = 32 MiB
7 ROMs: 8 + 4 + 4 + 4 + 4 + 4 + 4 MiB = 32 MiB
8 ROMs: 8 x 4 MiB = 32 MiB
```

Across these captures:

- catalog count fields encode 5, 6, 7, and 8 normally;
- the existing multi-loader template remains exact with 125 relocations;
- the `>=4` final-26-byte zero tail is consistent;
- the next unused slot is the normal sentinel;
- 24 MiB uses three erase/program windows and has a newly proven exact linear verify path;
- 32 MiB uses four erase/program windows and the existing full-card linear verify path;
- full-card ROM totals can still fit because EZ3Manager may place the loader inside an internal erased `FF` region.

The 0.5.12 writer therefore validates capacity rather than using a fixed 5/6/7/8-ROM limit.
