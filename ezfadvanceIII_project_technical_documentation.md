# EZF Advance III Reverse-Engineering Project

**Technical architecture, protocol, image-format, and validation documentation**  
**Current project/toolset version:** `0.10.0`<br>
**Current writer implementation:** `ezfadvanceIII_multirom_writer 0.10.0`<br>
**Version-synchronized utilities:** `ezfadvanceIII_multirom_writer`, `ezfadvanceIII_card_reader`, `ezfadvanceIII_save_reader`, `ezfadvanceIII_wipe_card`<br>
**Target hardware:** EZ-Flash Advance III / EZF Advance III, 256 Mbit (32 MiB) GBA flash cartridge<br>
**Host implementation:** object-oriented C++17 + libusb; native project scope is macOS, Linux, and BSD. The current shared 0.10.0 toolset has been compiled and transcript-tested on macOS / Apple Silicon. Linux CI compiles and runs the offline suite. The 0.9.0 extracted libusb writer backend was specifically hardware-requalified with the two-ROM 8-MiB F-Zero/Mario Kart case: exact-8-MiB full read-back verification succeeded, the EZ3 menu booted, and both games launched on a real GBA. The partial-first-window, partial 12-/20-/28-MiB, exact 16-/24-/32-MiB, and tiny-tail paths retain their earlier hardware qualification. The new 0.10.0 save-write path is capture-derived and transcript-tested but awaits hardware qualification. Official-cartridge detection, header confirmation, the guarded full scan, the correct 8-MiB Golden Sun trim, the trusted SHA-256 match, and extracted-file boot are hardware-confirmed. EZ3 ROM 1 and ROM 2 extraction from a two-ROM layout are hardware-confirmed by SHA-256 equality. The 1-MiB official extraction extent is unit-tested but awaits a physical-cartridge dump/hash comparison. Linux physical-USB and BSD build/hardware validation remain pending.

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

### 1.1 Shared project versioning

Beginning with **0.6.0**, every mainline utility uses one shared project version:

```text
ezfadvanceIII_multirom_writer
ezfadvanceIII_card_reader
ezfadvanceIII_save_reader
ezfadvanceIII_wipe_card
```

The version is bumped when the code of **at least one** of these programs changes. All four programs then carry the same new project version, even when some individual utilities have no functional code changes in that release.

Therefore the version number identifies a synchronized EZF Advance III toolset release, not a per-file change counter.

Beginning with **0.6.2**, the project version is **not hard-coded into normal
runtime banners**. From 0.7.29, standalone `--version` reads a single shared
constant and reports it without USB access. Normal banners continue to identify
the utility and host platform only.

The GBA header field reported as `ROM version` is unrelated to the toolset release number and remains displayed where applicable.

### 1.2 Current software architecture

The four executables retain their original top-level source filenames, but
shared behavior is implemented as composable C++ objects under `include/` and
`src/`:

```text
command-line entry points
        |
application services
        |
cartridge/image domain objects
        |
EZ3 protocol and read state machines
        |
abstract USB transport
        |
RAII libusb device/session ownership
```

The principal shared components are:

- `UsbDevice`, which owns the libusb context, handle, and claimed interface;
- `Transport` and `BulkTransport`, which isolate USB bulk transfers and permit recorded test transports;
- `Protocol`, which implements shared `0x92` command/data/echo transactions;
- `ReadOnlyCartridge`, which owns capture-derived initialization, probing, ROM reads, and proven linear read mappings;
- `CartridgeFormat`, `GbaHeader`, and `CatalogEntry`, which model and validate binary cartridge metadata;
- `Ez3CatalogParser`, which interprets shared loader/catalog structure without
  authorizing either reader's evidence policy;
- `SaveMemoryReader`, which owns capture-proven save-bank reads;
- `VerificationPolicy`, which selects only capture-supported verification geometries;
- `VerificationSession`, which owns the transport-injectable partial
  first-window, explicit partial 12-/20-/28-MiB, exact 8-/16-/24-/32-MiB, and
  tiny-tail verification transcripts;
- `WriterOptions`, which separates command-line parsing from image and device operations.
- `LibusbWriterBackend`, which contains the capture-derived destructive USB
  implementation behind the `WriterBackend` factory boundary.

The application workflows are represented by `CartridgeImageBuilder`,
`CardWriter`, `CardInspector`, `SaveExtractor`, and `CardEraser`. Composition is
used instead of a shared executable base class because the four programs share
dependencies and protocol primitives, not one common program behavior.

The normative software contract, safety requirements, and acceptance tests are
specified in [`SOFTWARE_SPECIFICATION.md`](SOFTWARE_SPECIFICATION.md).

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
- `map=4` for the captured Classic NES `EEPROM_V124` cases;
- `map=5` for Tales of Phantasia / `EEPROM_V124`;
- catalog counts through eight active entries.

### 2.2 Hardware-proven

The behavior was reproduced by the custom writer and then successfully exercised on a physical original GBA console.

Examples:

- 4 + 4 MiB after correcting catalog metadata;
- 8 + 4 + 4 MiB using automatic size/map classification;
- 1-MiB Classic NES map-4 single-ROM writes and launches;
- 4 MiB + 1 MiB and 4 MiB + 1 MiB + 1 MiB mixed map-3/map-4 menus;
- 6-ROM 24-MiB and 32-MiB writes, verification, menu selection, and launches.

### 2.3 Capture + hardware proven

Both conditions are satisfied. These are the strongest project facts.

### 2.4 Inferred

A rule is consistent with all observed captures but has not yet been isolated by a dedicated capture.

Examples include an isolated single 2-MiB ROM size class, smaller classes such as 512 KiB, and the generic ROM-level discriminator that makes EZ3Manager choose EEPROM map 4 versus map 5.

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
PROGRAM_BLOCK       = 0x00010000  = 64 KiB
FLASH_WINDOW_SIZE   = 0x00800000  = 8 MiB
CARD_HALF_SIZE      = 0x01000000  = 16 MiB
CARD_192MBIT_SIZE   = 0x01800000  = 24 MiB
MAX_CARD_IMAGE      = 0x02000000  = 32 MiB
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

The `0x98` response is treated as the cartridge presence/readiness signal. A
transient `00` is retried up to five checks with 100-ms intervals:

```text
01 -> ready
00 -> retry, then not ready after five checks
other -> unexpected state and immediate failure
```

If the response is missing, unexpected, or remains `00`, the custom writer
aborts **before erase or programming**.

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

### 11.1 Partial first-window verification below 8 MiB

Three independent captures establish a generic lower-window rule:

- a single 1-MiB Classic NES ROM;
- two 1-MiB ROMs with roughly 2 MiB of ROM payload;
- a single 4-MiB F-Zero ROM.

After programming, EZ3Manager sends only:

```text
FFFF
04
00
00
```

and then begins ordinary linear `0x91` reads. It does **not** send a guessed `0x0020` or the exact-8-MiB `0x0040` transition.

For these partial first-window images:

```text
program extent = align_up(max(0x10000, image_size), 0x100)
verify extent  = align_up(programmed_image_size, 0x10000)
```

Bytes in the final verification block beyond the programmed image are expected to remain `FF`.

The short gaps visible between status cleanup and the first `0x91` read differ between captures in 15.625-ms USBPcap timestamp quanta, so the current writer does not impose a fixed synthetic delay there.

Two independent exactly 2-MiB single-ROM sources additionally close the
2-MiB source-ROM hardware checkpoint. In both runs:

```text
source ROM size          0x00200000
loader start             0x00200000
constructed image        0x00200700
verification extent      0x00210000
verification blocks      33
final block start        0x00200000
```

The final block verifies the `0x700` loader fragment followed by expected
erased `FF` padding. Both runs completed full readback and booted on a real
GBA. This proves exactly 2-MiB source-ROM handling; it is not an exact-2-MiB
constructed-image test and does not qualify physical official-cartridge
extraction at 2 MiB.

### 11.2 Exact 8-MiB verify transition

An exact 8-MiB / 64-Mbit image uses a distinct capture-proven transition containing selector `0x0040` before linear readback. This is independently visible in the single 8-MiB Advance Wars capture and `4MiB-4MiB.pcap`.

### 11.3 Exact 16-MiB verify transition

The original manager enters a separate linear-read mapping involving selector `0x0080`, status/reset operations, a 125-ms settle interval, and a final read prefix before sequential `0x91` reads.

### 11.4 Exact 24-MiB verify transition

`4_4_4_4_8MB.pcap` proves the 192-Mbit / 24-MiB transition:

```text
status
55AA
0200
00C0
0000
quiet interval (~0.11-0.125 s)
AA55
0000
0000
0000
AA
55
06
status
55AA
0000
0000
0000
```

It then issues 384 consecutive 64-KiB linear `0x91` reads covering byte `0x0000000` through `0x017FFFFF`. All captured verify payloads matched the programmed payloads.

The custom writer also completed a real-hardware 6-ROM / 24-MiB write, full verification, menu selection, and launch test successfully.

### 11.5 Exact 32-MiB verify transition

A separate capture-derived transition is used after programming BANK3. It prepares a full 256-Mbit linear read mapping, after which the writer verifies all 512 x 64-KiB blocks. Independent 6-, 7-, and 8-ROM captures reproduce this same sequence.

Real-hardware full-card tests include a 6-ROM / 32-MiB image, a single 32-MiB
Kingdom Hearts image, and a 16+8+8-MiB three-ROM image. Each completed all 512
64-KiB reads through `0x01ff0000`. The single image booted, while both
multi-ROM menus and every tested entry launched successfully on a real GBA.

### 11.6 Current verification boundary

The current evidence-backed policy is:

```text
0 < image < 8 MiB          full read-back verification
image == 8 MiB             full read-back verification
image == 12 MiB            capture/transcript/hardware-proven
other 8 MiB < image < 16 MiB verification skipped
image == 16 MiB            full read-back verification
captured tiny >16-MiB tail full read-back verification
image == 20 MiB            capture/transcript/hardware-proven
other 16 MiB < image < 24 MiB verification skipped
image == 24 MiB            full read-back verification
image == 28 MiB            capture/transcript/hardware-proven
other 24 MiB < image < 32 MiB verification skipped
image == 32 MiB            full read-back verification
```

The successful ~14-MiB and ~22-MiB hardware tests prove packing/programming/menu/launch behavior for those layouts, but they do not establish the original-manager linear-read selector for the corresponding partial higher-window geometry. The writer therefore continues to skip full verification there rather than extrapolate from the exact 16-/24-/32-MiB cases.

### 11.7 Other partial higher-window images

Programming remains supported across all four physical windows, but arbitrary partial extents in the unproven higher-window ranges are not assumed to share neighboring exact-size mappings. Unsupported geometries are programmed normally and verification is skipped rather than guessed.

There is also a capture-specific Fire Emblem path for a very small loader tail immediately beyond 16 MiB.

### 11.8 `--skip-verify`

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

The first physical/catalog ROM is special because cartridge execution begins at GBA ROM address `0x08000000`, which corresponds to card byte offset `0x00000000`.

Original EZ3Manager does not place the EZ3 loader at byte zero. Instead, it preserves the first ROM at the beginning of the cartridge and rewrites **only that ROM's first 32-bit ARM instruction** so execution branches into the relocated EZ3 loader/menu.

The original first instruction is not discarded conceptually: its original branch destination is decoded before patching and stored in the first catalog entry so the loader can later transfer control back to the ROM's normal startup path.

### 13.1 Required original instruction form

The capture-supported ROMs begin with an ordinary unconditional ARM `B` instruction:

```text
EAxxxxxx
```

In little-endian ROM byte order, the four bytes appear with the immediate field first and `EA` last.

For an ARM branch instruction at cartridge offset zero, ARM's PC value is eight bytes ahead of the current instruction. The branch destination can therefore be expressed in card-relative form as:

```text
target = 0x00000008 + sign_extend(imm24) * 4
```

where:

```text
instruction = 0xEA000000 | (imm24 & 0x00FFFFFF)
```

The writer deliberately requires this captured `EAxxxxxx` form. If the ROM does not begin with such an instruction, it refuses the ROM rather than inventing a different entry-point patching rule.

The same calculation can be expressed using CPU-visible GBA ROM addresses:

```text
instruction address = 0x08000000
ARM PC               = 0x08000008
CPU target           = 0x08000000 + card_relative_target
```

The project normally keeps the branch calculation in card-relative offsets because the common `0x08000000` base cancels out.

### 13.2 What is saved before ROM #1 is patched

Before image construction, the writer decodes ROM #1's original branch target and records it as:

```text
original_entry_target
```

For the **first** catalog entry, bytes `24..27` contain this original branch destination.

For later catalog entries, bytes `24..27` instead contain that ROM's physical byte start on the cartridge.

Conceptually:

```text
catalog entry #1:
    field +24 = original branch target of ROM #1

catalog entry #2 and later:
    field +24 = physical card byte start of that ROM
```

This asymmetry exists because ROM #1 has had its normal first instruction replaced by the loader branch and therefore needs its original startup destination preserved separately.

### 13.3 Only the first physical/catalog ROM is patched

Only the ROM occupying card byte `0x00000000` is modified.

Later ROMs retain their original first four bytes exactly. They are reached through the EZ3 loader/menu using their catalog metadata instead of by changing their own reset instruction.

This also means that **sorting happens before the branch patch is applied**. The writer stable-sorts ROMs largest-first, performs placement, determines which ROM is physical/catalog ROM #1, then patches byte zero of the completed image.

The `--mapN` and other per-input metadata stay associated with their ROM object while it is reordered, but the ARM branch patch always belongs to whichever ROM ends up physically first.

### 13.4 Detailed captured example — Classic NES Series: Super Mario Bros.

The single-ROM capture provides a complete byte-level example.

The ROM's original branch destination is:

```text
card-relative target = 0x000000CC
CPU-visible target   = 0x080000CC
```

At cartridge offset zero, ARM evaluates the PC as:

```text
PC = 0x00000008
```

Therefore the original immediate is:

```text
imm24 = (0x000000CC - 0x00000008) / 4
      = 0x000000C4 / 4
      = 0x00000031
```

The original first instruction is consequently:

```text
ARM word        = 0xEA000031
little-endian   = 31 00 00 EA
```

EZ3Manager places the single-ROM loader at:

```text
loader card offset = 0x00100010
loader CPU address = 0x08100010
```

To redirect initial execution to that loader, the replacement branch immediate is:

```text
imm24 = (0x00100010 - 0x00000008) / 4
      = 0x00100008 / 4
      = 0x00040002
```

which produces:

```text
patched ARM word  = 0xEA040002
patched ROM bytes = 02 00 04 EA
```

Those are exactly the first four programmed bytes observed in the capture:

```text
02 00 04 EA
```

The first catalog entry simultaneously preserves the original target:

```text
catalog +24..+27 = CC 00 00 00
                   -> 0x000000CC
```

The startup chain is therefore:

```text
GBA reset / cartridge entry
        |
        v
0x08000000
patched instruction: B 0x08100010
        |
        v
EZ3 single-ROM loader
        |
        | loader/menu performs its required setup
        v
original ROM startup target
0x080000CC
```

The ROM's normal startup destination is therefore recoverable even though its original first instruction was overwritten on the flash image.

### 13.5 Multi-ROM confirmation of the same rule

The two-1-MiB capture shows the identical mechanism with the loader relocated farther into the image.

For that layout:

```text
ROM #1 start = 0x00000000
ROM #2 start = 0x00100000
loader start = 0x00200010
```

ROM #1 is patched to branch to `0x00200010`:

```text
imm24 = (0x00200010 - 8) / 4
      = 0x00080002

ARM word       = 0xEA080002
captured bytes = 02 00 08 EA
```

The capture contains exactly:

```text
02 00 08 EA
```

at card byte zero.

ROM #2 is **not** patched. Its own original first instruction remains in place at card byte `0x00100000`, and its catalog entry identifies `0x00100000` as its physical start.

This is why the project describes the rule as **first-ROM branch patching**, not general ROM entry-point patching.

### 13.6 GBA ROM mapping base

The GBA ROM CPU mapping base used by the loader and its relocated absolute literals is:

```text
GBA_ROM_BASE = 0x08000000
```

For branch encoding at byte zero, using card-relative targets and `PC = 8` is equivalent to using CPU addresses and `PC = 0x08000008`.

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

The original manager changes a small part of the serialized loader form based on ROM count, but the underlying multi-ROM loader template remains the same. Captures with 2, 3, 4, 5, 6, 7, and 8 active entries all use the same `0x7080` loader architecture and the same 125 relocation sites.

### 16.1 Two-ROM form

Capture-derived behavior:

```text
programmed loader extent = 0x6F80
not full 0x7080
```

The region from `0x6F26` to `0x6F7F` is left erased (`FF`).

The unused third catalog slot is encoded with a specific sentinel:

```text
byte 0       = 00
bytes 1..15  = ASCII spaces
bytes 16..27 = 00
```

### 16.2 Three-ROM form

Uses the full:

```text
0x7080-byte multi loader
```

The final 26 bytes remain `FF` in the captured three-ROM form.

### 16.3 Four or more ROMs

Independent 4-, 5-, 6-, 7-, and 8-ROM captures prove the same full `0x7080` loader and the same tail variant:

```text
loader offsets 0x7066..0x707F = 00
length = 0x1A = 26 bytes
```

The existing `kMultiTemplateB64` remains byte-for-byte valid after normal relocation and catalog patching. No new loader blob is required as entry count increases through the currently captured range.

### 16.4 Catalog capacity versus proven menu count

The repeated 28-byte catalog-slot region runs from loader offset `0x476E` to `0x548E`, which structurally contains:

```text
120 catalog slots
```

This is a **binary-structure safety bound**, not a claim that a 120-ROM menu has been proven on hardware.

The writer therefore no longer imposes a small `MAX_ROMS` behavioral limit. Instead it checks:

```text
total input ROM bytes <= 32 MiB / 256 Mbit
packed image + loader <= 32 MiB
number of entries <= 120 structural catalog slots
```

USB captures now prove the original manager serializes at least **8 active catalog entries** using the same format.

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

The current captures show a strong association between embedded Nintendo/SDK save-library metadata and the map value selected by EZ3Manager, but EEPROM proves that the ASCII library revision alone is insufficient:

```text
SRAM / SRAM_F / ordinary non-FLASH metadata -> map 3
Classic NES EEPROM_V124 captures            -> map 4
Tales of Phantasia EEPROM_V124              -> map 5
FLASH-family metadata                       -> map 6
```

The same `EEPROM_V124` revision therefore appears with **both map 4 and map 5**. The writer must not derive EEPROM map solely from `EEPROM_Vnnn`, ROM title, or ROM size.

Therefore `map 3`, `map 4`, `map 5`, and `map 6` are treated as capture-derived EZ3 catalog mapping/configuration values, not definitive runtime save-mode declarations.

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

### 19.3 Captured map-4 EEPROM examples

The small Classic NES captures prove:

```text
Classic NES Series - Super Mario Bros.
ROM size        = 1 MiB
embedded marker = EEPROM_V124
catalog type    = 5
mapping flag    = 4

Classic NES Series - Castlevania
ROM size        = 1 MiB
embedded marker = EEPROM_V124
catalog type    = 5
mapping flag    = 4
```

Both titles were hardware-tested successfully with explicit `--mapN=4`. A two-ROM `map 4 + map 4` image also wrote, fully verified, displayed the menu, and launched both titles successfully.

Controlled A/B hardware tests now show that map 4 is not merely the value used by EZ3Manager; it is functionally required for these two ROMs:

```text
Classic NES Series - Super Mario Bros.

map 4:
  write                         PASS
  full read-back verification  PASS
  launch/runtime               PASS

map 5:
  write                         PASS
  full read-back verification  PASS
  launch reaches game code     PASS
  runtime                      FAIL
```

The map-5 failure screen is:

```text
black background
centered white rectangular border

GAME PACK ERROR
TURN THE POWER OFF.
```

`GAME PACK ERROR` is displayed centered in red. `TURN THE POWER OFF.` is centered below it inside the same white rectangle.

The same controlled test was repeated with `Classic NES Series - Castlevania`. It produces the **same error message with the same presentation** when forced to map 5, while map 4 works normally:

```text
Classic NES Series - Castlevania

map 4:
  write                         PASS
  full read-back verification  PASS
  launch/runtime               PASS

map 5:
  write                         PASS
  full read-back verification  PASS
  launch reaches game code     PASS
  runtime                      FAIL
  identical GAME PACK ERROR / TURN THE POWER OFF. screen
```

This is strong hardware evidence that full USB/flash verification does **not** validate catalog-map correctness. A wrong map can produce a byte-perfect programmed image that is rejected later by the running game.

It also shows that map 4 and map 5 are not interchangeable EEPROM metadata values. They affect the runtime cartridge configuration seen by the game.

Mixed hardware-proven configurations include:

```text
4 MiB F-Zero map 3 + 1 MiB Super Mario map 4
4 MiB F-Zero map 3 + 1 MiB Super Mario map 4 + 1 MiB Castlevania map 4
```

### 19.4 Captured map-5 EEPROM example

`TOF-EEPROM.pcap` proves:

```text
Tales of Phantasia
ROM size        = 16 MiB
embedded marker = EEPROM_V124
catalog type    = 1
mapping flag    = 5
```

Because `EEPROM_V124` now exists in both map-4 and map-5 captures, the writer does **not** auto-select an EEPROM map. It requires:

```text
--mapN=4
or
--mapN=5
```

until a generic ROM-level discriminator is recovered.

### 19.5 Embedded signature versus active save implementation

Save patching is external to EZ3Manager. A dedicated patching tool may rewrite calls or routines so the game uses SRAM while leaving the original save-library signature in the ROM.

Static string detection answers:

> "Which save-library marker is still embedded in this ROM?"

It does **not** necessarily answer:

> "Which save technology will this patched ROM use at runtime?"

A true runtime classifier would require understanding the patch transformation itself, following save-library call sites, or detecting the replacement SRAM code path. That is outside the current writer.

### 19.6 FFTA SRAM patch byte-level evidence

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

For small/partial first-window images, the newer captures show:

```text
minimum program extent = 0x10000
programmed_size = align_up(max(0x10000, image.size()), 0x100)
verification_size (<8 MiB) = align_up(programmed_size, 0x10000)
```

The writer pads with `FF` as needed. This reproduces the captured 1-MiB and two-1-MiB cases.

---

## 26. Current CLI behavior

### 26.1 Dry run

```bash
ezfadvanceIII_multirom_writer rom1.gba [rom2.gba ...]
```

Behavior:

- loads and analyzes ROMs;
- displays non-SRAM warnings and requires confirmations when applicable;
- derives catalog type and non-EEPROM map values unless overridden;
- requires explicit `--mapN=4` or `--mapN=5` for EEPROM-signature ROMs;
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

### 26.3 Default progress display and `--verbose`

During destructive writes, the default UI uses in-place real-time progress bars for erase, program, and verification, reporting percentage, completed amount, elapsed time, average throughput where meaningful, and ETA.

Passing:

```bash
--verbose
```

restores detailed per-operation diagnostics, including lines such as:

```text
program card byte 0x00110000 local byte 0x110000 length 0x10000
    single data request: 1.129 s, 56.7 KiB/s
```

as well as per-sector erase and per-block verification messages.

### 26.4 Write without readback verification

```bash
ezfadvanceIII_multirom_writer --yes-really-write --skip-verify rom1.gba ...
```

Programming remains unchanged. Only post-write read comparison is skipped.

### 26.5 Metadata overrides

For protocol experiments:

```text
--typeN=VALUE
--mapN=VALUE
```

Multi-digit slot numbers are supported. Overrides remain useful for protocol experiments and are currently required for EEPROM ROMs until the generic map-4/map-5 discriminator is understood.

### 26.6 Official GBA cartridge inspection and extraction

The read-only card reader now preserves the four bytes returned by the
classification-relevant `0x91/sub2` probes. It explicitly distinguishes known
EZ3 flash-ID behavior, unchanged official-ROM behavior, and unknown changed
behavior. Known EZ3 IDs continue through the existing loader/catalog path. An
official cartridge stops before the later EZ3-only window probes and before
the manager `0x95` read-prime, then uses ordinary word-addressed
`0x91/sub0` reads. Unknown behavior fails conservatively.

Normal invocation reports the official cartridge's GBA header:

```bash
ezfadvanceIII_card_reader
```

The implementation has been hardware-confirmed on macOS with an official
Golden Sun cartridge. It reported title `GOLDEN_SUN_A`, game code `AGSF`,
maker code `01`, ROM version `0`, and a valid header checksum, then completed
the three-poll/1000-ms read-session readiness transition. This proves official
detection, header inspection, and cleanup on the tested device. Full ROM
extraction remains to be hardware-qualified.

Raw extraction is requested explicitly:

```bash
ezfadvanceIII_card_reader --extract OUTPUT.gba [--verbose]
```

This path is available only for a detected official cartridge. It reuses the
capture-proven ordinary ROM-read primitive to read the complete 32-MiB GBA
address space as 512 sequential 64-KiB blocks. Byte offsets are encoded as
word addresses, ending with byte offset `0x01ff0000` / word address
`0x00ff8000`. During the read, an in-place progress bar reports percentage,
completed/total MiB, average throughput, elapsed time, and ETA. The file is
written only after the ROM read and read-session cleanup succeed, and an
existing destination is not overwritten.

Passing `--verbose` replaces the live extraction bar with per-block diagnostics
showing each 64-KiB byte address, transfer length, duration, and throughput,
then prints the total read duration and average MiB/s. As with the writer,
verbose mode is intended for protocol and performance diagnosis; it does not
change the USB transcript or output bytes. The option is rejected unless
`--extract` is also present.

After the complete scan and session cleanup, the reader finds the last byte
that is not `0xFF` and writes the smallest standard extent—1, 2, 4, 8, 16, or
32 MiB—that contains it. Bytes inside that extent remain unchanged; only the
trailing erased address-space padding is omitted. This is a generic,
deterministic heuristic and is not claimed to reproduce the original manager's
unknown size-detection algorithm. Header-database lookup and title-specific
rules remain out of scope.

Extraction authorization is deliberately stronger than classification. The
first 64-KiB block must contain the GBA fixed header byte `0x96` at offset
`0xB2` and pass the complement checksum before any later block is requested.
Failure stops extraction, attempts normal read-session cleanup, and creates no
output file. The save reader independently requires
`CartridgeKind::ez3_flash` before EZ3 loader/catalog/save processing; an
official or unknown cartridge is rejected after cleanup.

### 26.7 EZ3 catalogued-ROM extraction

The card reader can extract one ROM from a recognized EZ3 single- or multi-ROM
catalog:

```bash
ezfadvanceIII_card_reader --extract N OUTPUT.gba [--verbose]
```

`N` is the one-based number printed by normal inspection. When it is omitted,
an EZ3 cartridge defaults to catalog ROM 1; the same unnumbered syntax retains
its existing official-cartridge behavior when an official ROM is detected.
The catalog type defines the stored size class as `32 MiB >> type`; its inclusive end is
`start + size - 1`. Extraction selects the smallest capture-proven linear read
mapping covering that end and reads the complete stored extent.

Physical ROM 1 begins at card byte zero. EZ3Manager replaces its original
four-byte ARM branch with a branch to the loader/menu and saves the original
branch target in the first catalog entry. The extractor reconstructs those
four bytes with `CartridgeFormat::makeArmBranch(target_or_start)`. Later
catalog entries store their card start in that field and their first
instruction was not patched, so ROM 2 and later are deliberately left
unchanged.

When a loader extent intersects the selected ROM size-class range, the reader
restores that intersection to erased `0xFF`. This mirrors the manager/builder
rule that permits loader placement only in erased ROM space. The reconstructed
fixed header byte and complement checksum must validate before the output is
created. Existing destinations are refused, and cleanup must complete before
file creation. The workflow sends no erase or program operation.

This produces the catalog-sized ROM image. It does not claim recovery of an
original nonstandard file length that was rounded into a catalog size class.
The implementation is offline-tested and hardware-qualified on a physical
two-ROM card. Extracted ROM 1, including reconstructed entry bytes, matched
the original file's SHA-256 checksum. Extracted ROM 2, whose entry bytes were
left unchanged, also matched its original file's SHA-256 checksum. A separate
single-ROM-layout extraction remains the next qualification gate.
Default reporting uses the same progress bar as official-cartridge extraction.
`--verbose` instead prints each block address, length, duration, and throughput,
followed by total duration and average throughput.

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

## 29. Save-memory companion work

The mainline `ezfadvanceIII_save_reader` implements save extraction and, from
0.10.0, a guarded 32-KiB save-write path derived directly from
`writesav.pcap`. The writer requires a completed backup before the payload,
explicit destructive authorization, and full read-back verification.

Capture-derived save access uses a different command mode from ROM reads/writes.

Known behavior includes:

```text
save read  : opcode 0x91, sub/mode 0x01
save write : opcode 0x92, sub/mode 0x01
common read extent tested: 32 KiB
```

A selector around `0x0900` was observed in save-related traffic.

A Bios_Dumper save-reader test produced an exact 32-KiB hardware match. The
object-oriented `SaveMemoryReader` refactor was subsequently tested against the
same multi-ROM Castlevania + Bios_Dumper card, and repeated dumps were
byte-identical.

The 0.10.0 writer emits the eight captured `0x92/02` selector exchanges,
selector `0x0900`, the exact `0x92/01` 32768-byte command, one 32768-byte OUT
payload, and validates the returned command echo. It then uses the established
bounded readiness transition before the independently proven save-read path.
The resulting 32768 bytes must match the input exactly. The transaction is
locked by an injectable-transport test; real-hardware qualification is the
next required checkpoint.

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
| single 1 MiB Classic NES Super Mario | yes | yes | `EEPROM_V124`, explicit map 4 |
| single 1 MiB Classic NES Castlevania | yes | yes | `EEPROM_V124`, explicit map 4 |
| single 1 MiB Classic NES Super Mario, forced map 5 | generated image fully verifies | runtime failure | exact `GAME PACK ERROR / TURN THE POWER OFF.` screen |
| single 1 MiB Classic NES Castlevania, forced map 5 | generated image fully verifies | runtime failure | identical error screen; map 4 works |
| 1 MiB + 1 MiB Classic NES | yes | yes | map 4 + map 4 |
| single 4 MiB F-Zero | yes | yes | status-only partial-first-window verify |
| 4 MiB F-Zero + 1 MiB Super Mario | derived from captured rules | yes | map 3 + map 4 |
| 4 MiB F-Zero + 1 MiB Super Mario + 1 MiB Castlevania | derived from captured rules | yes | map 3 + map 4 + map 4 |
| 8 MiB Advance Wars + 4 MiB F-Zero + 1 MiB Super Mario + 1 MiB Castlevania | partial higher-window verify not capture-proven | yes | mixed map 6 + map 3 + map 4 + map 4; ~14 MiB |
| 16 MiB Tales + 4 MiB F-Zero + 1 MiB Super Mario + 1 MiB Castlevania | partial higher-window verify not capture-proven | yes | mixed map 5 + map 3 + map 4 + map 4; ~22 MiB |
| single 8 MiB Advance Wars | yes | yes | exact-8-MiB `0x0040` transition |
| 6 ROMs / 24 MiB | yes | yes | write + verify + menu + all launches |
| 6 ROMs / 32 MiB | yes | yes | full-card write + verify + menu + all launches |
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
4_4_4_4_8MB.pcap
4_4_4_4_8_8MB.pcap
4_4_4_4_4_4_8MB.pcap
4_4_4_4_4_4_4_4MB.pcap
4MB.pcap
2MB.pcap
2_2MB.pcap
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
embedded marker: EEPROM_V...
warning: yes
captured maps:
  map 4 -> Classic NES Super Mario / Castlevania, EEPROM_V124
  map 5 -> Tales of Phantasia, EEPROM_V124
automatic map selection: disabled
required action: explicit --mapN=4 or --mapN=5
ROM patching: none performed by writer or original manager
```

The same `EEPROM_V124` marker occurs with both map 4 and map 5, so the marker revision is not a capacity/configuration discriminator.

Map-4 launch behavior is hardware-proven in single-ROM, two-ROM, and mixed map-3/map-4 menus.

Forced map-5 A/B tests on both Classic NES titles are also hardware-proven to fail at runtime with the identical:

```text
GAME PACK ERROR
TURN THE POWER OFF.
```

screen, despite successful write and full read-back verification. This separates flash-image integrity from runtime mapping correctness.

Save/load persistence testing remains useful, especially for identifying the underlying EEPROM capacity/configuration behavior that distinguishes map 4 from map 5.

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

The 0.5.10 source was compiled successfully on macOS / Apple Silicon with Homebrew libusb. Linux and BSD compile/hardware validation remain pending.

### 36.11 0.5.11

Added evidence from `4_4_4_4_8MB.pcap`:

- five active catalog entries;
- same existing `0x7080` multi-loader and 125 relocations;
- final 26 bytes zero for the five-ROM form, generalizing the four-ROM tail behavior;
- exact 24-MiB / 192-Mbit programming geometry;
- capture-proven exact 24-MiB linear verification transition using `0x00C0` before linear `0x91` reads.

The release also added `flash_192mb_prepare_linear_verify()`.

### 36.12 0.5.12

Removed the artificial small fixed ROM-count limit and replaced it with structural/capacity validation. Subsequent 6-, 7-, and 8-ROM captures validate this direction.

Current rules:

- total input ROM file bytes may be **up to and including 32 MiB / 256 Mbit**; equality is required for proven full-card 6-, 7-, and 8-ROM cases;
- the packed image plus loader must still fit the physical 32-MiB card;
- the embedded multi-loader exposes 120 whole 28-byte catalog slots between offsets `0x476E` and `0x548E`; this is enforced only as a structural safety bound;
- multi-digit override syntax is supported, for example `--type10=...` and `--map10=...`;
- six, seven, and eight active entries are capture-proven without changing the loader blob, relocation algorithm, catalog encoding, or full-32-MiB verify sequence.

The 8-ROM capture also proves that a full 32 MiB of ROM files can coexist with the loader when EZ3Manager places the loader inside an existing erased/internal `FF` run.

### 36.13 0.5.13

Added default live progress bars for erase, program, and verify. `--verbose` retained detailed per-operation output and transfer timing.

### 36.14 0.5.14

Experimental attempt to reuse the exact-8-MiB `0x0040` transition for a 4-MiB single image. Hardware disproved that generalization. Superseded.

### 36.15 0.5.15

Temporarily skipped unproven smaller first-window verification while preserving exact 8-MiB behavior.

### 36.16 0.5.16

`4MB.pcap` proved exact 4-MiB status-only linear verification. A fixed 50-ms delay was temporarily used from one quantized capture.

### 36.17 0.5.17

The 1-MiB and two-1-MiB captures completed the generic sub-8-MiB model:

- partial first-window images use `FFFF,04,00,00` then linear `0x91` reads;
- no `0x0020` or `0x0040` selector for partial first-window geometries;
- no fixed synthetic delay;
- program extent rounds to `0x100`;
- verification rounds to `0x10000`;
- `EEPROM_V124` is proven with both map 4 and map 5, so EEPROM auto-map selection was removed.

Real hardware subsequently validated the 1-MiB map-4 singles, two-ROM map-4 pair, single 4-MiB F-Zero, mixed map-3/map-4 configurations, and 6-ROM 24-/32-MiB images.

### 36.18 0.6.0 — synchronized toolset version

All mainline utilities are synchronized to:

```text
ezfadvanceIII_multirom_writer 0.6.0
ezfadvanceIII_card_reader     0.6.0
ezfadvanceIII_save_reader     0.6.0
ezfadvanceIII_wipe_card       0.6.0
```

From this release onward, any code change to at least one mainline program bumps the **shared version for every program**. An unchanged utility can therefore receive a new version solely to remain aligned with the toolset release.

The writer's 0.6.0 behavior adopts the stable 0.5.17 baseline. The other utilities are version-synchronized without implying a protocol change in every utility.


### 36.19 0.6.1

The writer's capture-backed verification policy was made explicit in code and user-facing diagnostics:

```text
< 8 MiB                 verify
exact 8 MiB             verify
exact 16 MiB            verify
captured tiny >16 MiB   verify
exact 24 MiB            verify
exact 32 MiB            verify
other partial higher-window geometries -> skip
```

No new experimental selector was introduced. In particular, successful ~14-MiB and ~22-MiB hardware writes did not justify guessing the linear-read mapping for those partial higher-window extents.

The card reader, save reader, and wipe utility received only the synchronized project-version update; their protocol behavior remained unchanged.

### 36.20 0.6.2

Hard-coded project-version text was removed from runtime banners in all four utilities.

Runtime output now identifies the utility and host platform, for example:

```text
ezfadvanceIII manager-primed ROM writer (macOS)
Read-only EZF Advance III card inspector (Linux).
Read-only EZF Advance III save dumper (FreeBSD).
EZF Advance III card wipe utility (macOS)
```

The shared release version remains represented by filenames, source comments, tags/releases, packaged artifacts, and documentation. The GBA header's `ROM version` field is not a tool version and remains unaffected.

### 36.21 0.7.0 — object-oriented architecture migration

The toolset was migrated from largely monolithic utilities to shared,
composable C++17 modules. This release introduces RAII USB ownership, an
injectable transport abstraction, shared protocol and read-state objects,
cartridge domain models, application services, and offline unit tests. The
major architectural change warrants a minor-version increment while retaining
the proven 0.6.2 hardware protocol behavior.

No USB protocol, erase timing, save-read behavior, or card-read behavior was changed by this cleanup.

### 36.22 0.7.1 — robust writer option parsing

Writer override parsing now uses strict, bounded decimal conversion for
`--typeN=VALUE` and `--mapN=VALUE`. Invalid text, signs, out-of-range slots,
values above 255, and numeric overflow are rejected through the normal parser
error path without uncaught conversion exceptions. Offline tests cover these
cases. No USB protocol behavior changed.

### 36.23 0.7.2 — ARM entry-branch regression coverage

ARM branch encoding and decoding now share the `CartridgeFormat` implementation
used by the writer. Offline tests cover known loader-branch bytes, original
entry-target decoding, signed branch immediates, representable boundaries,
invalid instructions, unaligned targets, and out-of-range targets. No USB
protocol behavior changed.

### 36.24 0.7.3 — protocol failure-path regression coverage

Offline fake-transport tests now verify exact `0x92` command layouts, selector
and data preservation, matching/mismatched/short echoes, command and data OUT
failures, IN failures, custom timeout propagation, and zero/custom settle
delays. The production protocol implementation and USB behavior are unchanged.

### 36.25 0.7.4 — reusable transcript-test transport

A test-only `TranscriptTransport` now validates ordered OUT, exact-IN, and
maximum-IN transfers, including exact bytes, requested sizes, and timeouts. It
reports missing, extra, wrong-direction, and mismatched transfers clearly.
Protocol success tests exercise the helper without changing production or USB
behavior.

### 36.26 0.7.5 — partial first-window verification transcript

A narrowly scoped `VerificationSession` now owns the `<8 MiB` post-program
verification operation through `Transport&`. Its offline transcript fixture
proves the capture-derived `FFFF`, `04`, `00`, `00` status transition, rounded
64-KiB global-linear `0x91` reads, erased-`FF` tail comparison, exact request
sizes and timeouts, and the absence of `0020`, `0040`, `0080`, `00C0`, `55AA`,
and `AA55` mapping operations. Policy selection remains outside the session;
exact-size and tiny-tail paths remain unchanged in the writer.

Real hardware subsequently confirmed this extraction using a single 4-MiB
F-Zero image after a full card wipe. Programming completed, full read-back
verification succeeded, and the cartridge booted successfully on a real Game
Boy Advance.

### 36.27 0.7.6 — exact 8-MiB verification transcript

The exact 8-MiB verification operation now runs through `VerificationSession`
without changing policy selection or any higher verification path. Its offline
fixture preserves the captured status sequence, `55AA`, `0200`, `0040`, `0000`,
the precisely positioned 125-ms delay, `AA55`, reset/select operations, the
final read prefix, and all 128 global-linear 64-KiB `0x91` reads. Negative
assertions prove that `0020`, `0080`, and `00C0` selectors are absent. Hardware
confirmation subsequently succeeded with an exact 8-MiB image: writing and full
read-back verification completed through the final 64-KiB block, and the
cartridge booted successfully on a real Game Boy Advance.

### 36.28 0.7.7 — exact 8-MiB fixture hardening

Every 64-KiB block in the exact 8-MiB fixture now has distinct content, making
accidental comparison against the wrong image block detectable. The fixture
also asserts every callback offset and length. An explicitly empty test delay
callback is rejected during `VerificationSession` construction with a clear
argument error. The production constructor, timing, and USB behavior are
unchanged.

### 36.29 0.7.8 — exact 16-MiB verification extraction

The exact 16-MiB path is now an explicit `VerificationSession` operation. It
preserves the `FFFF`, `04`, `00`, `00` status sequence, the capture-derived
`55AA`, `0200`, `0080`, `0000` mapping transition, the 125-ms settle before
`AA55`, the selector writes, the second status sequence, the four-write read
prefix, and 256 global-linear 64-KiB reads.

The offline transcript uses different content in each block, asserts every
verification callback offset and length, and rejects unintended `0020`,
`0040`, and `00C0` selectors. Policy selection, erase/program behavior, and
all remaining verification geometries are unchanged.

Real-hardware confirmation subsequently succeeded with an exact 16-MiB image.
Writing completed, full read-back verification reached and completed the final
64-KiB block at `0x00ff0000`, and the cartridge booted successfully on a real
Game Boy Advance.

### 36.30 0.7.9 — tiny-tail-above-16-MiB verification extraction

The narrow `fireemblem.pcap` verification exception now belongs to
`VerificationSession`. It accepts only `16 MiB < size <= 16 MiB + 64 KiB`,
rounds the read extent through `0x1010000`, and compares bytes beyond the image
end against erased `0xFF`. The transcript preserves the short status and
four-write read prefix followed by 257 global-linear 64-KiB reads.

The fixture uses a non-aligned tail and block-distinct content, checks every
callback, proves zero delay calls, and rejects `0020`, `0040`, `0080`, `00C0`,
`0200`, and `AA55`. Policy, programming, exact 24/32-MiB verification, and
unsupported partial higher-window behavior remain unchanged.

Real-hardware confirmation subsequently used the capture-derived single Fire
Emblem layout. The loader was placed at `0x01000010`, the constructed image was
`0x1000700`, and the writer programmed a `0x700`-byte BANK2 tail. Full
read-back verification completed through the rounded block at `0x01000000`.
The card reader then reported one Fire Emblem ROM, the expected loader address,
and a valid header, and the game booted successfully on a real Game Boy
Advance.

### 36.31 0.7.10 — exact 24-MiB verification extraction

The exact 24-MiB path from `4_4_4_4_8MB.pcap` is now an explicit
`VerificationSession` operation. It preserves the status sequence, the
`55AA / 0200 / 00C0 / 0000` mapping transition, `AA55` and selector sequence,
the second status sequence, the four-write read prefix, and 384 global-linear
64-KiB reads ending at `0x017f0000`.

The capture timestamp indicates an approximately 109-ms quiet interval. The
implementation deliberately preserves the established 125-ms settle as its
behavioral contract. The transcript asserts exactly one `125000`-microsecond
delay after `55AA / 0200 / 00C0 / 0000` and immediately before `AA55`. It also
uses block-distinct data, verifies every callback, rejects the wrong size, and
proves `0020`, `0040`, and `0080` are absent. Exact 32-MiB verification remains
unchanged in the legacy writer.

Real-hardware confirmation subsequently used an exact five-ROM
`0x01800000` image. Programming and the preserved 125-ms transition succeeded,
and full read-back verification completed through the final 64-KiB block at
`0x017f0000`. Card inspection reported the expected layout, menu behavior was
correct, and all selected games launched successfully on a real Game Boy
Advance.

### 36.32 0.7.11 — card-reader stored end reporting

The read-only card inspector now reports an inclusive `Stored end` address for
every cataloged ROM. It derives the value from the physical start and the
catalog size class (`type 0` = 32 MiB through `type 9` = 64 KiB), so compact
layouts remain represented by their catalog allocations rather than inferred
from the next ROM or loader address. Invalid classes or extents are identified
without address wraparound. No USB reads, mappings, or card state transitions
changed.

### 36.33 0.7.12 — exact 32-MiB verification extraction

The final exact-size path from `256MBits-rom.pcap` now belongs to
`VerificationSession`. Its post-program transition intentionally omits a
`00C0` selector because programming has already ended in the `00C0` window. It
preserves status, `55AA / 0000 / 0000 / 0200`, exactly one 125-ms delay before
`AA55`, the reset/select and second status sequences, the four-write read
prefix, and 512 global-linear 64-KiB reads ending at `0x01ff0000`.

The fixture uses block-distinct content, verifies every callback, rejects the
wrong size, and proves `0020`, `0040`, `0080`, and `00C0` selectors are absent.
After caller checks showed no remaining dependencies, the legacy writer-side
read-command builder, verification extent and comparison helpers, and generic
linear verifier were removed. No erase, programming, or unsupported-partial
behavior changed.

The extracted implementation was then hardware-confirmed with two exact-size
images. A single 32-MiB Kingdom Hearts ROM completed write and all 512
read-back blocks through `0x01ff0000`, and booted on a real GBA. A three-ROM
16+8+8-MiB image completed the same full-card verification; its loader menu and
all three games booted without issue. The verbose transcript is recorded in
`resultmultiroms32MiB.log`.

### 36.34 0.7.13 — bounded readiness retry

The writer preflight, shared read-only cartridge startup, and wipe preflight
now tolerate a device that initially returns `0x98 -> 00` while the inserted
cartridge becomes ready. They perform at most five readiness checks separated
by 100 ms, retrying only the explicit `00` state. A transport failure or
unexpected byte still fails immediately, and five `00` responses fail before
any destructive action. The writer and reader's surrounding capture-derived
`0x97` and `0x99` startup operations are unchanged.

After card inspection and after any successfully initialized save-reader
operation, the read-only tools replay three `0x98 -> 01` polls followed by a
1000-ms quiet interval. The first hardware retest proved that this restores
the readiness response, but not necessarily an erase-ready bridge: a wipe
started without unplugging passed `0x98 -> 01`, then safely stopped when its
first `0x96` erase returned status `01` instead of the required `00`. No later
erase command was issued.

The wipe preflight was therefore brought to the same known-good bridge-startup
boundary already owned by the writer: require `0x97 -> 00`, bounded
`0x98 -> 01`, then the exact `0x99` parameter-`01` echo before entering the
otherwise unchanged captured erase sequence. Repeating card reader then wipe
without an unplug/replug cycle completed successfully on real hardware. The
writer already performed the full manager startup and needed no protocol
change. This evidence supports describing the three-poll reader epilogue as a
readiness transition, not as a complete bridge reset.

---

### 36.35 0.7.14 — read-session transition regression coverage

The post-0.7.13 wipe startup correction is now represented by synchronized
toolset version 0.7.14. A narrow `ReadSessionTransition` component owns only
the read-only path's bounded `0x98` readiness polling and its three-poll
readiness epilogue. Its transcript fixture asserts immediate success, explicit
`00` retries with 100-ms delays, success after retries, five-attempt
exhaustion, unexpected-response and transport failures, and exactly three
successful epilogue polls followed by exactly one 1000-ms delay.

The reader epilogue remains classified as a readiness transition rather than
a full bridge reset. Destructive workflows remain responsible for their own
complete `0x97 -> 0x98 -> 0x99` startup. No verification mapping or other
flash protocol sequence changed. `resultmultiroms32MiB.log` is intentionally a
local ignored hardware artifact; the durable exact-32-MiB results are recorded
in sections 11.5 and 36.33.

Real-hardware stabilization then exercised both process boundaries without a
USB unplug/replug cycle:

```text
card reader -> wipe       passed
save reader -> writer     passed
```

The wipe and writer each completed their own full `0x97 -> 0x98 -> 0x99`
startup before destructive work. Together these tests hardware-confirm the
0.7.14 ownership rule: the read-only epilogue is a readiness transition only,
while every destructive workflow establishes its own complete bridge state.

### 36.36 0.7.15 — official cartridge detection and inspection

The shared read-only initializer exposes explicit EZ3, official-ROM, and
unknown classification while preserving the established EZ3 probe and mapping
path. The card reader applies GBA-header semantics to official cartridges.
Golden Sun detection and header inspection are hardware-confirmed on macOS.

### 36.37 0.7.16 — official cartridge raw extraction

The card reader adds a fixed 32-MiB raw extraction using 512 global-linear
64-KiB reads, with a live progress display or verbose per-block timing. The
software specification documents the fixed untrimmed output boundary and the
remaining hardware qualification requirement.

### 36.38 0.7.17 — guarded extraction and save-reader policy repair

Block zero must pass the GBA fixed-value and header-checksum validation before
the remaining 511 blocks are requested. Unchanged probe behavior alone is no
longer sufficient to authorize a complete dump.

The save reader requires EZ3 classification before parsing a loader or catalog
or selecting save memory, preventing official-ROM initialization from falling
through into EZ3-only semantics.

### 36.39 0.7.18 — generic official-ROM erased-padding trim

Official extraction continues to read the full 32-MiB address space. After a
successful read and cleanup, a pure tested sizing function finds the final
non-`FF` byte and rounds its exclusive end up to 2, 4, 8, 16, or 32 MiB. The
output file contains the selected prefix byte-for-byte; only trailing erased
padding is removed. This is a documented generic sizing heuristic, not a claim
that every selected size has been established on hardware. Hardware testing
completed the guarded 32-MiB Golden Sun
scan, selected the correct 8-MiB extent, matched the trusted SHA-256
`5eb59f508c25548fb0ef72911cc75a81867f16b0ef8fca2a22cb6d026a862cd8`,
and booted the extracted file successfully on real hardware. The 2-, 4-, 16-,
and 32-MiB sizing outcomes remain without equivalent hardware evidence.

### 36.40 0.7.19 — shared official-header confirmation policy

Ordinary official-cartridge inspection now requires the same GBA fixed byte
`0x96` and complement checksum used to authorize extraction. Probe behavior
still selects the candidate official-ROM branch, but the program does not print
the confirmed official-cartridge report unless the header validation passes.
Failure attempts the normal read-session cleanup. No classification command,
EZ3 probe, extraction block count, or word address changed.

### 36.41 0.7.20 — explicit 12-MiB partial verification transcript

The writer now recognizes exactly `0x00C00000` bytes as the capture-proven
12-MiB partial higher-window checkpoint from `8_4MB.pcap`. Its dedicated
`VerificationSession` operation emits the standard status sequence followed by
`55AA` and three `0000` writes, then performs 192 global-linear 64-KiB `0x91`
reads. The final read begins at `0x00BF0000` and ends at `0x00C00000`.

The transcript test asserts every callback offset and length, block-varying
payload data, no delay, and the absence of `0200`, `0020`, `0040`, `0080`,
`00C0`, `AA55`, and the `AA/55/06` selector tail. No other partial size is
enabled, and all existing exact-size and tiny-tail paths remain unchanged.
Hardware testing completed all 192 reads through final offset `0x00BF0000`.
The two-ROM menu booted on a real GBA, and both Advance Wars and F-Zero
launched successfully.

### 36.42 0.7.21 — 1-MiB official-ROM sizing correction

The generic official-ROM trailing-`FF` sizing table now includes the standard
1-MiB extent. The boundary tests require a final meaningful byte at
`0x000FFFFF` to select `0x00100000` bytes and a meaningful byte at
`0x00100000` to select 2 MiB, while preserving all existing higher-size
transitions and all-`FF` rejection.

This is a size-policy correction only. The full 32-MiB / 512-block scan,
word-address encoding, official-cartridge classification, header validation,
read-session cleanup, and EZ3 writer verification mappings are unchanged.
Existing real-hardware tests prove 1-MiB Classic NES images on the EZ3
build/program/verify/boot path; physical official-cartridge extraction at
1 MiB remains pending a trusted dump/hash comparison.

### 36.43 0.7.22 — explicit 20-MiB partial verification transcript

The writer now recognizes exactly `0x01400000` bytes as the capture-proven
20-MiB partial higher-window checkpoint. `16_4MB.pcap` and `4_8_8MB.pcap`
independently show the same transition despite different ROM compositions.
The dedicated `VerificationSession` operation emits the standard status
sequence followed by `55AA` and three `0000` writes, then performs 320
global-linear 64-KiB `0x91` reads. The final read begins at `0x013F0000` and
ends at `0x01400000`.

The transcript test asserts every callback offset and length, block-varying
payload data, no delay, and the absence of `0200`, `0020`, `0040`, `0080`,
`00C0`, `AA55`, and the `AA/55/06` selector tail. No other size in the
16–24-MiB partial range is enabled, and all existing verification paths remain
unchanged. Hardware testing without a preliminary full-card wipe completed all
320 reads through final offset `0x013F0000`. The menu booted on a real GBA,
and both Tales of Phantasia and F-Zero launched successfully.

### 36.44 0.7.23 — explicit 28-MiB partial verification transcript

The writer now recognizes exactly `0x01C00000` bytes as the capture-proven
28-MiB partial higher-window checkpoint from `4_8_16MB.pcap`. Its dedicated
`VerificationSession` operation emits the standard status sequence followed by
`55AA` and three `0000` writes, then performs 448 global-linear 64-KiB `0x91`
reads. The final read begins at `0x01BF0000` and ends at `0x01C00000`.

The transcript test asserts every callback offset and length, block-varying
payload data, no delay, and the absence of `0200`, `0020`, `0040`, `0080`,
`00C0`, `AA55`, and the `AA/55/06` selector tail. No other size in the
24–32-MiB partial range is enabled, and all existing verification paths remain
unchanged. Hardware testing completed all 448 reads through final offset
`0x01BF0000`. The menu booted on a real GBA, and Tales of Phantasia, Advance
Wars, and F-Zero all launched successfully.

### 36.45 0.7.24 — EZ3 catalogued-ROM extraction

The reader adds catalog-size-class extraction from recognized EZ3 layouts.
It restores overlapping loader bytes to `0xFF`, reconstructs the original ARM
branch for physical ROM 1 only, validates the resulting GBA header, and writes
only after read-session cleanup. ROM 1 and ROM 2 from a physical two-ROM card
both matched their original files by SHA-256.

### 36.46 0.7.25 — unified extraction and progress reporting

Official and EZ3 extraction now share the `--extract` interface. EZ3 reads use
the same default progress display and optional verbose per-block diagnostics as
official-cartridge extraction.

### 36.47 0.7.26 — numbered EZ3 extraction routing fix

The numbered form now bypasses the obsolete official-only refusal branch and
reaches the selected EZ3 catalog entry. Hardware retesting confirmed matching
ROM 1 and ROM 2 checksums.

### 36.48 0.7.27 — default EZ3 ROM selection

When an EZ3 extraction omits the catalog number, the reader now selects ROM 1.
Numbered EZ3 extraction and unnumbered official-cartridge extraction retain
their existing behavior.

### 36.49 0.7.28 — extraction regression coverage

Card-reader CLI parsing and official/EZ3 action selection now live in a small,
device-independent component. The offline suite locks down the numbered EZ3
routing fix, unnumbered ROM-1 default, invalid forms, stored catalog extents,
loader-overlap blanking, first-ROM-only entry reconstruction, mapping
thresholds, and a final official-ROM transfer shorter than 64 KiB.

### 36.50 0.7.29 — shared version reporting

All four applications accept standalone `--version` without initializing
libusb. They print their stable executable name and the version owned by the
shared `version.hpp` header, preventing per-program version drift.

### 36.51 0.7.30 — shared EZ3 catalog parser

The duplicated single/multi-ROM loader interpretation in the card reader and
save reader is replaced by the pure `Ez3CatalogParser`. It validates duplicated
counts, structural bounds, entry plausibility, alignment, and caller-provided
address bounds without performing USB operations or deciding evidence policy.
The card reader continues to distinguish the 1–8 capture-proven range from the
120 structural slots. The save reader continues to reject counts above three
and loader/catalog addresses outside its first-16-MiB evidence boundary. It
also requires explicit `--rom N` selection for multi-ROM save extraction while
retaining automatic ROM-1 selection on a single-ROM layout. The current 0.8.1
policy requires exactly one supported `SRAM_V111` candidate and requires the
explicit selection to match it. Zero, multiple, or mismatched candidates are
refused because `--rom N` cannot represent an unproven hardware save-slot
switch. Signature scanning is independently clamped to the save reader's
first-16-MiB evidence boundary. Hardware validation on the Castlevania +
Bios_Dumper layout confirmed that mismatched ROM 1 is refused without an output
file and that the unique matching ROM 2 produces the expected 32-KiB save.

### 36.52 0.7.31 — pure cartridge image builder

`RomInfo`, `BuiltCartridgeImage`, the captured loader assets, ROM placement,
loader relocation, catalog generation, first-entry patching, and programmed
extent calculation now belong to the separately compiled
`CartridgeImageBuilder`. The executable retains file loading, interactive
warnings, destructive authorization, USB erase/program operations, and
evidence-specific verification selection. Deterministic whole-image FNV-1a
fixtures cover single, two-, three-, and eight-ROM construction plus internal
single-ROM loader placement.

### 36.53 0.8.0 — injectable card-writer workflow

The established destructive-write workflow is now coordinated by the
separately compiled `CardWriter`. A `WriterBackend` interface preserves the
capture-specific libusb implementation while allowing offline tests to assert
the exact preflight, bridge initialization, erase, program, cleanup, and
verification-selection order. Tests also prove that preflight and erase
failures stop later operations, `--skip-verify` performs its final status
cleanup, and unsupported higher-window geometries use only the conservative
cleanup path.
The extracted workflow was subsequently validated on real hardware with an
8-MiB F-Zero/Mario Kart image: programming and full verification succeeded,
the EZ3 menu booted, and both games launched on a real GBA.

### 36.54 0.8.1 — evidence-bounded multi-ROM save selection

Multi-ROM save extraction now requires exactly one capture-proven `SRAM_V111`
candidate and an explicit `--rom N` matching that candidate. Zero, multiple,
and mismatched candidates are refused because no hardware save-slot switch is
proven. Save-signature scanning is clamped to the first-16-MiB evidence limit.
Both refusal and successful extraction were confirmed on real hardware.

### 36.55 0.9.0 — extracted libusb writer backend

The capture-derived destructive USB implementation and its console progress
reporting now reside in `src/libusb_writer_backend.cpp`. The writer executable
opens the device and obtains the concrete implementation through
`makeLibusbWriterBackend`; `CardWriter` continues to consume only the abstract
backend. The mechanical move preserves command bytes, timing, flash-window
geometry, erase/program behavior, and separately named verification paths.
The 0.9.0-specific hardware requalification used the established two-ROM 8-MiB
F-Zero/Mario Kart image. Programming and exact-8-MiB full verification
succeeded; the menu and both games booted on a real GBA. The other
capture-proven verification paths retain their prior hardware qualification
and are mechanically preserved, but were not all physically rerun under 0.9.0.
The offline coordinator matrix now covers all nine verification dispatches,
unsupported-geometry and `--skip-verify` cleanup (including cleanup failures),
and short-circuit failures at preflight, initialization, preparation, erase,
programming, and verification stages.

### 36.56 0.10.0 — guarded SRAM save writing

The save utility now exposes the exact 32-KiB `SRAM_V111` transaction captured
in `writesav.pcap`: eight `0x92/02` selector exchanges ending at selector
`0x0900`, an `0x92/01` command declaring `0x8000` bytes, one payload OUT, and
the matching command echo. The application accepts only an exact 32768-byte
input and requires `--backup FILE` plus `--yes-really-write`. It refuses an
existing backup path, saves the current cartridge contents first, performs the
bounded readiness transition before writing and again before verification, and
requires a complete byte-for-byte read-back match through the independently
proven `0x91/01` path.

The same unique-`SRAM_V111` and explicit multi-ROM selection policy remains in
force; the new option does not invent a save-slot switch or generalize to
other SRAM, EEPROM, or Flash formats. Offline transcript tests cover the exact
read/write transfers and failure checks. Real-hardware save backup, write,
read-back, and game-load validation remain required before this path is called
hardware-proven.

---

## 37. Build environment and native platform scope

The current source targets C++17 and libusb on Unix-like systems. The portable
Makefile selects only the support modules required by each executable and
prefers `pkg-config`, with fallbacks for common system and Homebrew prefixes.

Native project scope:

```text
macOS       supported target; current 0.10.0 baseline compiled and transcript-tested; extracted backend specifically requalified with the two-ROM exact-8-MiB path; save writing awaits hardware qualification; other prior qualifications retained; 1-MiB official extraction awaits hardware qualification
Linux       supported target; CI compile/offline tests pass; physical USB validation pending
FreeBSD     supported target; validation pending
OpenBSD     supported target; validation pending
NetBSD      supported target; validation pending
DragonFly   supported target; validation pending
Windows     no native mainline support; use a Linux VM with USB passthrough
```

Build all four programs:

```bash
make
```

Run syntax checks and offline regression tests:

```bash
make check
make test
```

The offline tests do not initialize libusb or access a cartridge. They cover
binary-format parsing, GBA checksums, catalog decoding, ARM branch decoding,
card-reader CLI/action routing, EZ3 extraction reconstruction and mapping
thresholds, partial official-ROM reads, `0x92` command construction, recorded
transport ordering/timeouts, and every verification-policy boundary.

Warnings-enabled development check:

```bash
make check WARNFLAGS="-Wall -Wextra -Wpedantic"
```

Callers may override `CPPFLAGS`, `CXXFLAGS`, `WARNFLAGS`, `LDFLAGS`, and
`LDLIBS`. The Makefile keeps its internal `-Iinclude` path active when
`CPPFLAGS` is overridden.

The exact BSD linker/include flags may vary with the base system or package installation and should be documented only after each target is compiled successfully.

Windows XP `usbscan.sys` experiments remain useful historical reverse-engineering material, but they are not maintained as a native backend for the current writer. Windows users are expected to pass VID `0x0E6A` / PID `0x5088` through to a Linux VM and run the Linux build there.

Warnings-enabled development builds are recommended while modifying protocol code.

---

## 38. Recommended test procedure after code changes

### 38.1 Before USB write

First run the complete offline suite:

```bash
make test
```

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

Protocol bytes, command ordering, transfer sizes, delays, erase geometry, or
read/write mapping changes require an explicit real-device checkpoint. Tests
must stop at that boundary and record the hardware result before further
protocol refactoring.

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

### 39.7 Five-ROM 24-MiB fixture (`4_4_4_4_8MB.pcap`)

After stable descending-size ordering:

```text
#1 8 MiB @ 0x0000000
#2 4 MiB @ 0x0800000
#3 4 MiB @ 0x0C00000
#4 4 MiB @ 0x1000000
#5 4 MiB @ 0x1400000
Loader @ 0x2CC420
Image end = 0x1800000 = 24 MiB
```

The loader has count fields `5 / 5`, uses the unchanged multi-loader template with exactly 125 relocations, and uses the same final-26-byte zero tail as the four-ROM form.

This capture additionally proves exact 24-MiB verification. After window-2 programming, EZ3Manager performs the `0x00C0` mapping transition and then 384 linear 64-KiB `0x91` reads. All captured program and verify payloads matched.

### 39.8 Six-ROM full-card fixture (`4_4_4_4_8_8MB.pcap`)

```text
8 + 8 + 4 + 4 + 4 + 4 MiB = 32 MiB
ROM starts = 0x0000000, 0x0800000, 0x1000000, 0x1400000,
             0x1800000, 0x1C00000
Loader @ 0x5C2360
Catalog count = 6 / 6
```

The reconstructed loader matched the existing template logic byte-for-byte: same `0x7080` extent, 125 relocations, normal 28-byte entries, and the `>=4` zero-tail form. Full-card operation is 540 erases, 512 program blocks, and 512 linear verify reads.

### 39.9 Seven-ROM full-card fixture (`4_4_4_4_4_4_8MB.pcap`)

```text
8 + 4 + 4 + 4 + 4 + 4 + 4 MiB = 32 MiB
Catalog count = 7 / 7
Loader @ 0x5C2360
```

The full captured `0x7080` loader reconstructed with **0 mismatches** using the existing template, known 28-byte gap restoration, 125 relocations, seven catalog entries, and the `>=4` zero tail. Slot 8 is the normal empty sentinel.

### 39.10 Eight-ROM full-card fixture (`4_4_4_4_4_4_4_4MB.pcap`)

```text
8 x 4 MiB = 32 MiB
ROM starts = 0x0000000, 0x0400000, 0x0800000, 0x0C00000,
             0x1000000, 0x1400000, 0x1800000, 0x1C00000
Catalog count = 8 / 8
Loader @ 0x9E9C30
```

This capture is particularly important because all ROM files already total the complete 32-MiB cartridge capacity. EZ3Manager still succeeds by embedding the loader inside an internal erased `FF` run. Slot 9 is the normal empty sentinel. All observable loader bytes match the current template construction; the only unobservable portion is the USBPcap snaplen gap for this placement. Full-card verification again uses 512 linear 64-KiB reads.

### 39.11 Exact block-count sanity table

| Image extent | 64-KiB blocks | Typical captured use |
|---:|---:|---|
| 8 MiB | 128 | MegaManZ+Piano, F-Zero+Mario Kart |
| 16 MiB | 256 | 8+4+4, ordinary full 16-MiB image |
| 24 MiB | 384 | exact 24-MiB program + linear verify capture-proven |
| 32 MiB | 512 | 16+8+8, 4/5/6/7/8-entry full-card layouts, full-card image |

The relation is simply:

```text
block_count = image_bytes / 0x10000
```

for exact block-aligned extents.

---

## 40. Open questions

### 40.1 Higher menu counts

The binary loader has 120 structural 28-byte catalog slots, and captures now prove 1 through 8 active entries. The writer no longer imposes a small fixed count limit, but **menu/runtime behavior above 8 entries remains unproven**.

Future captures with 9+ ROMs are useful for extending the proven range and checking whether any UI/runtime limit appears before the 120-slot structural boundary.

### 40.2 Other partial 16-32 MiB readback geometries

Exact 24-MiB verification is now capture-proven, along with exact 16 MiB and exact 32 MiB. The Fire-Emblem-style tiny tail immediately above 16 MiB also has a dedicated capture-derived path.

Other arbitrary partial higher-window extents remain deliberately unproven and should not be generalized without captures.

### 40.3 FLASH1M

The writer maps `FLASH1M_V...` to 6, consistent with the FLASH family, but a dedicated original-manager capture and save test would strengthen this assumption.

### 40.4 EEPROM map-4 / map-5 discriminator

`EEPROM_V124` is capture-proven with **both map 4 and map 5**:

```text
Classic NES Super Mario / Castlevania -> map 4
Tales of Phantasia                    -> map 5
```

The discriminator used by original EZ3Manager remains unknown. Recovering it is
the next active engineering task, and any generic rule must come from direct
capture or controlled hardware evidence. ROM titles, save-library strings, and
assumptions are not sufficient.

The Classic NES A/B tests prove the distinction is runtime-significant rather than cosmetic catalog metadata: both titles fully verify when forced to map 5, yet both reject that configuration with the identical `GAME PACK ERROR / TURN THE POWER OFF.` screen. Map 4 works normally.

Until that discriminator is recovered, the writer requires explicit `--mapN=4` or `--mapN=5`.

### 40.5 Save-bank behavior in multi-ROM mode

ROM catalog mapping is understood much better than multi-ROM save selection. The exact way the original manager associates multiple save regions with menu entries remains a separate research area.

### 40.6 Cartridge density probing

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
   +--> identify save-library family / captured map class
   |       SRAM/other -> map 3
   |       EEPROM     -> explicit map 4 or 5
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

At shared toolset version **0.10.0**, the project has an object-oriented structural model:

- all four mainline utilities share one synchronized version; a code change in at least one utility bumps the version for the entire toolset;
- normal runtime banners do not embed the project version; from 0.7.29,
  standalone `--version` reports the shared version constant;
- cartridge geometry is four 8-MiB program/erase windows;
- 1-8 active catalog entries are capture-proven; 120 structural slots remain a safety bound rather than a proven menu limit;
- total ROM bytes may reach the full 32-MiB / 256-Mbit capacity when packing and loader placement fit;
- ROM ordering is stable largest-first and placement can reuse trailing/internal `FF`;
- the same `0x7080` multi-loader with 125 relocations matches captured 2-8 ROM configurations;
- catalog type is ROM-size based;
- catalog map uses values `3`, `4`, `5`, and `6`; EEPROM map 4 versus 5 is explicit because `EEPROM_V124` alone does not distinguish them;
- map-4 versus map-5 is now proven to be functionally significant: both Classic NES `EEPROM_V124` titles work with map 4 but display an identical `GAME PACK ERROR / TURN THE POWER OFF.` runtime screen when forced to map 5, even after byte-perfect full verification;
- partial-first-window verification, including explicit 1-/2-/4-MiB checkpoints, and exact 8-/16-/24-/32-MiB verification are capture-, transcript-, and hardware-proven for the tested layouts;
- the explicit 12-MiB partial higher-window path is capture-, transcript-, and hardware-proven through all 192 reads, menu boot, and successful launch of both games;
- the explicit 20-MiB partial higher-window path is capture-, transcript-, and hardware-proven through a no-pre-wipe write, all 320 reads, menu boot, and successful launch of both games;
- the explicit 28-MiB partial higher-window path is capture-, transcript-, and hardware-proven through all 448 reads, menu boot, and successful launch of all three games;
- the 2-MiB source-ROM checkpoint is independently hardware-proven with two single-ROM inputs, each producing a `0x200700` image, verifying 33 padded blocks through `0x210000`, and booting on real GBA hardware;
- official-ROM extraction recognizes 1/2/4/8/16/32-MiB extents through a generic trailing-`FF` heuristic; only the Golden Sun 8-MiB extraction size is hardware-qualified;
- EZ3 catalogued-ROM extraction is hardware-qualified on a two-ROM layout: ROM 1 matches its original after entry-branch reconstruction, and ROM 2 matches with its entry bytes unchanged;
- partial first-window programming rounds to `0x100` and verification to `0x10000`;
- default destructive-write reporting uses progress bars; `--verbose` restores detailed diagnostics;
- native code scope remains macOS/Linux/BSD via libusb; native Windows remains out of scope;
- official GBA cartridge classification, header inspection, and read-session
  cleanup are transcript-tested and hardware-confirmed on macOS with Golden
  Sun; guarded full-address-space extraction, correct 8-MiB trimming, trusted
  SHA-256 equality, and extracted-file boot are also hardware-confirmed, while
  the other generic trim sizes are not yet hardware-generalized;
- 0.9.0 specifically requalifies the two-ROM exact-8-MiB writer path; the
  previously hardware-proven 1-/2-/4-MiB partial-first-window checkpoints,
  exact 16-/24-/32-MiB paths, explicit partial 12-/20-/28-MiB paths, and the
  Fire-Emblem-style tiny-tail checkpoint are mechanically preserved and retain
  their historical qualification rather than being claimed as 0.9.0 reruns.

The current refactored source additionally provides RAII device ownership, an
injectable USB transport, shared protocol and read-state objects, domain models
for GBA/catalog data, explicit application services for all four utilities,
offline unit tests, and per-executable build dependency boundaries. Real-device
checkpoints completed during this refactor covered card inspection, byte-stable
save extraction, full-card wipe/blank verification, and write/full read-back
verification.

The next active engineering task is the EEPROM map-4/map-5 discriminator,
using direct capture or controlled hardware evidence only. Remaining work also
includes 9+ menu counts, save-bank behavior, additional save-library families,
and Linux/BSD hardware validation.
