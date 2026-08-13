# EZF Advance III Reverse-Engineering Project

**Technical architecture, protocol, image-format, and validation documentation**  
**Current writer implementation:** `ezfadvanceIII_multirom_writer 0.5.10`  
**Target hardware:** EZ-Flash Advance III / EZF Advance III, 256 Mbit (32 MiB) GBA flash cartridge  
**Host implementation:** C++17 + libusb; native project scope is macOS, Linux, and BSD. Current 0.5.10 compilation is verified on macOS / Apple Silicon; Linux/BSD validation remains pending.

---

## 1. Project purpose

This project is a hardware-driven reimplementation of the original Windows **EZ3Manager** behavior for an **EZ-Flash Advance III** Game Boy Advance flash cartridge.

The goal is not to design a new cartridge format, invent a new boot menu, or implement a generic GBA flash-cart protocol from documentation. The goal is to reproduce the behavior of the original Windows manager closely enough that images produced and programmed by the Unix-like C++/libusb implementation behave like images produced by EZ3Manager on real GBA hardware.

The reverse-engineering method is empirical:

1. Run the original Windows software.
2. Capture its USB traffic with USBPcap.
3. Recover command framing, timing, flash-window state, erase geometry, program geometry, readback behavior, loader bytes, catalog entries, ROM ordering, and placement rules.
4. Reproduce those behaviors in C++/libusb.
5. Program a real EZ-Flash Advance III cartridge.
6. Test the cartridge on an original GBA console.
7. Treat hardware behavior as the final arbiter.

A central project rule is:

> **Do not add per-game hacks when a generic rule can be recovered from captures.**

ROM size, ROM geometry, save-library signatures, catalog fields, flash-window boundaries, and loader placement are treated as structural properties. A title name may be used to identify a test case in documentation, but should not be used as a runtime special case unless the original manager itself demonstrably does so.

---

## 2. Evidence terminology

Throughout this document, claims are classified informally as follows.

### 2.1 Capture-proven

Behavior was observed directly in USB traffic from the original Windows EZ3Manager.

Examples:

- the four 8-MiB flash-window selectors;
- 64-KiB program transactions;
- `type=3` for 4-MiB ROMs;
- `map=5` for Tales of Phantasia / `EEPROM_V124`;
- four catalog entries in the 4-ROM capture.

### 2.2 Hardware-proven

The behavior was reproduced by the custom writer and then successfully exercised on a physical original GBA console.

Examples:

- 4 + 4 MiB after correcting catalog metadata;
- 8 + 4 + 4 MiB using automatic 0.5.5 classification;
- 8 + 8 + 8 + 8 MiB four-ROM menu and launches.

### 2.3 Capture + hardware proven

Both conditions are satisfied. These are the strongest project facts.

### 2.4 Inferred

A rule is consistent with all observed captures but has not yet been isolated by a dedicated capture.

Examples include some untested ROM size classes such as 2 MiB, 1 MiB, 512 KiB, etc., and whether every possible `EEPROM_Vxxx` revision uses map 5.

### 2.5 Deliberately unsupported or conservative

The writer intentionally refuses to invent behavior where the original read/write state mapping is unknown. The most important example is partial higher-window readback verification.

---

## 3. Hardware target

### 3.1 USB identity

The EZF Advance III USB bridge used by this project enumerates as:

```text
VID = 0x0E6A
PID = 0x5088
Interface = 0
Bulk OUT endpoint = 0x02
Bulk IN endpoint  = 0x81
```

The writer uses libusb and automatically asks libusb to detach a kernel driver if necessary before claiming interface 0.

### 3.2 Cartridge capacity

The physical cartridge used in the reverse-engineering work is:

```text
256 Mbit = 32 MiB = 0x02000000 bytes
```

Project unit convention:

```text
8 Mbit   = 1 MiB
16 Mbit  = 2 MiB
32 Mbit  = 4 MiB
64 Mbit  = 8 MiB
128 Mbit = 16 MiB
256 Mbit = 32 MiB
```

### 3.3 Physical flash geometry

The important constants in the current writer are:

```text
PROGRAM_BLOCK      = 0x00010000  = 64 KiB
FLASH_WINDOW_SIZE  = 0x00800000  = 8 MiB
CARD_HALF_SIZE     = 0x01000000  = 16 MiB
MAX_CARD_IMAGE     = 0x02000000  = 32 MiB
MAX_ROMS           = 4
```

The 32-MiB cartridge is exposed to the programming protocol as **four 8-MiB physical program/erase windows**.

---

## 4. USB command framing

Most bridge commands use a 13-byte command structure beginning with:

```text
5A A5
```

A simplified common layout is:

```text
Offset  Size  Meaning
------  ----  ------------------------------------------------
0x00    1     0x5A
0x01    1     0xA5
0x02    1     opcode
0x03    1     subcommand / mode
0x04    4     little-endian word address or parameter
0x08    4     little-endian transfer length or parameter
0x0C    1     mode/final/status byte
```

Addresses used by ROM read/program commands are generally **16-bit word addresses**, so:

```text
word_address = byte_address / 2
byte_address = word_address * 2
```

This distinction is critical when decoding PCAPs: catalog starts are byte addresses, while many bridge commands carry word addresses.

---

## 5. Bridge startup and cartridge readiness

The original-manager startup sequence includes three bridge-level commands:

```text
0x97 -> expects one byte 0x00
0x98 -> expects one byte 0x01
0x99 parameter 0x01 -> expects a 13-byte command echo
```

### 5.1 `0x98` is the readiness gate

The `0x98` response is treated as the cartridge presence/readiness signal:

```text
expected: 01
```

If it is missing or not `01`, the custom writer aborts **before erase or programming**.

This was important because earlier experimental writers could communicate with the USB bridge even when the cartridge itself was absent or not electrically ready.

### 5.2 Why `0x97/0x98/0x99` are replayed

Early experiments that sent only a subset of the original initialization sequence could read and erase but failed to sustain full 64-KiB program transfers. Restoring the complete startup path fixed the large-transfer state.

The project therefore treats the startup phase as bridge state configuration, not merely informational probing.

---

## 6. Original-manager flash probing

The current writer reproduces the manager's initialization/probe phase before destructive operations.

The sequence includes classic flash-command style writes around word addresses `0x555` and `0x2AA`, ID/readback operations, resets, and repeated probing with different window selectors.

The four observed probe windows are:

```text
window selector 0x0000
window selector 0x0040
window selector 0x0080
window selector 0x00C0
```

The writer reports the cartridge geometry as:

```text
Status            : inserted / ready
EZ3 probe windows : 4 x 8 MiB
Inferred geometry : 32 MiB / 256 Mbit
```

This is intentionally described as a **geometry inference**, not a formal JEDEC density decode. The project has not proven how a smaller device that mirrors upper windows would behave.

---

## 7. Flash-window state machine

The capture-proven program/erase windows are:

| Physical card range | Window index | Select word |
|---|---:|---:|
| `0x0000000-0x07FFFFF` | 0 | base/default |
| `0x0800000-0x0FFFFFF` | 1 | `0x0040` |
| `0x1000000-0x17FFFFF` | 2 | `0x0080` |
| `0x1800000-0x1FFFFFF` | 3 | `0x00C0` |

The non-zero window setup pattern is structurally:

```text
55AA
0200
WINDOW_SELECT
0000
~125 ms quiet interval
AA55
0000
0000
0000
AA
55
06
```

Concrete selectors:

```text
BANK1: 0040
BANK2: 0080
BANK3: 00C0
```

The `~125 ms` quiet interval before `AA55` is not cosmetic. It was repeatedly present in successful Windows captures and became an important part of achieving reliable programming on Apple Silicon.

### 7.1 Status/reset sequence

A common status/reset sequence is:

```text
FFFF
04
00
00
```

The writer uses this when leaving program state, between flash windows, and before certain verification transitions.

---

## 8. Erase protocol

Erase operations use opcode `0x96` in a 13-byte command.

Conceptually:

```text
5A A5 96 00
<32-bit little-endian word address>
00 00 00 00
00
```

The bridge returns a 13-byte completion response whose first 12 bytes echo the request and whose final status byte is expected to be zero.

### 8.1 <= 8 MiB selective erase

For images contained entirely in window 0, the writer uses a selective erase list derived from original-manager captures rather than erasing the full cartridge.

The capture includes a boot-sector group followed by erase commands covering the programmed image extent.

### 8.2 Multi-window erase

For images larger than 8 MiB, the writer uses capture-derived full-window erase address lists and switches windows as required.

For a full 32-MiB card, the original manager performs:

```text
135 erase commands per 8-MiB window
4 windows
540 erase operations total
```

This exact four-window geometry is used by the separate full-card wipe utility as well.

### 8.3 Fire Emblem special geometry

The Fire Emblem capture exposed a special single-ROM case where the 16-MiB ROM itself fills the first half and the loader is placed just above 16 MiB. In that case only a tiny BANK2 tail is required; the original manager erases only the necessary beginning of that window.

The current implementation preserves this capture-derived behavior.

---

## 9. Programming protocol

ROM data is programmed in blocks of up to:

```text
0x10000 bytes = 64 KiB
```

A program transaction begins with opcode `0x92`, subcommand `0x00`:

```text
Offset  Value
------  -------------------------------------
0       5A
1       A5
2       92
3       00
4..7    local word address
8..11   byte length
12      41 during program request
```

The sequence is:

1. Send the 13-byte program command.
2. Wait the capture-derived command/data settle interval.
3. Send the raw ROM block through bulk OUT.
4. Read a completion response.
5. Expect the same command with final byte changed from `0x41` to `0x00`.

The current writer uses:

```text
COMMAND_DATA_SETTLE_US = 750
```

### 9.1 Local addressing inside an 8-MiB window

At physical offset `off`, programming uses:

```text
window    = off / 0x800000
local_off = off % 0x800000
```

When crossing an exact 8-MiB boundary, the writer changes the selected flash window and restarts local programming addresses from zero.

This is why image layout matters: a ROM that unnecessarily straddles a physical 8-MiB boundary may not behave the same as one packed by the original manager into a single suitable slot.

---

## 10. Readback protocol

ROM readback uses opcode `0x91`:

```text
5A A5 91 00
<32-bit little-endian word address>
<32-bit little-endian byte length>
00
```

The bridge then returns exactly the requested data bytes on bulk IN.

### 10.1 Important distinction: program-window mapping != read-window mapping

A major reverse-engineering failure occurred when an experimental verifier assumed that selecting BANK1 for programming would make subsequent local `0x91` reads return BANK1.

Real hardware disproved this.

In a partial BANK1 test, the verifier expected the first byte of the higher window but read the first byte of window 0 instead. This established that **program-window selection cannot be reused as a generic local read-window selector**.

That experiment is why the current writer is conservative about verification.

---

## 11. Verification policy

The writer only performs full readback verification for geometries whose read mapping is supported by original-manager captures.

### 11.1 Capture-supported full verification

Current full linear verification is used for:

```text
image <= 8 MiB
image == 16 MiB
image == 32 MiB
```

There is also a capture-specific Fire Emblem path for a very small loader tail immediately beyond 16 MiB.

### 11.2 Exact 16-MiB verify transition

The original manager does not simply leave BANK1 selected and begin reads. It enters a separate linear-read mapping involving selector `0x0080`, status/reset operations, a 125-ms settle interval, and a final read prefix before issuing sequential `0x91` reads.

This is intentionally different from BANK1 program selection, which uses `0x0040`.

### 11.3 Exact 32-MiB verify transition

A separate capture-derived transition is used after programming BANK3. It prepares a full 256-Mbit linear read mapping, after which the writer verifies all 512 x 64-KiB blocks.

### 11.4 Partial higher-window images

For an image such as a generic 24-MiB layout, programming is supported because the program-window geometry is known. However, the exact original-manager readback mapping for that partial geometry is not yet known.

The writer therefore:

1. finishes programming;
2. returns the flash/bridge to a normal status state;
3. reports that full readback verification was skipped;
4. does **not** fabricate a false verification failure.

### 11.5 `--skip-verify`

When explicitly requested:

```bash
--skip-verify
```

the writer performs erase and programming normally but does not issue post-write ROM `0x91` comparison reads. A short status/reset cleanup is still sent.

---

## 12. Cartridge image architecture

The image written to the cartridge consists of:

- one or more GBA ROM images;
- an EZ3 loader/menu template;
- a catalog embedded inside the loader;
- one patched ARM branch at cartridge address zero;
- possibly reused erased `0xFF` regions inside ROM files.

The current implementation constructs the entire image **in memory**. Since 0.5.4, it no longer writes an intermediate `ezfadvanceIII_multirom_image.bin` file.

---

## 13. GBA entry branch patch

The first physical/catalog ROM is special.

Its original first ARM instruction is decoded to recover its original branch target. The custom image replaces the first instruction with a branch to the relocated EZ3 loader/menu.

Only physical/catalog ROM #1 is patched this way.

Later ROMs retain their original first instruction and the catalog stores their physical start address as their launch target metadata.

The GBA ROM CPU mapping base used when relocating loader literals is:

```text
GBA_ROM_BASE = 0x08000000
```

---

## 14. Loader templates

Two capture-derived loader families are embedded in the source.

### 14.1 Single-ROM loader

```text
Original template base : 0x0000D3B0
Length                 : 0x660 bytes
Relocations            : 13
Catalog entry offset   : 0x4F8
```

The single loader contains one catalog entry and two count fields that are set to 1.

### 14.2 Multi-ROM loader

```text
Original template base : 0x0004A880
Nominal length         : 0x7080 bytes
Relocations            : 125
Header offset          : 0x475E
Catalog entry offset   : 0x476E
```

A 28-byte region at loader-relative offset `0x5764` was not fully visible in USBPcap because the 64-KiB transfer records were truncated by the capture snap length. Those bytes were recovered directly from a cartridge programmed by original EZ3Manager and are explicitly restored before use.

---

## 15. Loader relocation

The loader template contains absolute GBA addresses pointing into its original load range.

Relocation works by scanning 32-bit aligned words in the template. A word is relocated when it lies within:

```text
[GBA_ROM_BASE + original_loader_base,
 GBA_ROM_BASE + original_loader_base + template_size]
```

The relocation delta is:

```text
delta = new_loader_start - original_loader_base
```

and the relocated value is:

```text
new_value = old_value + delta
```

Relocation counts are validated:

```text
single loader: 13
multi loader : 125
```

Unexpected counts are treated as a build error because they would imply either template corruption or an incorrect relocation assumption.

---

## 16. Multi-ROM loader variants

The original manager changes the serialized loader form based on ROM count.

### 16.1 Two-ROM form

Capture-derived behavior:

```text
programmed loader extent = 0x6F80
not full 0x7080
```

The region from `0x6F26` to `0x6F7F` is left erased (`FF`).

The unused third catalog slot is encoded with a specific sentinel:

```text
byte 0      = 00
bytes 1..15 = ASCII spaces
bytes 16..27 = 00
```

### 16.2 Three-ROM form

Uses the full:

```text
0x7080-byte multi loader
```

The final 26 bytes remain `FF` in the captured three-ROM form.

### 16.3 Four-ROM form

The 4 x 8-MiB capture proves four active catalog entries.

It also exposes a small but real four-ROM loader variant:

```text
loader offsets 0x7066..0x707F = 00
length = 0x1A = 26 bytes
```

Otherwise the loader is the same capture-derived `0x7080` template.

### 16.4 More than four entries

The captured loader contains many unused 28-byte slot-like structures, suggesting the original menu architecture may have been designed for substantially more entries.

However, the project currently caps support at:

```text
MAX_ROMS = 4
```

because four is the highest entry count that has been both captured and exercised on hardware in this project.

---

## 17. Catalog entry binary format

Each catalog entry is exactly:

```text
28 bytes
```

Layout:

```text
Offset  Size  Meaning
------  ----  ----------------------------------------------------------
0x00    16    display name, max 16 bytes
0x10    4     type field: entry_type << 24
0x14    4     packed physical start + mapping/config byte
0x18    4     first ROM: original ARM entry target
              later ROMs: physical ROM byte start
```

### 17.1 Type field

Serialized as:

```text
type_field = entry_type << 24
```

For example:

```text
type 3 -> 00 00 00 03 in capture byte order representation
```

The important semantic discovery is that **type is a ROM size class**, not a save type.

### 17.2 Packed start/mapping field

Formula:

```text
packed_start = ((rom_start / 2) << 8) | mapping_flag
```

This packs the ROM start as a word address into the high 24 bits and reserves the low byte for the map/config value.

Examples:

```text
ROM start 0x000000, map 6 -> 0x00000006
ROM start 0x400000, map 6 -> 0x20000006
ROM start 0x800000, map 6 -> 0x40000006
ROM start 0xC00000, map 3 -> 0x60000003
ROM start 0x1000000, map 3 -> 0x80000003
ROM start 0x1800000, map 3 -> 0xC0000003
```

---

## 18. Catalog `type`: ROM size class

A major correction occurred after the 4-MiB captures.

Earlier builds incorrectly tried to derive `type` from save-library signatures. The 4-MiB captures demonstrated that the field follows ROM size.

Current rule:

| ROM size class | Catalog type |
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

Equivalent conceptual formula for power-of-two sizes:

```text
type = log2(32 MiB / ROM_size)
```

or, with size in bytes:

```text
type = 25 - log2(size_bytes)
```

The implementation starts at the 64-KiB class (`type 9`) and doubles the size class until the ROM fits, decrementing type each time.

### 18.1 Non-power-of-two ROMs

Unusual files are rounded up to the next power-of-two size class, consistent with the placement allocator.

This behavior is generic but not capture-proven for every possible unusual homebrew size.

---

## 19. Catalog `map`: EZ3 mapping/configuration associated with ROM metadata

The map byte is independent of ROM size.

The current captures show a strong association between embedded Nintendo/SDK save-library signatures and the map value selected by EZ3Manager:

```text
SRAM / SRAM_F / ordinary non-FLASH metadata -> map 3
EEPROM_V metadata                           -> map 5
FLASH-family metadata                       -> map 6
```

This must **not** be interpreted as proof that the ROM is actively using that save technology at runtime. A ROM can be manually SRAM-patched by a separate tool while still retaining its original `FLASH_V...`, `FLASH512_V...`, or `EEPROM_V...` ASCII signature in unused or bypassed code.

Therefore `map 3`, `map 5`, and `map 6` are treated here as **EZ3 catalog mapping/configuration values associated with metadata observed in captures**, not as definitive runtime save-mode declarations.

### 19.1 Captured map-3 examples

```text
F-Zero              SRAM_V111    -> map 3
MegaManZ            SRAM_V112    -> map 3
Fire Emblem         SRAM_F_V102  -> map 3
Kingdom Hearts      SRAM_F_V103  -> map 3
Piano               no standard FLASH marker -> map 3
```

### 19.2 Captured map-6 examples

```text
Advance Wars        FLASH_V121     -> map 6
Mario Kart          FLASH_V124     -> map 6
Advance Wars 2      FLASH_V126     -> map 6
FFTA                 FLASH512_V130  -> map 6
```

**Important FFTA paired-control result:** the project now has both an unpatched FFTA control capture and an independently SRAM-patched FFTA ROM. The unpatched ROM contains `FLASH512_V130` at ROM offset `0x370820` and EZ3Manager serializes `type 1 / map 6` with the single loader at `0xD97730`. The SRAM-patched ROM retains the same `FLASH512_V130` text unchanged.

A direct ROM-to-ROM comparison shows that the SRAM patch modifies only **94 bytes total**, grouped into five localized regions between `0x1454D8` and `0x145B4D`. The replacement code redirects save reads/writes to the GBA SRAM address space at `0x0E000000`, bypasses FLASH-management routines, and leaves the old FLASH library signature intact.

The earlier EZ3Manager capture of the SRAM-patched FFTA also serialized `type 1 / map 6` and used the same loader location `0xD97730`. Therefore the patched and unpatched forms have the same key EZ3 catalog classification even though their effective save code differs.

That proves:

```text
embedded FLASH signature != guaranteed active FLASH save implementation
map 6                    != proof that the ROM currently performs FLASH saves
SRAM patch state         != a reason to change FFTA from map 6 to map 3
```

It also means the current signature-based classifier is correct for reproducing captured EZ3 catalog behavior while intentionally remaining separate from runtime save-code analysis.

The current detector also recognizes:

```text
FLASH1M_Vxxx -> map 6
```

as a generic extension of the observed FLASH-family metadata association. A dedicated `FLASH1M` PCAP/hardware validation is still desirable.

### 19.3 Captured map-5 EEPROM example

`TOF-EEPROM.pcap` proves:

```text
Tales of Phantasia
ROM size        = 16 MiB
embedded marker = EEPROM_V124
catalog type    = 1
mapping flag    = 5
```

The original manager did **not** patch the ROM's save code. The EEPROM library remained in the programmed image.

The current implementation therefore maps generic `EEPROM_V...` metadata to 5, while noting that universality across every EEPROM library revision/capacity is still inferred from the `EEPROM_V124` capture.

### 19.4 Embedded signature versus active save implementation

Save patching is external to EZ3Manager. A dedicated patching tool may rewrite calls or routines so the game uses SRAM while leaving the original save-library signature in the ROM.

Static string detection answers:

> "Which save-library marker is still embedded in this ROM?"

It does **not** necessarily answer:

> "Which save technology will this patched ROM use at runtime?"

A true runtime classifier would require understanding the patch transformation itself, following save-library call sites, or detecting the replacement SRAM code path. That is outside the current writer.

### 19.5 FFTA SRAM patch byte-level evidence

The exact unpatched and SRAM-patched European FFTA ROM pair was compared byte-for-byte:

```text
ROM size, each:     0x1000000 bytes (16 MiB)
changed bytes:      94
first changed byte: 0x1454D8
last changed byte:  0x145B4D
FLASH512_V130:      unchanged at 0x370820
```

The 94 changed bytes fall into five localized patch regions:

```text
0x1454D8..0x1454FD  FLASH read path replaced by direct SRAM reads
0x145770..0x145777  FLASH identification/call behavior replaced with a constant result
0x14587C..0x145883  FLASH-management entry stubbed to immediate success
0x1458F0..0x1458F7  second FLASH-management entry stubbed to immediate success
0x145B24..0x145B4D  FLASH programming path replaced by direct SRAM writes
```

The replacement read/write code targets the GBA SRAM region beginning at `0x0E000000`. The write replacement operates in `0x1000`-byte chunks. The patch does **not** rewrite the SDK/library signature string, which is why simple signature scanning continues to report `FLASH512_V130` after the runtime path has been converted to SRAM.

This byte-level evidence strengthens the project rule that **EZ3 catalog mapping and effective runtime save implementation are separate concerns**. The writer should continue reproducing EZ3Manager's observed metadata classification rather than trying to reinterpret manually patched code.

---

## 20. Save-signature policy and warnings

The writer does not patch save routines.

This mirrors the original Windows workflow: ROM save conversion, if desired, is a separate manual user decision performed with a dedicated patching tool before running EZ3Manager/writer.

### 20.1 Signature-triggered warning

The current writer scans for these embedded markers:

```text
FLASH_V...
FLASH512_V...
FLASH1M_V...
EEPROM_V...
```

When one is present, it displays a warning and requires:

```text
Continue anyway? [y/N]:
```

Only an explicit `y` or `yes` continues. Blank input, `n`, EOF, or any other answer aborts before USB access.

The warning must be interpreted as **signature-based and conservative**. It means a non-SRAM save-library marker remains in the ROM; it does **not** prove that the ROM still uses that save method after manual patching.

FFTA is the concrete counterexample in this project: its SRAM patch changes only 94 bytes in five localized executable-code regions, redirects save access to `0x0E000000`, and leaves `FLASH512_V130` unchanged at `0x370820`. It therefore still looks like a FLASH-library ROM to simple signature scanning even though its active save path has been converted to SRAM.

The writer never patches or converts save routines automatically. SRAM conversion, if desired, must be performed separately before writing.

### 20.2 Map values, embedded markers, and runtime behavior are separate

Catalog mapping, embedded library markers, and actual runtime save behavior are three separate concepts.

A ROM can:

```text
retain FLASH512_V130 text
be manually patched to use SRAM at runtime
still receive EZ3 catalog map 6
```

Therefore the project should not describe `map 6` as "FLASH mode" or `map 5` as "EEPROM mode" without qualification. They are currently best described as **capture-derived EZ3 mapping/configuration values associated with those ROM metadata classes**.

The writer reports observed metadata and reproduces original-manager catalog behavior; it does not attempt to prove or transform the ROM's effective runtime save code.

---

## 21. ROM ordering

Original EZ3Manager does not necessarily preserve user insertion order.

The capture-supported rule is:

> **Stable descending ROM file size.**

That means:

1. larger ROM files move before smaller ROM files;
2. equal-size ROMs retain their original relative order.

Examples:

### 21.1 Piano + MegaManZ

User added:

```text
Piano     64 KiB
MegaManZ   8 MiB
```

EZ3Manager catalog/physical order:

```text
MegaManZ
Piano
```

### 21.2 Advance Wars + MegaManZ + Fire Emblem

User added:

```text
Advance Wars   8 MiB
MegaManZ       8 MiB
Fire Emblem   16 MiB
```

EZ3Manager order:

```text
Fire Emblem   16 MiB
Advance Wars   8 MiB
MegaManZ       8 MiB
```

The two 8-MiB ROMs preserve their original order.

### 21.3 Four 8-MiB ROMs

All sizes are equal, so the original order is preserved exactly.

---

## 22. Multi-ROM size-class allocator

After stable sorting, each ROM is placed according to the size class of the ROM being placed.

The allocator uses:

```text
alignment = next power-of-two >= max(64 KiB, ROM file size)
```

The important twist is that it advances by the previous ROM's **meaningful non-trailing-FF extent**, not blindly by the previous file length.

This allows smaller ROMs to reuse trailing erased padding in larger ROM files.

### 22.1 Meaningful ROM end

For placement purposes:

```text
meaningful_end = one byte after the final non-FF byte
```

Trailing `0xFF` bytes are treated as available erased space.

### 22.2 Overlap safety

The builder explicitly tracks non-FF occupied bytes. A later ROM is allowed to overlap an earlier ROM file only where the earlier bytes are `FF`.

If two meaningful non-FF regions would collide, the build aborts.

This prevents the compact allocator from silently overwriting game data.

---

## 23. Critical packing examples

### 23.1 MegaManZ + Piano

Original-manager result:

```text
MegaManZ @ 0x000000
Piano    @ 0x7F0000
loader   @ 0x2CC420
end      @ 0x800000
```

Although the nominal input sizes are 8 MiB + 64 KiB + loader, the entire image still fits exactly in 8 MiB.

Piano occupies MegaManZ's trailing erased 64-KiB region.

The loader occupies an internal erased region of MegaManZ.

This capture disproved the earlier sequential layout that placed Piano first and forced MegaManZ to straddle the physical 8-MiB boundary.

### 23.2 Fire Emblem + Advance Wars + MegaManZ

```text
Fire Emblem  @ 0x0000000
Advance Wars @ 0x1000000
MegaManZ     @ 0x1800000
loader       @ 0x15C2360
end          @ 0x2000000
```

The loader is placed inside erased space in the Advance Wars slot.

### 23.3 F-Zero + Mario Kart

Original-manager physical layout:

```text
F-Zero       @ 0x000000
Mario Kart   @ 0x400000
end          @ 0x800000
```

The 4-MiB placement was correct in the allocator even when an early custom build failed to launch the games. The actual problem was catalog metadata: both games required `type=3`, while map differed by save family.

### 23.4 Advance Wars 2 + Mario Kart + F-Zero

Original-manager result:

```text
Advance Wars 2 @ 0x0000000  type 2 / map 6
Mario Kart     @ 0x0800000  type 3 / map 6
F-Zero         @ 0x0C00000  type 3 / map 3
end            @ 0x1000000
```

This capture simultaneously validated:

- 8-MiB-before-4-MiB sorting;
- 4-MiB alignment;
- size-derived type values;
- FLASH/SRAM map separation;
- exact 16-MiB program/verify behavior.

### 23.5 Four 8-MiB ROMs

Capture/hardware result:

```text
ROM #1 @ 0x0000000
ROM #2 @ 0x0800000
ROM #3 @ 0x1000000
ROM #4 @ 0x1800000
end    @ 0x2000000
```

The loader is embedded in erased space inside one ROM so the nominally full 32-MiB ROM set does not require capacity beyond the cartridge.

---

## 24. Loader placement strategy

Loader placement is not simply "append after ROMs".

The original manager aggressively reuses erased `0xFF` regions.

### 24.1 Multi-ROM loader

The multi-ROM builder searches the completed packed image for the first sufficiently large 16-byte-aligned `FF` run.

If found:

```text
loader_start = aligned internal/trailing FF slot
```

If no suitable slot exists, it appends the loader at the next 16-byte boundary, provided the result still fits within 32 MiB.

If neither is possible, the build fails rather than overwriting ROM data.

### 24.2 Single-ROM loader

Single-ROM captures expose subtler behavior.

The actual copied single loader is only:

```text
0x660 bytes
```

but internal slot selection reserves a gap large enough for the full multi-loader footprint:

```text
0x7080 bytes
```

This behavior was exposed by Tales of Phantasia.

### 24.3 Tales of Phantasia

Tales contains an earlier FF run large enough for `0x660` but smaller than `0x7080`; original EZ3Manager skips it.

The manager chooses a later large run beginning around:

```text
0xD49010
```

and starts the loader at the next 16-byte boundary:

```text
0xD49020
```

Relocating the existing single loader to `0xD49020` and inserting `type 1 / map 5` matches all visible loader bytes from the PCAP.

### 24.4 FFTA

Both the unpatched FFTA control capture and the earlier SRAM-patched FFTA evidence are consistent with the same single-loader placement:

```text
0xD97730
```

The unpatched control capture also confirms `type 1 / map 6`. This reinforces that the SRAM patch state does not alter the observed loader placement or key EZ3 catalog classification for this title.

### 24.5 Fire Emblem fallback

Fire Emblem is a full 16-MiB ROM with no suitable internal loader slot under the capture-derived rule.

The original manager places the loader just into the next half:

```text
loader @ 0x01000010
```

with 16 zero bytes before it.

This path produces a very small BANK2 tail and has its own capture-derived verify behavior.

---

## 25. Image size versus programmed size

The logical constructed image is always bounded by:

```text
0x02000000 bytes
```

For very small single-ROM images, Windows still writes at least one 64-KiB block, so the custom writer enforces:

```text
programmed_size = max(0x10000, image.size())
```

and pads with `FF` as needed.

---

## 26. Current CLI behavior

### 26.1 Dry run

```bash
ezfadvanceIII_multirom_writer rom1.gba [rom2.gba] [rom3.gba] [rom4.gba]
```

Behavior:

- loads and analyzes ROMs;
- displays non-SRAM warnings and requires confirmations when applicable;
- derives catalog type/map unless overridden;
- sorts and packs the ROMs;
- constructs the loader/catalog in memory;
- prints final layout;
- does **not** open USB;
- does **not** modify the cartridge;
- does **not** create an intermediate BIN file.

### 26.2 Destructive write

```bash
ezfadvanceIII_multirom_writer --yes-really-write rom1.gba ...
```

Behavior:

1. build image in memory;
2. initialize libusb;
3. open VID/PID `0E6A:5088`;
4. claim interface 0;
5. reproduce manager initialization/probe;
6. perform global write setup;
7. erase required flash geometry;
8. program image;
9. verify when a capture-supported mapping exists;
10. clean up USB state.

### 26.3 Write without readback verification

```bash
ezfadvanceIII_multirom_writer --yes-really-write --skip-verify rom1.gba ...
```

Programming remains unchanged. Only post-write read comparison is skipped.

### 26.4 Metadata overrides

For protocol experiments:

```text
--type1=N ... --type4=N
--map1=N  ... --map4=N
```

These are intentionally retained even though automatic classification is much more accurate now. They are useful for testing newly observed catalog values without recompiling the program.

---

## 27. Safety model

The writer contains several deliberate interlocks.

### 27.1 `--yes-really-write`

Without this option, no USB device is touched.

### 27.2 Cartridge readiness

`0x98` must return `01` before erase/program operations are allowed.

### 27.3 Non-SRAM confirmation

FLASH/EEPROM-family ROMs require an explicit `y/yes` after a warning.

### 27.4 Capacity checks

The packed image must fit in 32 MiB.

### 27.5 Loader overlap checks

Loader placement is rejected if it collides with meaningful ROM data.

### 27.6 Verification conservatism

Unknown read mappings are skipped, not guessed.

### 27.7 No automatic wipe

A full-card wipe is never launched automatically.

---

## 28. Preventive full-card wipe utility

A separate `ezfadvanceIII_wipe_card` utility exists for full-card erase testing.

Observed project behavior suggests that large writes can be more reliable after a complete wipe, especially after prior experimental states.

Important properties:

- full 32-MiB erase geometry;
- four 8-MiB windows;
- 135 erase operations per window;
- 540 erase operations total;
- destructive and always explicit;
- never run automatically by the writer.

The project historically recommends a fresh USB unplug/replug before some wipe experiments because the flash/bridge state can be influenced by prior sessions.

---

## 29. Save reader/writer companion work

The project also contains separate save-memory experiments/utilities.

Capture-derived save access uses a different command mode from ROM reads/writes.

Known behavior includes:

```text
save read  : opcode 0x91, sub/mode 0x01
save write : opcode 0x92, sub/mode 0x01
common read extent tested: 32 KiB
```

A selector around `0x0900` was observed in save-related traffic.

A Bios_Dumper save-reader test produced an exact 32-KiB hardware match.

Multi-ROM save selection remains a separate area of reverse engineering and should not be conflated with the ROM catalog `map` byte.

---

## 30. PCAP capture methodology

Recommended capture procedure:

1. physically unplug/replug the EZF Advance III USB bridge;
2. start USBPcap before launching EZ3Manager;
3. launch EZ3Manager;
4. add ROMs in a precisely recorded input order;
5. allow erase/program/verify to complete;
6. close the manager;
7. stop the capture;
8. test the resulting cartridge on real GBA hardware before changing it.

For ordering experiments, deliberately use scrambled ROM sizes so sorting behavior is visible.

For equal-size ordering experiments, record the exact insertion order.

---

## 31. USBPcap snap-length limitation

Several captures were recorded with a nominal snap length of 65,535 bytes.

A GBA program block is 65,536 bytes, and USBPcap metadata consumes part of the captured record budget. As a result, the final bytes of some 64-KiB bulk payloads are absent from the PCAP.

In the reverse-engineering work, this manifested as approximately 28 missing data bytes per affected transfer.

This matters because the loader can cross a 64-KiB transfer boundary.

One exact 28-byte loader region was recovered from a physical cartridge programmed by original EZ3Manager and is now explicitly restored in the embedded multi-loader template.

Therefore:

> A PCAP with truncated bulk payloads can still prove command geometry, addresses, catalog fields, ordering, loader position, and almost all image bytes, but byte-perfect template reconstruction may require cartridge readback or a higher-snaplen capture.

---

## 32. Major diagnostic failures that improved the design

### 32.1 False BANK1 verification failure

An earlier partial-window verifier programmed higher-window data successfully but tried to read it using an assumed local BANK1 read mapping.

The first byte returned after "selecting verification window 1" matched window 0, proving the assumption false.

Result:

- experimental local-window verifier removed;
- current verifier uses only capture-supported mappings.

### 32.2 MegaManZ failed only when placed second after Piano

An early input-order layout produced:

```text
Piano    @ 0x000000
MegaManZ @ 0x010000
```

The 8-MiB MegaManZ image then crossed the 8-MiB physical flash boundary by 64 KiB.

The chooser worked and Piano launched, but MegaManZ did not.

Original EZ3Manager was then captured with the same requested input order and was found to reorder/pack as:

```text
MegaManZ @ 0x000000
Piano    @ 0x7F0000
```

This established that image packing, not write corruption, was the problem.

### 32.3 4-MiB games initially failed despite successful verify

F-Zero + Mario Kart produced:

```text
WRITE + FULL READ-BACK VERIFICATION SUCCEEDED
chooser worked
both games failed to launch
```

The original-manager PCAP proved that the physical `0` / `4 MiB` placement was correct.

The actual bug was the old catalog classifier. Both games needed `type=3`, while map differed:

```text
F-Zero     type 3 / map 3
Mario Kart type 3 / map 6
```

Forcing those values with CLI overrides fixed both games on hardware.

This led to the generic size-derived `type` model in 0.5.5.

---

## 33. Hardware/capture validation matrix

The following combinations are important project milestones.

| Geometry | Capture status | Hardware status | Notes |
|---|---|---|---|
| 64 KiB + 8 MiB | yes | yes | compact trailing-FF reuse; 8-MiB image |
| 4 MiB + 4 MiB | yes | yes | validated after type/map correction |
| 8 MiB + 4 MiB + 4 MiB | yes | yes | automatic 0.5.5 metadata validated |
| 8 MiB + 8 MiB | partial/historical | yes | menu + both games launch |
| 16 MiB + 8 MiB + 8 MiB | yes | yes | full 32-MiB layout |
| 8 MiB + 8 MiB + 8 MiB + 8 MiB | yes | yes | four-ROM loader/catalog proven |
| single 8 MiB Advance Wars | yes | yes | internal/trailing loader placement |
| single 16 MiB FFTA | yes | yes | internal FF loader slot |
| single 16 MiB Fire Emblem | yes | yes | loader just above 16 MiB |
| single 32 MiB Kingdom Hearts | yes | yes | full-card behavior |
| single 16 MiB Tales / EEPROM_V124 | yes | pending writer save-cycle validation | map 5; loader 0xD49020 |

The table intentionally distinguishes "launches correctly" from "save persistence fully validated".

---

## 34. Key capture inventory

Important captures used during development include:

```text
initialize.pcap
writeromonemptycard.pcap
writerom128Mb.pcap
fireemblem.pcap
256MBits-rom.pcap
piano-bios.pcap
piano-megamanz.pcap
8_8_16MiB.pcap
8_8_8_8MiB.pcap
4MiB-4MiB.pcap
4_4_8MiB.pcap
TOF-EEPROM.pcap
readmultiromonesav.pcap
readsav.pcap
writesav.pcap
```

Each capture should be treated as a protocol fixture: it is more valuable when the original ROM input order, ROM hashes, original manager version, and real-console result are recorded alongside it.

---

## 35. Current save-signature / mapping support status

### 35.1 SRAM / SRAM_F

```text
classification: map 3
warning: no
launch support: hardware-proven in multiple titles
```

### 35.2 FLASH / FLASH512 markers

```text
embedded marker association: map 6
warning: yes
launch support: hardware-proven in multiple titles
save conversion: never automatic
```

FFTA is now a paired-control case: unpatched FFTA is `FLASH512_V130`, `type 1 / map 6`, loader `0xD97730`; the SRAM-patched ROM retains the same signature and earlier EZ3Manager evidence also produced `type 1 / map 6` with the same loader location. Direct ROM diffing shows 94 changed executable bytes that redirect save access to SRAM. The association therefore describes metadata/catalog behavior, not guaranteed runtime FLASH behavior.

### 35.3 FLASH1M marker

```text
embedded marker association: map 6
warning: yes
implementation: supported generically
capture validation: still desirable
```

This is not a runtime save-mode detector.

### 35.4 EEPROM marker

```text
embedded marker association: map 5
warning: yes
capture proof: EEPROM_V124 / Tales of Phantasia
ROM patching: none performed by writer or original manager
```

For the captured unpatched Tales ROM, `EEPROM_V124` and map 5 coincide with native EEPROM code. This does not yet prove what EZ3Manager would do with a separately SRAM-patched EEPROM ROM that still retained the marker.

A dedicated hardware save/load test with the current writer remains useful to validate the complete EEPROM save workflow.

### 35.5 No recognized save marker

The current fallback mapping is 3.

This is appropriate for observed non-FLASH cases but should not be interpreted as a universal database of every homebrew/custom save implementation.

---

## 36. Version evolution

### 36.1 0.5.1

Introduced an experimental window-aware verifier for partial higher flash windows.

Hardware disproved its read-window assumption. This version is historically important because its failure established that program-window selection and readback mapping are different state machines.

### 36.2 0.5.2

Reworked multi-ROM packing using original-manager evidence:

- stable descending file-size ordering;
- equal-size stability;
- size-class placement;
- reuse of trailing `FF` padding;
- internal FF loader placement;
- removal of the incorrect experimental verifier.

### 36.3 0.5.3

Added:

```text
--skip-verify
```

Erase/program behavior remains unchanged when verification is skipped.

### 36.4 0.5.4

Added:

- capture-proven four-ROM support;
- four-ROM loader zero-tail variant;
- no intermediate BIN output;
- retained dry-run safety mode.

### 36.5 0.5.5

Corrected catalog semantics:

- `type` changed from save-signature guesses to ROM size class;
- FLASH-family embedded-signature handling generalized to map 6; later SRAM-patched FFTA evidence showed this must not be described as a definitive runtime FLASH mode.

This release fixed the 4-MiB launch-classification problem generically.

### 36.6 0.5.6

Added per-ROM non-SRAM save-format warnings.

### 36.7 0.5.7

Made non-SRAM warnings interactive:

```text
Continue anyway? [y/N]
```

### 36.8 0.5.8

Added EEPROM support from `TOF-EEPROM.pcap`:

- `EEPROM_V... -> map 5`;
- removed EEPROM hard rejection;
- reproduced Tales loader placement at `0xD49020`;
- confirmed no automatic EEPROM ROM patch in original manager traffic.

### 36.9 0.5.9

Text-only clarification:

- explicitly states that the writer never patches save routines;
- SRAM conversion is a separate manual user action.

No protocol/image behavior changed from 0.5.8.

### 36.10 0.5.10

Unix-like portability release; EZ3 protocol/image behavior is unchanged.

Changes include:

- libusb header detection accepts either `<libusb-1.0/libusb.h>` or `<libusb.h>`;
- Linux kernel-driver auto-detach is isolated to the Linux build path;
- `std::filesystem` is no longer required for catalog-name derivation;
- host reporting recognizes macOS, Linux, FreeBSD, OpenBSD, NetBSD, and DragonFly BSD;
- native Windows builds are intentionally out of scope; Windows users should use a Linux VM with USB passthrough;
- save-warning wording explicitly describes the marker as an embedded save-library signature and notes that stale FLASH/EEPROM signatures may survive SRAM patching.

The 0.5.10 source has been compiled successfully on macOS / Apple Silicon with Homebrew libusb. Linux and BSD compile/hardware validation remain pending.

---

## 37. Build environment and native platform scope

The current source targets C++17 and libusb on Unix-like systems.

Native project scope:

```text
macOS       supported target; 0.5.10 compilation verified
Linux       supported target; validation pending
FreeBSD     supported target; validation pending
OpenBSD     supported target; validation pending
NetBSD      supported target; validation pending
DragonFly   supported target; validation pending
Windows     no native mainline support; use a Linux VM with USB passthrough
```

Typical Apple Silicon/Homebrew build command:

```bash
c++ -std=c++17 -O2 \
  ezfadvanceIII_multirom_writer_0.5.10.cpp \
  -I/opt/homebrew/opt/libusb/include \
  -L/opt/homebrew/opt/libusb/lib \
  -lusb-1.0 \
  -o ezfadvanceIII_multirom_writer
```

On systems where libusb publishes a `pkg-config` file, the intended portable form is:

```bash
c++ -std=c++17 -O2 \
  ezfadvanceIII_multirom_writer_0.5.10.cpp \
  $(pkg-config --cflags --libs libusb-1.0) \
  -o ezfadvanceIII_multirom_writer
```

The exact BSD linker/include flags may vary with the base system or package installation and should be documented only after each target is compiled successfully.

Windows XP `usbscan.sys` experiments remain useful historical reverse-engineering material, but they are not maintained as a native backend for the current writer. Windows users are expected to pass VID `0x0E6A` / PID `0x5088` through to a Linux VM and run the Linux build there.

Warnings-enabled development builds are recommended while modifying protocol code.

---

## 38. Recommended test procedure after code changes

### 38.1 Before USB write

Run dry mode first:

```bash
ezfadvanceIII_multirom_writer rom1.gba rom2.gba ...
```

Check:

- sorted order;
- physical starts;
- ROM size classes;
- catalog type values;
- map values;
- loader start;
- final image extent;
- expected verification policy.

### 38.2 Before destructive tests

Record:

- source version/hash;
- ROM filenames;
- ROM SHA-256 hashes;
- input order;
- expected sorted order;
- whether card was wiped first;
- whether `--skip-verify` was used.

### 38.3 After programming

Test on real GBA:

1. chooser appears;
2. every menu entry launches;
3. game executes beyond initial boot;
4. for save-related tests, create a save;
5. power cycle;
6. verify save reload;
7. if relevant, dump save and compare it byte-for-byte.

A successful USB verify does **not** prove launch correctness. The 4-MiB classification failure demonstrated this clearly.

---

## 39. Capture-derived regression fixtures

The following cases are especially useful as byte/address-level regression fixtures for future changes.

### 39.1 MegaManZ + Piano

```text
Input order tested against original manager:
    Piano       64 KiB
    MegaManZ     8 MiB

Final catalog/physical order:
    #1 MegaManZ
    #2 Piano

MegaManZ start = 0x000000
Piano start    = 0x7F0000
Loader start   = 0x2CC420
Image end      = 0x800000
Program blocks = 128 x 0x10000
Verify blocks  = 128 x 0x10000
```

The patched first ARM instruction in this configuration was observed as:

```text
06 31 0B EA
```

This fixture proves compact packing entirely inside the first 8-MiB window.

### 39.2 Fire Emblem + Advance Wars + MegaManZ

```text
Requested order:
    Advance Wars   8 MiB
    MegaManZ       8 MiB
    Fire Emblem   16 MiB

Final order:
    #1 Fire Emblem
    #2 Advance Wars
    #3 MegaManZ

Fire Emblem start   = 0x0000000
Advance Wars start  = 0x1000000
MegaManZ start      = 0x1800000
Loader start        = 0x15C2360
Image end           = 0x2000000
Program blocks      = 512
Verify blocks       = 512
```

The loader's relative position inside the Advance Wars slot is:

```text
0x15C2360 - 0x1000000 = 0x5C2360
```

This fixture proves stable sorting and loader reuse inside a later ROM slot.

### 39.3 Four 8-MiB ROMs

The tested order was preserved because all entries had equal size.

```text
#1 Advance Wars @ 0x0000000
#2 Advance Wars @ 0x0800000
#3 MegaManZ     @ 0x1000000
#4 MegaManZ     @ 0x1800000
Loader          @ 0x05C2360
Image end       @ 0x2000000
Program blocks  = 512
Verify blocks   = 512
```

Catalog packed fields:

```text
#1 start 0x0000000 map 6 -> 0x00000006
#2 start 0x0800000 map 6 -> 0x40000006
#3 start 0x1000000 map 3 -> 0x80000003
#4 start 0x1800000 map 3 -> 0xC0000003
```

This fixture proves four active entries and the four-ROM zero-tail loader form.

### 39.4 F-Zero + Mario Kart

```text
F-Zero
    size   = 4 MiB
    start  = 0x000000
    type   = 3
    map    = 3

Mario Kart
    size   = 4 MiB
    start  = 0x400000
    type   = 3
    map    = 6

Loader start   = 0x5E9C30
Image end      = 0x800000
Program blocks = 128
Verify blocks  = 128
```

The initial custom 0.5.4 automatic classifier produced the correct geometry but wrong metadata. Manually forcing `type3/map3` and `type3/map6` made both games launch, proving the geometry independently from the classifier.

### 39.5 Advance Wars 2 + Mario Kart + F-Zero

```text
#1 Advance Wars 2 @ 0x0000000  type 2 / map 6
#2 Mario Kart     @ 0x0800000  type 3 / map 6
#3 F-Zero         @ 0x0C00000  type 3 / map 3

Loader start   = 0x0617EF0
Image end      = 0x1000000
Program blocks = 256
Verify blocks  = 256
```

This fixture is one of the strongest validations of the current generic classifier because the same 0.5.5 automatic rules subsequently produced a hardware-working cartridge with all three games launching.

### 39.6 Tales of Phantasia / EEPROM

```text
ROM size       = 0x1000000 = 16 MiB
Signature      = EEPROM_V124
ROM start      = 0x0000000
Catalog type   = 1
Catalog map    = 5
Loader start   = 0x0D49020
Image end      = 0x1000000
```

The single loader template relocated to `0xD49020` matched all 1,632 loader bytes visible in the capture.

Across the captured ROM payload, outside the expected first-entry branch patch and loader insertion, no EEPROM-specific patch was observed. This agrees with the user's clarification that save-format patching in the original Windows workflow is performed manually with a separate tool, not by EZ3Manager itself.

### 39.7 Exact block-count sanity table

| Image extent | 64-KiB blocks | Typical captured use |
|---:|---:|---|
| 8 MiB | 128 | MegaManZ+Piano, F-Zero+Mario Kart |
| 16 MiB | 256 | 8+4+4, ordinary full 16-MiB image |
| 24 MiB | 384 | program geometry supported; generic verify mapping still open |
| 32 MiB | 512 | 16+8+8, 8+8+8+8, full-card image |

The relation is simply:

```text
block_count = image_bytes / 0x10000
```

for exact block-aligned extents.

---

## 40. Open questions

### 39.1 More than four ROMs

The loader appears to contain many unused entry slots, but only 1-4 entries are currently capture/hardware supported.

A five-ROM PCAP is required before increasing `MAX_ROMS`.

### 39.2 Partial 16-32 MiB readback mapping

Programming is known across all four windows, but the original-manager verify state for arbitrary partial higher-window images is still unknown.

A 24-MiB original-manager capture is especially valuable.

### 39.3 FLASH1M

The writer maps `FLASH1M_V...` to 6, consistent with the FLASH family, but a dedicated original-manager capture and save test would strengthen this assumption.

### 39.4 EEPROM universality

`EEPROM_V124 -> map 5` is capture-proven. It is plausible that all `EEPROM_Vxxx` revisions and both EEPROM capacities use map 5, but that remains a family-level inference until more captures are collected.

### 39.5 Save-bank behavior in multi-ROM mode

ROM catalog mapping is understood much better than multi-ROM save selection. The exact way the original manager associates multiple save regions with menu entries remains a separate research area.

### 39.6 Cartridge density probing

Four probe windows imply the tested 32-MiB geometry, but the project does not yet contain a formal density-identification algorithm for arbitrary EZ3 cartridge variants.

---

## 41. Design principles for future changes

1. **Prefer PCAP evidence over intuition.**
2. **Prefer real-console behavior over successful USB verification.**
3. **Do not special-case game titles when a structural rule exists.**
4. **Keep ROM-size classification separate from EZ3 map/configuration and from actual runtime save behavior.**
5. **Never silently patch save routines.**
6. **Never invent a read-window mapping.**
7. **Reject meaningful-data overlap.**
8. **Keep destructive operations behind explicit user intent.**
9. **Preserve capture-derived timing unless a controlled experiment disproves its necessity.**
10. **Treat every newly recovered capture as a regression fixture.**

---

## 42. Short architectural summary

The current project can be summarized as the following pipeline:

```text
ROM files
   |
   +--> identify save-library family
   |       SRAM/other -> map 3
   |       EEPROM     -> map 5
   |       FLASH      -> map 6
   |
   +--> derive ROM size class -> catalog type 0..9
   |
   +--> warn/confirm non-SRAM save formats
   |
   +--> stable sort largest-first
   |
   +--> size-class placement using meaningful non-FF extents
   |
   +--> reuse safe trailing/internal FF space
   |
   +--> choose and relocate EZ3 loader
   |
   +--> generate 28-byte catalog entries
   |
   +--> patch physical ROM #1 branch to loader
   |
   +--> construct complete card image in memory
   |
   +--> dry run OR --yes-really-write
             |
             +--> bridge startup/readiness
             +--> manager-compatible flash probe
             +--> capture-derived erase
             +--> four-window 64-KiB programming
             +--> capture-supported verification
             +--> final status/reset cleanup
```

The project is therefore not merely a USB flasher. It is a reconstruction of the **EZ3Manager image-building rules and flash state machine** required for those images to boot correctly on real GBA hardware.

---

## 43. Current project status

At `0.5.10`, the project has moved from title-specific experimentation to a mostly structural model:

- cartridge geometry is understood as four 8-MiB program/erase windows;
- 1-4 ROM menu images are supported;
- ROM ordering is stable largest-first;
- ROM placement is size-class based and can reuse erased padding;
- single and multi loader templates are capture-derived and relocatable;
- catalog type is ROM-size based;
- catalog map uses capture-derived metadata associations (`3`, `5`, `6` in current evidence) and is not treated as a definitive runtime save-mode field;
- exact 8-, 16-, and 32-MiB verification paths are known;
- unsafe partial-window verification guesses have been removed;
- non-SRAM save-library signatures are visible to the user; paired FFTA evidence now proves that a manual SRAM patch can redirect runtime save code while leaving `FLASH512_V130` and EZ3 `map 6` unchanged;
- native code scope is macOS/Linux/BSD via libusb; native Windows is intentionally out of scope and Windows users should use a Linux VM with USB passthrough;
- multiple mixed-size configurations have been verified on original GBA hardware.

The remaining work is concentrated in **expanding evidence coverage**, not redesigning the core architecture.

