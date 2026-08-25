# EZF Advance III Software Specification

**Specification baseline:** shared `0.7.23` toolset, including the guarded
official-cartridge detection and raw-extraction increment of 2026-08-19.

This specification distinguishes capture evidence, deterministic transcript
coverage, and physical-hardware confirmation. A behavior is not described as
hardware-proven merely because it compiles, passes an offline transcript, or
completes a USB transfer.

## 1. Purpose

This document specifies the architecture, behavior, safety properties, and
validation requirements of the EZF Advance III command-line toolset.

The software provides an independent, capture-derived interface to an
EZ-Flash Advance III Game Boy Advance flash cartridge on modern Unix-like
systems. It reproduces only behavior supported by USB captures and real-device
tests. Unproven flash mappings and save-slot operations must not be guessed.

The maintained scope comprises four command-line workflows: EZ3 image
construction/programming, read-only cartridge inspection and official-ROM
dumping, supported save extraction, and explicit full-card erase. It is not a
generic GBA cartridge programmer, ROM database, save converter, or native
Windows driver replacement.

## 2. Supported platforms

The native targets are:

- macOS
- Linux
- FreeBSD
- OpenBSD
- NetBSD
- DragonFly BSD

macOS on Apple Silicon is the current compile, transcript, and physical-device
validation baseline. Linux and BSD are supported source/build targets, but
their hardware qualification remains pending unless a later durable test
record states otherwise.

The project requires:

- a C++17 compiler;
- libusb 1.0 headers and library;
- a POSIX-compatible `make` and shell environment.

Native Windows builds are intentionally unsupported. Windows users may use a
Linux virtual machine with USB passthrough.

## 3. Programs

### 3.1 Multi-ROM writer

Executable: `ezfadvanceIII_multirom_writer`

Responsibilities:

- load and validate one or more GBA ROM files;
- detect known save-library markers;
- accept explicit catalog type and mapping overrides;
- construct a single- or multi-ROM cartridge image in memory;
- patch only the first physical/catalog ROM entry point;
- place and relocate the captured EZ3 loader;
- erase the required flash sectors;
- program the image in 64 KiB blocks;
- perform read-back verification when the geometry is capture-proven;
- perform status cleanup when verification is skipped or unsupported.

The writer must not access USB or modify a cartridge unless
`--yes-really-write` is supplied. Without that option, it is a dry-run image
planner.

`--skip-verify` may disable post-write comparison, but it must not disable the
final status/reset cleanup.

Writer metadata rules are:

- catalog `type` is derived from the ROM size class (`32 MiB -> 0` through
  `64 KiB -> 9`) unless explicitly overridden;
- SRAM/other ROMs use map `3` and FLASH-family ROMs use map `6` under the
  current capture-derived classifier;
- EEPROM requires an explicit per-slot map `4` or `5` because
  `EEPROM_V124` alone does not distinguish the required hardware behavior;
- FLASH/FLASH512/FLASH1M/EEPROM signatures require an interactive warning
  acknowledgement, and save routines are never patched automatically;
- `--typeN=VALUE` and `--mapN=VALUE` accept structural slots `1..120` and
  byte values `0..255`; 120 is a parser/catalog safety bound, while only
  1..8 active entries are capture-proven.

### 3.2 Card inspector

Executable: `ezfadvanceIII_card_reader`

Responsibilities:

- initialize the bridge through the capture-proven read path;
- retain the classification-relevant four-byte flash-probe responses;
- classify the inserted cartridge as EZ3 flash, official GBA ROM, or unknown;
- read and identify the EZ3 loader;
- parse single- and multi-ROM catalogs;
- select a proven linear read mapping when required;
- read and validate GBA headers;
- report catalog and ROM metadata;
- optionally extract the complete 32-MiB GBA address space from a detected
  official cartridge to a local `.gba` file;
- optionally extract one catalogued ROM from an EZ3 flash cartridge.

The inspector is read-only. It must not send flash erase commands or ROM
program payloads.

Official-cartridge classification must be based on capture-observed probe
behavior, not on an ARM branch at ROM byte zero or one game-specific reset
instruction. A recognized official cartridge must branch away before the
remaining EZ3-only `0040`, `0080`, `00C0`, and `0200` probes and before the
EZ3 manager `0x95` read-prime. Unknown changed probe behavior must fail
conservatively.

Classification alone does not authorize extraction. The first returned block
must contain the GBA fixed header byte `0x96` at offset `0xB2` and pass the
header complement checksum before extraction proceeds beyond that block.
Ordinary official-cartridge inspection must apply the same fixed-byte and
checksum requirements before labeling the cartridge confirmed and reporting
its header. Rejection must attempt the normal read-session cleanup.

The extraction interface is:

```sh
ezfadvanceIII_card_reader --extract OUTPUT.gba [--verbose]
```

It is restricted to a detected official GBA cartridge and must:

- refuse to overwrite an existing output file;
- issue 512 sequential `0x91/sub0` reads of 64 KiB each;
- validate the GBA fixed header value and complement checksum from block zero
  before requesting any later block;
- encode each command address as `byte_offset / 2`;
- read through final byte offset `0x01FF0000` and exclusive end
  `0x02000000`;
- display an in-place progress bar with percentage, completed/total MiB,
  throughput, elapsed time, and ETA;
- complete the read-session readiness transition before writing the file;
- locate the final byte that is not `0xFF` after the complete read;
- choose the smallest supported output extent—1, 2, 4, 8, 16, or 32 MiB—that
  contains that byte;
- remove only trailing `0xFF` address-space padding beyond the selected extent;
- preserve every byte within the selected output extent unchanged.

This is a deterministic erased-padding heuristic, not a reconstruction of the
original manager's size-detection algorithm. An all-`0xFF` scan or meaningful
data beyond the supported 32-MiB extent must fail without creating output.

With `--verbose`, extraction replaces the live progress bar with one diagnostic
line per 64-KiB block containing the byte address, transfer length, elapsed
seconds, and throughput, followed by total duration and average throughput.
`--verbose` without `--extract` is invalid because ordinary header/catalog
inspection has no long-running block operation to report.

EZ3 ROM extraction uses:

```sh
ezfadvanceIII_card_reader --extract-rom N OUTPUT.gba
```

The one-based ROM number must exist in the parsed catalog. The reader must
derive the inclusive stored end from the catalog type (`32 MiB >> type`), read
that complete extent, and select a proven linear mapping covering its final
address. Any intersection with the known loader extent is reconstructed as
erased `0xFF` bytes. Only physical ROM 1 has bytes 0..3 reconstructed, using
an ARM branch to its catalogued original entry target. ROM 2 and later must
retain their first four bytes unchanged. The reconstructed GBA fixed byte and
header checksum must validate before session cleanup and file creation.

The command must refuse an existing destination. This path is read-only and
does not claim exact recovery of an input file whose discarded trailing bytes
cannot be represented by the catalog size class.

### 3.3 Save reader

Executable: `ezfadvanceIII_save_reader`

Responsibilities:

- inspect the cartridge catalog;
- scan ROM allocation spans for known save-library markers;
- identify a uniquely supported save-bearing ROM;
- read capture-proven 32 KiB SRAM save data;
- write the returned bytes to a local `.sav` file.

On multi-ROM cards, the reader may auto-select a ROM only when exactly one
capture-proven `SRAM_V111` save-bearing ROM exists. It must refuse ambiguous
save-slot configurations because no general hardware save-slot switch has been
proven.

The save reader must not write save memory, erase flash, or program ROM data.
It must require `CartridgeKind::ez3_flash` after shared initialization and
before EZ3 loader, catalog, ROM-allocation, or save-bank processing. Official
and unknown cartridges must be rejected and read-session cleanup attempted.

Supported invocations are:

```sh
ezfadvanceIII_save_reader
ezfadvanceIII_save_reader --output FILE.sav
ezfadvanceIII_save_reader --rom N [--output FILE.sav]
```

The capture-proven application path is `SRAM_V111`, selector `0x0900`, and
one `0x8000`-byte read. Other SRAM signatures may be reported during scanning
but must not be exported as if their size or bank selection were proven.
`--rom` identifies catalog intent only; it must not imply that a general
multi-ROM hardware save-slot switch exists.

### 3.4 Card eraser

Executable: `ezfadvanceIII_wipe_card`

Responsibilities:

- require the explicit `--yes-really-wipe` authorization option;
- establish the full manager-compatible `0x97 -> 0x98 -> 0x99` bridge state
  and verify cartridge readiness before sending any erase command;
- erase all four physical flash windows using the captured sector geometry;
- perform final flash status cleanup;
- perform the two capture-derived blank-check reads.

The operation succeeds only if the complete erase sequence and both blank
checks succeed.

## 4. Hardware contract

USB device:

| Property | Value |
|---|---:|
| Vendor ID | `0x0E6A` |
| Product ID | `0x5088` |
| Interface | `0` |
| Bulk OUT endpoint | `0x02` |
| Bulk IN endpoint | `0x81` |

Tested flash geometry:

| Property | Value |
|---|---:|
| Capacity | 32 MiB / 256 Mbit |
| Program block | 64 KiB / `0x10000` |
| Flash window | 8 MiB / `0x800000` |
| Window count | 4 |

Physical windows:

| Window | Physical range | Setup value |
|---:|---|---:|
| 0 | `0x0000000..0x07FFFFF` | base/default |
| 1 | `0x0800000..0x0FFFFFF` | `0x0040` |
| 2 | `0x1000000..0x17FFFFF` | `0x0080` |
| 3 | `0x1800000..0x1FFFFFF` | `0x00C0` |

Program-window state and read-mapping state are distinct. Code must not assume
that selecting a programming window changes the physical region returned by a
local `0x91` read.

The geometry in this section describes the tested EZ3 flash cartridge. An
official GBA cartridge is read through the adapter but is not assumed to share
EZ3 erase/program geometry, and no destructive operation is exposed for it.

## 5. USB protocol contract

Known command families:

| Command | Purpose |
|---:|---|
| `0x91` | ROM or save read |
| `0x92` | flash command/program transaction |
| `0x95` | manager read-prime state |
| `0x96` | sector erase |
| `0x97` | bridge startup |
| `0x98` | cartridge readiness |
| `0x99` | bridge large-transfer initialization |

Known `0x91` subcommands used by the maintained tools are:

| Subcommand | Use |
|---:|---|
| `0x00` | word-addressed ROM/card data read |
| `0x01` | capture-proven save-memory read |
| `0x02` | four-byte cartridge/flash probe read |

The manager-compatible startup sequence is:

1. send `0x97` and require response `00`;
2. send `0x98` and require response `01`, retrying an explicit transient `00`
   up to five checks at 100-ms intervals;
3. send `0x99` with parameter `01` and require the 13-byte command echo.

After card inspection or a successfully initialized save-reader operation, the
read-only tool performs three `0x98 -> 01` polls and a 1000-ms quiet interval.
This is a readiness transition, not a substitute for destructive tools
establishing their own full manager-compatible startup state.

The bounded readiness polls and read-only epilogue are owned by the narrow
`ReadSessionTransition` component and covered by deterministic transport and
delay-callback transcript tests.

Read-only initialization preserves two classification-relevant four-byte
`0x91/sub2` responses. A post-flash-ID value of `1C 00 B8 00` or
`1C 00 B9 00` selects the known EZ3 path. An unchanged value across the
captured flash-ID attempt selects the official-ROM path. Any other changed
response is unknown and fails before later EZ3-only probing. This decision is
based on probe behavior; it must not classify from an ARM branch or from the
captured Golden Sun word `EE 00 00 EA` alone.

For manager-style command/data transactions, the implementation preserves the
legacy 750 microsecond command-to-data delay. The wipe workflow retains its
separately captured timing behavior and must not inherit a delay that was not
present in its source capture.

All bulk transfers must validate libusb status. Exact-length reads and writes
must also validate the transferred byte count.

## 6. Architecture

### 6.1 Layers

```text
Command-line programs
        |
Application services
        |
Cartridge and image domain objects
        |
EZ3 protocol and read state machines
        |
Abstract transport
        |
libusb device/session ownership
```

### 6.2 Core objects

#### `UsbDevice`

Owns the libusb context, device handle, and claimed interface. Its destructor
must release the interface, close the handle, and exit the context. The object
is non-copyable and non-movable.

#### `Transport`

Abstract interface for bulk input and output. It permits protocol tests without
physical hardware.

#### `BulkTransport`

Non-owning libusb implementation of `Transport`. It centralizes endpoints,
transfer validation, timeouts, and diagnostics.

#### `Protocol`

Encodes shared EZ3 `0x92` commands and implements command/data/echo
transactions. It accepts either a real or test transport.

#### `ReadOnlyCartridge`

Owns the capture-derived startup, flash probing, chunked ROM reads, status
transitions, explicit cartridge classification, and 16/24/32 MiB linear-read
mappings. Its four-byte `0x91/sub2` helper preserves probe data for the
classification decision. It deliberately exposes no erase or program
operation.

#### `CartridgeFormat`

Provides pure binary-format operations, including little-endian decoding,
ASCII cleanup, GBA header checksum validation, and ARM branch-target decoding.

#### `GbaHeader`

Models parsed GBA title, game code, maker code, ROM version, readability, and
checksum state.

#### `CatalogEntry`

Models an EZ3 catalog entry and validates its decoded address against a caller-
provided image limit.

#### `SaveMemoryReader`

Owns capture-proven save-bank selection and 32/64 KiB save reads. Current
application policy permits only the proven 32 KiB `SRAM_V111` path.

#### `VerificationPolicy`

Maps an image extent to a capture-supported verification mode without sending
USB traffic.

#### `VerificationSession`

Owns transport-injectable post-program verification operations without choosing
the verification geometry. The partial first-window operation emits only the
capture-proven status sequence and global-linear `0x91` reads; its 1-, 2-, and
4-MiB checkpoints are hardware-proven. The exact 8-MiB operation preserves its
explicit `0x0040` mapping transition and captured 125-ms delay. Exact 16-,
24-, and 32-MiB, explicit partial 12-/20-/28-MiB, and
tiny-tail-above-16-MiB operations remain explicit capture-derived state
machines. Every currently selected mode has a deterministic transcript
fixture.

#### `WriterOptions`

Parses writer authorization, verification, verbosity, catalog overrides, and
ROM paths independently from image construction and device access.

### 6.3 Application services

- `CartridgeImageBuilder` constructs and aligns writer images.
- `CardWriter` coordinates preflight, erase, program, cleanup, and verification.
- `CardInspector` coordinates catalog and ROM inspection.
- The official-ROM extraction branch coordinates a fixed 32-MiB read,
  read-session cleanup, and local `.gba` creation.
- `SaveExtractor` coordinates ROM selection and local save-file creation.
- `CardEraser` coordinates readiness, erase, cleanup, and blank verification.

Application services use composition. The four tools do not inherit from a
shared program base class.

## 7. Image construction

A generated cartridge image contains:

1. one or more GBA ROM images;
2. a relocated EZ3 loader/menu;
3. catalog entries embedded in the loader;
4. a patched ARM branch in physical/catalog ROM 1;
5. erased `0xFF` padding and reusable erased regions.

Only ROM 1 receives the loader branch patch. Later ROM entry points remain
unchanged.

Multi-ROM inputs are stable-sorted by descending file size. Equal-size ROMs
retain their input order. The builder may reuse suitable trailing `0xFF`
regions and internal erased runs, but it must reject overlap with meaningful
ROM bytes and the resulting programmed extent must not exceed 32 MiB.

Single-ROM images use the capture-derived `0x660` loader. Multi-ROM images use
the capture-derived `0x7080` loader and its relocation set, except that the
capture-exact two-ROM extent is `0x6F80`. Four-or-more-ROM images apply the
captured final 26-byte zero tail. The loader may be appended or embedded in a
sufficiently large erased run; only physical/catalog ROM 1 is patched to branch
to it.

The minimum programmed extent is 64 KiB. A larger constructed extent is
rounded up to a `0x100`-byte boundary.

## 8. Verification policy

| Image extent | Verification behavior |
|---|---|
| Below 8 MiB | Partial first-window verification |
| Exactly 8 MiB | Full verification |
| Exactly 12 MiB | Capture-, transcript-, and hardware-proven partial higher-window verification |
| Other partial 8–16 MiB | Skip as unsupported |
| Exactly 16 MiB | Full verification |
| Up to one 64 KiB block above 16 MiB | Dedicated tiny-tail verification |
| Exactly 20 MiB | Capture-, transcript-, and hardware-proven partial higher-window verification |
| Other partial 16–24 MiB | Skip as unsupported |
| Exactly 24 MiB | Full verification |
| Exactly 28 MiB | Capture-, transcript-, and hardware-proven partial higher-window verification |
| Other partial 24–32 MiB | Skip as unsupported |
| Exactly 32 MiB | Full verification |

Unsupported verification geometry must not cause the writer to invent a read
mapping. It must perform status cleanup, report that full verification was
skipped, and distinguish that outcome from verification success.

New capture evidence exists for representative 12-, 20-, and 28-MiB partial
higher-window verification using the simple global-linear prefix. Versions
0.7.20, 0.7.22, and 0.7.23 select only the explicit 12-, 20-, and 28-MiB
checkpoints, each backed by a deterministic transcript fixture. The 12-MiB
hardware test completed all 192 reads through final offset `0x00BF0000`,
booted the menu, and launched both games. The 20-MiB hardware test, performed
without a preliminary full-card wipe, completed all 320 reads through
`0x013F0000`, booted the menu, and launched both games. The 28-MiB hardware
test completed all 448 reads through `0x01BF0000`, booted the menu, and
launched all three games. The existing tiny-tail exception remains unchanged.

The 2-MiB source-ROM checkpoint has two independent single-ROM hardware runs.
Each source is exactly `0x200000` bytes, but the appended loader makes the
constructed/programmed image `0x200700` bytes. Partial-first-window verification
rounds that image to `0x210000`, performs 33 64-KiB reads, and finishes with the
block at `0x200000`; both images booted on real GBA hardware. This evidence
must not be described as an exactly 2-MiB constructed image or as proof of
physical 2-MiB official-cartridge extraction.

## 9. Safety requirements

1. Dry-run writer execution must not initialize libusb or access the device.
2. Writer erase/program operations require `--yes-really-write`.
3. Full-card erase requires `--yes-really-wipe`.
4. Wipe must complete `0x97 -> 00`, require the proven `0x98 -> 01` readiness
   response, and validate the `0x99` parameter-`01` echo before erase; an
   explicit transient `00` may be retried under the bounded startup policy.
5. Failed writer preflight must occur before any erase or program operation.
6. Read-only programs must not expose destructive methods.
7. Unsupported mappings and save configurations must fail conservatively.
8. No ROM save routine may be silently patched.
9. EEPROM map selection must remain explicit when map `4` versus `5` cannot be
   derived from evidence.
10. Resource cleanup must occur on every return path through RAII.
11. Official-ROM extraction must refuse an existing destination and must not
    create the output until the complete read and read-session cleanup succeed.
12. A successful USB read-back does not by itself prove correct catalog map,
    menu launch, game behavior, or save behavior on a GBA.

## 10. Build contract

Build all programs:

```sh
make
```

Perform syntax checks:

```sh
make check
```

Run offline tests:

```sh
make test
```

Run stricter compiler diagnostics:

```sh
make check WARNFLAGS="-Wall -Wextra -Wpedantic"
```

Command-line `CPPFLAGS`, `CXXFLAGS`, `WARNFLAGS`, `LDFLAGS`, and `LDLIBS`
overrides must remain usable. The internal `-Iinclude` path must remain active
when callers override `CPPFLAGS`.

## 11. Offline test requirements

Offline tests must not require or access a USB device.

The test suite covers:

- little-endian binary decoding;
- ARM branch-target decoding;
- GBA header parsing and checksum validation;
- EZ3 catalog parsing, address decoding, plausibility, and bounds errors;
- exact `0x92` command construction;
- command/data transport ordering and timeout propagation;
- immediate, delayed, failed, and epilogue read-session transitions;
- official-cartridge probe classification and the absence of later EZ3-only
  operations after the official branch;
- a synthetic 512-block official-ROM extraction through final word address
  `0x00FF8000` using block-varying data;
- every verification-policy boundary;
- exact transcripts for partial-first-window (including the 1/2/4-MiB
  checkpoints), explicit partial 12/20/28 MiB, exact 8/16/24/32 MiB, and
  tiny-tail-above-16-MiB verification;
- writer option parsing, including multi-digit structural catalog slots.

Writer dry-run regression comparisons should use the preserved pre-migration
binary when available. Output and exit status must remain identical, excluding
the executable path printed in usage text.

## 12. Hardware acceptance tests

Hardware testing is manual and must be performed at explicit checkpoints.

### Read-only inspection

```sh
./ezfadvanceIII_card_reader
```

Acceptance criteria:

- interface claim succeeds;
- initialization succeeds and the appropriate EZ3 or official-ROM branch is
  selected;
- the EZ3 manager read-prime succeeds when inspecting EZ3 flash;
- catalog and GBA header information match the programmed card;
- the process exits successfully;
- no erase or programming operation occurs.

For an official cartridge, acceptance additionally requires:

- classification selects the official-ROM branch;
- the displayed title, game code, maker code, version, and header checksum
  match the cartridge;
- no EZ3 loader/catalog interpretation or manager `0x95` prime occurs;
- the three-poll/1000-ms read-session cleanup succeeds.

### Official ROM extraction

```sh
./ezfadvanceIII_card_reader --extract /tmp/official-card.gba
```

Acceptance criteria:

- the inserted cartridge is classified as an official GBA ROM;
- exactly 512 sequential 64-KiB reads complete;
- the device scan is exactly `0x02000000` bytes;
- the output is exactly 2, 4, 8, 16, or 32 MiB according to the last-non-`FF`
  rounding rule;
- populated ROM regions match a trusted dump or hash;
- session cleanup succeeds before file creation;
- an existing destination is not overwritten;
- no erase, save write, or ROM programming operation occurs.

### Save extraction

```sh
./ezfadvanceIII_save_reader --output /tmp/test.sav
```

Acceptance criteria for a supported card:

- exactly one supported save ROM is selected;
- exactly 32 KiB is returned;
- repeated reads produce identical hashes if save memory is unchanged;
- no write, erase, or ROM programming command occurs.

### Card wipe

```sh
./ezfadvanceIII_wipe_card --yes-really-wipe
```

Acceptance criteria:

- full `0x97 -> 0x98 -> 0x99` startup and readiness succeed before erase;
- all four windows are erased;
- cleanup succeeds;
- both captured blank-check reads contain only `0xFF`.

### Write and verification

```sh
./ezfadvanceIII_multirom_writer --yes-really-write [options] rom1.gba ...
```

Acceptance criteria:

- dry-run layout was reviewed first;
- preflight succeeds before erase;
- erase and programming finish without transfer errors;
- a supported geometry completes full byte comparison;
- the final status is `WRITE + FULL READ-BACK VERIFICATION SUCCEEDED.`;
- card inspection reports the expected catalog afterward.

## 13. Change-control rules

Changes to protocol bytes, command order, transfer sizes, delays, flash-window
selection, erase geometry, loader relocation, or verification mappings require
both:

1. an offline regression test or transcript fixture; and
2. an explicit real-device checkpoint before the change is considered proven.

Pure parsing, CLI, and policy changes require offline tests and output
regression checks. A hardware test is additionally required when such a change
alters the code path that interprets live cartridge bytes.

Experimental behavior must remain separate from the mainline implementation
until supported by captures and real-device evidence.

## 14. Evidence and support status

The current durable support boundary is:

| Area | Capture | Transcript | Hardware |
|---|---|---|---|
| EZ3 inspection and catalog parsing | yes | partial/pure parsing | yes |
| Official-ROM detection and header inspection | yes | yes | yes, Golden Sun on macOS |
| Official full scan with 1/2/4/8/16/32-MiB trailing-`FF` sizing | scan yes / generic sizing heuristic | yes | 8 MiB only: Golden Sun trim/hash/boot on macOS; 1/2/4/16/32-MiB sizing not yet hardware-generalized |
| Partial first-window verification, including 1/2/4-MiB checkpoints | yes | yes | yes |
| Exact 8/16/24/32-MiB verification | yes | yes | yes |
| Tiny tail immediately above 16 MiB | yes | yes | yes |
| Representative 12/20/28-MiB verification | yes | yes | yes |
| Other arbitrary partial higher extents | no | no | no |
| `SRAM_V111` 32-KiB save extraction | yes | protocol component coverage | yes |
| EEPROM map-4/map-5 generic discriminator | incomplete | no | explicit override required |
| More than 8 active menu entries | structural slots only | parser bound | pending |
| Linux/BSD physical hardware operation | applicable | build target | pending |

Known open boundaries include the original manager's official-ROM size
algorithm, general EZ3 density detection, multi-ROM save-bank switching,
EEPROM map-4 versus map-5 discrimination, FLASH1M-specific save-cycle
validation, menu behavior above eight active entries, and Linux/BSD hardware
qualification. EEPROM map-4/map-5 discrimination is the next active engineering
task; a generic rule requires direct capture or controlled hardware evidence
and must not be inferred from ROM titles, save-library strings, or assumptions.
