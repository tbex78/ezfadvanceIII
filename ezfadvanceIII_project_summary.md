# EZF Advance III Reverse-Engineering Project — Technical Summary

## Project goal

This project reconstructs the behavior of the original Windows **EZ3Manager** software for the **EZ-Flash Advance III** GBA flash cartridge, with the goal of reproducing its cartridge image-building rules and USB flash-management protocol on Unix-like systems using C++17 and libusb. The native project scope is macOS, Linux, and BSD; Windows users are expected to use a Linux VM with USB passthrough.

The project is intentionally **evidence-driven**:

- USBPcap captures from original EZ3Manager are treated as protocol fixtures.
- Real GBA console behavior is considered more authoritative than USB success alone.
- Generic structural rules are preferred over title-specific hacks.
- Unproven read/write mappings are not guessed.
- The writer never silently patches ROM save routines.

Current shared project/toolset version covered by this summary: **0.7.13**.

All mainline utilities carry this same version:

```text
ezfadvanceIII_multirom_writer
ezfadvanceIII_card_reader
ezfadvanceIII_save_reader
ezfadvanceIII_wipe_card
```

Beginning with 0.6.0, a code change to **any one** of these utilities bumps the shared version for **all four**, even when some utilities have no functional change in that release.

Beginning with **0.6.2**, runtime banners do not contain a hard-coded project version. Release identity is carried by filenames, source comments, tags/releases, packaged artifacts, and documentation. The GBA header's `ROM version` field is unrelated and remains part of ROM inspection output.

---

## Shared versioning policy

From **0.6.0** onward, the project uses one version across writer, card reader, save reader, and wipe utility. If code changes in at least one program, the next release number applies to all four programs. This keeps logs, documentation, binaries, and support discussions aligned to one toolset release.

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

`0x98 -> 01` is used as the cartridge readiness gate. A transient `00` is
retried up to five times at 100-ms intervals; transport failures, unexpected
values, or five consecutive `00` responses still fail safely before mutation.

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

Capture-supported readback now includes:

```text
partial first window (<8 MiB):
    FFFF / 04 / 00 / 00
    then linear 0x91 reads
exact 8 MiB:
    separate 0x0040 transition
exact 16 MiB
exact 24 MiB
exact 32 MiB
Fire-Emblem-style tiny tail immediately above 16 MiB
```

The sub-8-MiB rule is supported by a single 1-MiB capture, a two-1-MiB / ~2-MiB image capture, and a single 4-MiB capture. Real hardware then validated 1-MiB, ~2-MiB, 4-MiB, ~5-MiB, and ~6-MiB layouts.

For partial first-window images:

```text
program extent -> 0x100-byte alignment
verify extent  -> 0x10000-byte / 64-KiB alignment
```

Arbitrary other partial images in the higher 16–32 MiB region are programmed normally, but full verification is skipped unless a capture-proven read mapping exists.

This avoids false failures caused by invented window-selection rules.

Current boundary:

```text
< 8 MiB                 verify
= 8 MiB                 verify
8–16 MiB partial        skip
= 16 MiB                verify
captured tiny >16 MiB   verify
16–24 MiB partial       skip
= 24 MiB                verify
24–32 MiB partial       skip
= 32 MiB                verify
```

The ~14-MiB and ~22-MiB mixed-map hardware tests work for programming/menu/launch, but full read-back remains intentionally skipped because the original-manager read mapping for those partial higher-window geometries has not been captured.

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

Current 0.6.0 policy therefore has **no small fixed ROM-count limit**. It requires:

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
Classic NES EEPROM_V124                     -> map 4
Tales of Phantasia EEPROM_V124              -> map 5
FLASH-family metadata                       -> map 6
```

The same `EEPROM_V124` marker therefore occurs with both map 4 and map 5. The current writer does not guess between them; EEPROM ROMs require an explicit per-ROM `--mapN=4` or `--mapN=5` until a generic discriminator is recovered.

Controlled hardware A/B tests show that the distinction is functionally significant. Both Classic NES titles work with map 4. When either one is forced to map 5, programming and full read-back verification still succeed, but the game displays the same runtime error screen:

```text
GAME PACK ERROR
TURN THE POWER OFF.
```

The screen has a black background, a centered white rectangular border, `GAME PACK ERROR` centered in red, and `TURN THE POWER OFF.` centered below it. This proves that byte-perfect flash verification does not validate runtime map correctness.

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

Classic NES Super Mario EEPROM_V124 -> map 4
Classic NES Castlevania EEPROM_V124  -> map 4
Tales of Phantasia EEPROM_V124       -> map 5
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

For metadata families with a generic captured rule, classification should remain metadata-faithful rather than attempting runtime save-code detection. EEPROM remains explicit because its map-4/map-5 discriminator is not yet known.

`FLASH1M_V... -> map 6` remains a generic metadata-based implementation rule that still deserves dedicated capture validation.

`EEPROM_V124` is directly capture-proven with **map 4** in Classic NES Super Mario/Castlevania and **map 5** in Tales of Phantasia. The open problem is the generic capacity/configuration discriminator, not the marker revision.

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
| single 1 MiB Classic NES Super Mario, map 4 | yes | yes |
| single 1 MiB Classic NES Castlevania, map 4 | yes | yes |
| single 1 MiB Classic NES Super Mario, forced map 5 | full verify succeeds | runtime `GAME PACK ERROR` |
| single 1 MiB Classic NES Castlevania, forced map 5 | full verify succeeds | identical runtime `GAME PACK ERROR` |
| 1 MiB + 1 MiB Classic NES, map 4 + 4 | yes | yes |
| single 4 MiB F-Zero | yes | yes |
| 4 MiB + 1 MiB, map 3 + 4 | derived from captured rules | yes |
| 4 MiB + 1 MiB + 1 MiB, map 3 + 4 + 4 | derived from captured rules | yes |
| ~14 MiB: Advance Wars + F-Zero + two Classic NES, maps 6 + 3 + 4 + 4 | partial verify unproven | yes |
| ~22 MiB: Tales + F-Zero + two Classic NES, maps 5 + 3 + 4 + 4 | partial verify unproven | yes |
| single 8 MiB Advance Wars | yes | yes |
| 6 ROMs / 24 MiB | yes | yes |
| 6 ROMs / 32 MiB | yes | yes |
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

### Progress display / verbose diagnostics

Default destructive writes show live in-place progress bars for erase, program, and verification, including percentage, elapsed time, throughput where meaningful, and ETA.

Use:

```bash
--verbose
```

to restore detailed per-sector/per-block diagnostics and individual transfer timing.

### Write without readback

```bash
ezfadvanceIII_multirom_writer \
  --yes-really-write \
  --skip-verify \
  rom1.gba rom2.gba
```

### Manual catalog overrides

Supported for protocol experiments; multi-digit slot numbers are accepted. EEPROM ROMs currently require an explicit map 4 or map 5 override:

```text
--typeN=VALUE
--mapN=VALUE
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

### EEPROM map-4 / map-5 discriminator

`EEPROM_V124` is proven with both map 4 and map 5. The missing rule is the generic ROM-level property that makes EZ3Manager choose one or the other.

The Classic NES A/B tests prove map 4 and map 5 are not interchangeable: map 4 works, while map 5 still writes and fully verifies but is rejected at runtime with the same `GAME PACK ERROR / TURN THE POWER OFF.` screen in both tested titles.

Until the discriminator is recovered, EEPROM map selection remains explicit.

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

Shared version 0.6.0 keeps the toolset on the same C++17/libusb Unix-like platform policy:

```text
macOS       target; current 0.7.13 baseline derives from code compiled on Apple Silicon/Homebrew
Linux       target; compile/hardware validation pending
FreeBSD     target; validation pending
OpenBSD     target; validation pending
NetBSD      target; validation pending
DragonFly   target; validation pending
Windows     no native mainline support; use a Linux VM with USB passthrough
```

The portability foundation remains the same. The 0.6.0 version synchronization changes release/version policy, not the Unix-like platform scope.

---

## 0.6.1 / 0.6.2 release changes

**0.6.1** clarified the evidence-backed verification boundary. It did not enable speculative higher-window read mappings: partial 8–16, 16–24, and 24–32 MiB images continue to skip full verification unless a capture-proven path exists.

**0.6.2** removed hard-coded project-version text from all four runtime banners. Writer, card reader, save reader, and wipe utility still share the synchronized project version, but the executable output no longer duplicates it internally. No protocol behavior changed in card reader, save reader, or wipe utility.

## 0.7.0 release changes

**0.7.0** marks the object-oriented architecture migration. Shared USB,
transport, protocol, cartridge, save-reading, verification, and option-parsing
behavior now lives in reusable C++17 modules, with offline unit tests and
explicit application-service boundaries for the four utilities.

## 0.7.1 release changes

**0.7.1** hardens writer command-line parsing. Malformed or oversized
`--typeN=VALUE` and `--mapN=VALUE` overrides now produce normal parser errors
instead of allowing numeric conversion exceptions to escape. Offline regression
tests cover valid boundaries, malformed values, and overflow.

## 0.7.2 release changes

**0.7.2** consolidates ARM entry-branch encoding and decoding in
`CartridgeFormat` and adds offline regression tests for known encodings,
little-endian bytes, signed immediates, range boundaries, invalid instructions,
and invalid targets. USB protocol behavior is unchanged.

## 0.7.3 release changes

**0.7.3** expands offline protocol tests to cover exact command layouts,
selector and data preservation, matching and malformed echoes, command/data/IN
transfer failures, timeout propagation, and zero/custom settle delays. Protocol
implementation and USB behavior are unchanged.

## 0.7.4 release changes

**0.7.4** adds a reusable test-only transcript transport that validates exact
transfer ordering, direction, bytes, requested sizes, and timeouts, with clear
diagnostics for missing, extra, or mismatched transfers. Existing protocol
success tests now exercise this helper. Production and USB behavior are
unchanged.

## 0.7.5 release changes

**0.7.5** introduces a narrowly scoped `VerificationSession` and moves only the
partial first-window (`<8 MiB`) post-program verification path behind
`Transport&`. A complete offline transcript fixture proves the capture-derived
`FFFF`, `04`, `00`, `00` transition, rounded global-linear `0x91` reads, and
the absence of exact-window mapping selectors and unlock transitions. Exact
8/16/24/32-MiB and tiny-tail paths remain unchanged in the writer.

The extracted path was subsequently hardware-confirmed with a single 4-MiB
F-Zero image after a full card wipe: programming and full read-back verification
succeeded, and the cartridge booted successfully on a real Game Boy Advance.

## 0.7.6 release changes

**0.7.6** moves only the exact 8-MiB verification path into
`VerificationSession`. Its offline transcript preserves the explicit `0040`
mapping sequence, the narrowly injected 125-ms transition delay, and all 128
global-linear `0x91` reads. It also proves that `0020`, `0080`, and `00C0`
selectors are absent. Higher verification paths remain unchanged in the
writer. The extraction was subsequently hardware-confirmed with an exact
8-MiB image: writing and full read-back verification succeeded, including the
final 64-KiB block, and the cartridge booted successfully on a real Game Boy
Advance.

## 0.7.7 release changes

**0.7.7** strengthens the exact 8-MiB offline fixture by giving every 64-KiB
block distinct content and asserting every verified callback offset and length.
The explicit test-delay constructor now rejects an empty callback immediately
instead of deferring failure until a captured delay is reached. Production
timing and USB behavior are unchanged.

## 0.7.8 release changes

**0.7.8** moves only the exact 16-MiB verification path into
`VerificationSession`. Its complete offline transcript preserves the
capture-derived `0080` mapping transition, the explicit 125-ms delay, and all
256 global-linear 64-KiB `0x91` reads. The fixture uses block-distinct data,
checks every callback offset and length, and proves that `0020`, `0040`, and
`00C0` mapping selectors are absent. Verification policy, erase/program logic,
and the tiny-tail and exact 24/32-MiB paths remain unchanged.

The extracted path was subsequently confirmed on real hardware with an exact
16-MiB image: writing succeeded, full read-back verification completed through
the final 64-KiB block at `0x00ff0000`, and the cartridge booted successfully
on a real Game Boy Advance.

## 0.7.9 release changes

**0.7.9** moves only the `fireemblem.pcap` tiny-tail-above-16-MiB
verification path into `VerificationSession`. The operation accepts exactly
`16 MiB < size <= 16 MiB + 64 KiB`, rounds verification through `0x1010000`,
and expects erased `0xFF` bytes beyond the image end. Its transcript proves the
short status/read prefix, all 257 global-linear 64-KiB reads and callbacks,
zero verification delays, and the absence of `0020`, `0040`, `0080`, `00C0`,
`0200`, and `AA55`. Exact 24/32-MiB verification remains in the legacy writer
verifier, and other partial higher-window geometries remain unsupported.

The extracted path was subsequently confirmed with the capture-derived single
Fire Emblem layout. The loader was placed at `0x01000010`, producing a
`0x1000700` image and a `0x700`-byte BANK2 program tail. Full read-back
verification completed through the rounded final block at `0x01000000`, the
card reader identified the single-ROM layout and valid header, and the game
booted successfully on a real Game Boy Advance.

## 0.7.10 release changes

**0.7.10** moves only the exact 24-MiB verification path from the legacy
writer into `VerificationSession`. Its complete transcript preserves the
capture-derived `00C0` mapping transition and all 384 global-linear 64-KiB
reads through final offset `0x017f0000`. The capture shows an approximately
109-ms quiet interval; the implementation intentionally retains the established
exact `125000`-microsecond settle, asserted once after
`55AA / 0200 / 00C0 / 0000` and immediately before `AA55`. The fixture uses
block-distinct data, checks every callback, rejects wrong sizes, and proves
that `0020`, `0040`, and `0080` selectors are absent. Exact 32-MiB verification
remains in the legacy writer verifier.

The extracted path was subsequently confirmed on real hardware with an exact
five-ROM `0x01800000` image. Programming succeeded, the preserved 125-ms
transition completed, and full read-back verification reached the final
64-KiB block at `0x017f0000`. Card inspection and menu behavior were correct,
and all selected games launched successfully on a real Game Boy Advance.

## 0.7.11 release changes

**0.7.11** adds a read-only `Stored end` field to every ROM reported by the
card reader. The value is the inclusive physical end address derived from the
catalog entry's stored start and capture-derived size class (`type 0` = 32 MiB
through `type 9` = 64 KiB). Invalid catalog size classes or extents are reported
explicitly instead of producing a wrapped address. USB and card state behavior
are unchanged.

## 0.7.12 release changes

**0.7.12** moves the final exact 32-MiB verification path into
`VerificationSession`. The capture-derived transition deliberately emits no
`00C0` selector because programming already ended in that window. Its complete
transcript preserves `55AA / 0000 / 0000 / 0200`, exactly one
`125000`-microsecond delay immediately before `AA55`, and all 512 global-linear
64-KiB reads through final offset `0x01ff0000`. The fixture uses block-distinct
data, checks every callback, rejects wrong sizes, and proves `0020`, `0040`,
`0080`, and `00C0` selectors are absent. With every supported verification
path delegated, the now-dead legacy writer-side read command, extent,
comparison, and generic verifier were removed.

## 0.7.13 release changes

**0.7.13** makes startup tolerate a cartridge that reports transiently late
readiness. The writer preflight, shared read-only card session, and wipe
preflight retry only a valid `0x98 -> 00` response up to five times with
100-ms intervals. `01` continues startup; transport failures, unexpected
response values, or five consecutive `00` responses fail safely. The writer
and reader's surrounding `0x97` and `0x99` sequence and all flash protocol
behavior are unchanged. After card inspection and after any successfully
initialized save-reader operation, the read-only tools now also replay the
already capture-derived close-manager transition—three successful `0x98` polls
followed by a 1000-ms quiet interval—before releasing the device, to avoid
leaving the bridge in the observed stale session state.


## Current project status

At shared version **0.7.13**, the project has an object-oriented structural model of original EZ3Manager behavior:

- every mainline utility shares one synchronized project version; any code update in at least one program bumps the version for all four;
- from 0.6.2, runtime banners intentionally omit the project version to avoid hard-coded duplicate version strings;
- tested cartridge geometry is four 8-MiB windows / 32 MiB total;
- there is no small fixed ROM-count limit; 1–8 active entries are capture-proven and the loader exposes 120 structural catalog slots as a safety bound;
- total input ROM bytes may be up to and including 32 MiB / 256 Mbit, provided packing and loader placement still fit;
- ROM ordering is stable largest-first;
- placement is size-class based and reuses erased padding/internal `FF` runs;
- single and multi loaders are capture-derived and relocatable; the same `0x7080` multi-loader with 125 relocations matches captured 2–8 ROM configurations;
- catalog `type` is ROM-size based;
- catalog `map` uses capture-derived values `3`, `4`, `5`, and `6`; EEPROM map 4 versus 5 is explicit because `EEPROM_V124` occurs with both;
- hardware A/B testing proves map correctness is independent of flash verification: both Classic NES map-5 images fully verify but fail at runtime with an identical Game Pak error screen, whereas map 4 works;
- partial first-window, extracted exact 8-/16-/24-MiB, and tiny-tail verification paths are capture-, transcript-, and hardware-proven in the tested layouts; the exact 32-MiB path is also known;
- partial first-window programming rounds to `0x100` and verification to `0x10000`;
- default destructive-write output uses progress bars, while `--verbose` exposes per-operation diagnostics;
- paired FFTA evidence proves that non-SRAM save-library signatures may survive SRAM patching while runtime save code changes;
- native scope is macOS/Linux/BSD through libusb; Windows users should use a Linux VM with USB passthrough;
- real hardware now validates map-4 single/multi cases, mixed map-3/map-4 menus, 4-, 8-, exact 16-MiB, and Fire-Emblem-style tiny-tail verification checkpoints, and 6-ROM 24-/32-MiB images.

The remaining work is primarily **expanding evidence coverage**, especially the EEPROM map-4/map-5 discriminator, 9+ menu counts, save-bank behavior, and Linux/BSD hardware validation.

## Multi-ROM evidence carried into 0.6.0

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

The current 0.7.13 writer therefore validates capacity rather than using a fixed 5/6/7/8-ROM limit.
