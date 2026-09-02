# EZF Advance III Software Specification

**Specification baseline:** shared `0.19.2` toolset, including guarded
official-cartridge extraction and hardware-proven EZ3 catalogued-ROM
extraction.

This specification distinguishes capture evidence, deterministic transcript
coverage, and physical-hardware confirmation. A behavior is not described as
hardware-proven merely because it compiles, passes an offline transcript, or
completes a USB transfer.

## 1. Purpose

This document specifies the architecture, behavior, safety properties, and
validation requirements of the EZF Advance III command-line toolset.

The software provides an independent, capture-derived interface to an
EZ-Flash Advance III Game Boy Advance flash cartridge on modern desktop
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
- Windows 10
- Windows 11

macOS on Apple Silicon is the current compile, transcript, and physical-device
validation baseline. Linux CI covers the Makefile build. Version 0.13.0 added a
CMake/MSVC Windows build and offline-test target; version 0.13.1 disables the
Win32 `min`/`max` macros for every Windows target. Windows CI and physical USB
qualification must be recorded before calling a particular binary release
hardware-proven.

The project requires:

- a C++17 compiler;
- libusb 1.0 headers and library;
- either a POSIX-compatible `make`/shell environment or CMake 3.20+;
- on Windows, a libusb-compatible WinUSB/libusbK device-driver association.

The Unix Makefile remains supported and unchanged in purpose. Windows 10/11
uses CMake with MSVC or MinGW-w64; no Windows-specific USB protocol sequence is
introduced.

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
- SRAM/other ROMs use map `3`, FLASH/FLASH512 ROMs use map `6`, and FLASH1M
  ROMs use map `7` under the capture-derived classifier;
- EEPROM map selection uses a structurally recovered Nintendo SDK capacity
  argument when present: 4-Kbit/512-byte EEPROM selects map `4`, while
  64-Kbit/8-KiB EEPROM selects map `5`. Marker revision and ROM size are not
  substitutes; unresolved call structures still require an explicit map;
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

`--dump` is an exact alias for `--extract` in both official-cartridge and EZ3
catalogued-ROM workflows.

It is restricted to a detected official GBA cartridge and must:

- refuse to overwrite an existing output file;
- issue 512 sequential `0x91/sub0` reads of 64 KiB each;
- validate the GBA fixed header value and complement checksum from block zero
  before requesting any later block;
- encode each command address as `byte_offset / 2`;
- read through final byte offset `0x01FF0000` and exclusive end
  `0x02000000`;
- display an in-place progress bar with percentage, completed/total MiB,
  throughput, elapsed time, and ETA. On an interactive terminal, each update
  must clear and replace one physical line and must be clipped to the detected
  terminal width to prevent wrapping;
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
`--verbose` without `--extract` or `--dump` is invalid because ordinary header/catalog
inspection has no long-running block operation to report.

EZ3 ROM extraction uses:

```sh
ezfadvanceIII_card_reader --extract N OUTPUT.gba [--verbose]
```

The equivalent `--dump N OUTPUT.gba [--verbose]` form uses the same parsed
request and execution path.

The one-based ROM number must exist in the parsed catalog. When the command is
invoked as `--extract OUTPUT.gba` and the detected cartridge is EZ3 flash, the
reader must select catalog ROM 1. On an official cartridge, that same form
retains the official full-address-space extraction behavior. The reader must
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

Default EZ3 extraction reporting must use the same progress-bar presentation
as official-cartridge extraction. `--verbose` must replace it with per-block
address, timing, and throughput diagnostics without changing transferred or
written bytes.

### 3.3 Save reader/writer

Executable: `ezfadvanceIII_save_reader`

Responsibilities:

- inspect the cartridge catalog;
- scan ROM allocation spans for known save-library markers;
- require either an explicit catalog-ROM choice or a physical save-bank choice
  for multi-ROM layouts;
- read supported 32 KiB SRAM and 64 KiB FLASH512 save data, plus explicitly
  selected direct ranges of one through four banks;
- write extracted bytes to a local `.sav` file;
- back up, write, and verify supported 32 KiB SRAM and 64 KiB FLASH512 data,
  plus explicitly selected direct ranges of one through four banks.

On single-ROM cards, the reader selects ROM 1 automatically. On multi-ROM
cards, it must display the catalog and require `--rom N` unless an explicit
`--save-bank` requests direct physical-bank access; it must not infer a ROM
choice even when exactly one `SRAM_V111` marker is present. A selected ROM
must identify a supported `SRAM_V111` or `FLASH512` entry. Missing or mismatched candidates,
unsupported save formats, unknown predecessor capacity, and cumulative layouts
larger than four banks must be refused.

With no arguments, the layout summary must be presented before save-marker
scanning and visible per-ROM progress must update on one terminal line. When
arguments are supplied, layout and scan-progress presentation must be
suppressed. The analyzer must inspect only the selected ROM and any preceding
entries needed for cumulative bank allocation; it must not scan unrelated
following ROMs.

Save writing must require `--write FILE` and `--yes-really-write`.
`--backup FILE` is recommended but optional. If it is omitted, the tool must
warn that the existing save may become unrecoverable and require an explicit
interactive `y` or `yes` response before opening the USB device. Any other
response, including end-of-input, must abort safely. Catalog-selected input must equal the selected ROM's
32768- or 65536-byte allocated capacity. Direct-bank input must contain one
through four complete 32768-byte banks and fit within `0x0900` through
`0x0930`. When supplied, the backup path must not already exist and the current
save must be read and the backup completed before the first save-write payload
is sent. Without a backup path, the current save is still read before writing.
A bounded readiness transition is
required before and after the captured `0x92/01` write. The tool must then read
the full save through the proven `0x91/01` path. The read-back must match every
input byte. A mismatch is failure,
not partial success. The save tool must never erase flash or program ROM data.

Immediate same-session verification is distinct from persistence across a
completed session and later initialization. In the 0.11.0 FFTA hardware test,
the immediate 65536-byte comparison passed, while a fresh extraction differed
only at offsets 0 and 1 (`FF FF` became `00 04`). The software must not silently
normalize or ignore those bytes until the mutation's cause and meaning are
proven.

Controlled tests isolated the mutation to one-byte `0x92` transfers in the
linear mapping sequence: selector/value `1/04` writes save offset 1, while
`0/00`, `0/AA`, and `0/55` write save offset 0. The production read mapping
must therefore use only the proven two-byte control transfers. A staged
tx92Two-only 16-to-24-MiB transition exposed the genuine two-ROM FFTA/DumpRom
catalog while preserving all 32768 bytes of bank `0x0900`.
The production save tool was then hardware-qualified end to end: it wrote both
FFTA banks, passed immediate byte-for-byte verification, and a separate fresh
invocation extracted a 65536-byte file whose SHA-256 exactly matched the input.
After another complete FFTA write, the adjacent DumpRom bank at `0x0920` also
retained its trusted SHA-256 across a fresh extraction.
Shared read-only ROM and save discovery must follow the same rule: no
`tx92One` mapping or status transaction is permitted. Writer post-program
verification is a separate state transition. Where a direct manager capture
requires one-byte status or selector operations to expose the correct flash
window, the writer must reproduce that exact sequence and must subsequently
clear and verify all four known save banks. Those writer-only operations must
never be reused by read-only applications.

For the Fire Emblem tiny-tail geometry, the final USB read remains a complete
64-KiB transport block, but comparison stops at the constructed image end.
The capture erases and programs only the small sector containing the loader;
bytes in later sectors of the rounded read block are outside the image and are
not required to be `0xFF`.
Because capture-required erase/program window selection itself uses one-byte
transactions, the writer must clear all four save banks again after the entire
ROM workflow. This final clear is attempted after both success and any failure
occurring after the initial global setup.
The corrected ordering is hardware-qualified with the established exact-8-MiB
two-ROM workflow; afterward, all four 32-KiB banks were independently verified
to contain only zero bytes.

The four observed 32-KiB banks use selectors `0x0900`, `0x0910`, `0x0920`, and
`0x0930`. The software's cumulative catalog-order allocator is policy, not a
fully capture-proven protocol rule. It reserves one bank for SRAM/EEPROM, two
for FLASH512/FLASH, and four for FLASH1M according to marker-derived capacity.
`FLASH1MB.pcap` directly proves `BPEF` with `FLASH1M_V103` is catalogued with
map `7` and that the original manager clears selectors `0x0900` through
`0x0930` before programming it. A complete 128-KiB save import/export cycle
remains to be hardware-qualified.
Only the observed `FLASH512` FFTA allocation at `0x0900`/`0x0910`, followed by
`SRAM_V111` DumpRom at `0x0920`, is hardware-qualified. EEPROM predecessors,
generic FLASH predecessors, and FLASH1M predecessors still require direct
manager captures or controlled hardware validation. Unknown predecessor
capacity and total allocation beyond four banks must be refused.
This deliberately corrects the original manager's standalone-import behavior,
which restarts a later save at `0x0900` and can overwrite a predecessor.

It must require `CartridgeKind::ez3_flash` after shared initialization and
before EZ3 loader, catalog, ROM-allocation, or save-bank processing. Official
and unknown cartridges must be rejected and read-session cleanup attempted.

Supported invocations are:

```sh
ezfadvanceIII_save_reader
ezfadvanceIII_save_reader --output FILE.sav
ezfadvanceIII_save_reader --rom N [--output FILE.sav]
ezfadvanceIII_save_reader --rom N --save-bank 0x09X0 [--output FILE.sav]
ezfadvanceIII_save_reader --save-bank 0x09X0 [--output FILE.sav]
ezfadvanceIII_save_reader --save-bank 0x09X0 --consecutive-bank N \
    [--output FILE.sav]
ezfadvanceIII_save_reader --write FILE.sav --backup ORIGINAL.sav \
    --yes-really-write [--rom N]
ezfadvanceIII_save_reader --write FILE.sav --backup ORIGINAL.sav \
    --yes-really-write --save-bank 0x09X0 [--consecutive-bank N]
ezfadvanceIII_save_reader --erase [--backup ORIGINAL.sav]
ezfadvanceIII_save_reader --erase --save-bank 0x09X0 \
    [--consecutive-bank N] [--backup ORIGINAL.sav]
```

The supported application paths are `SRAM_V111` with one 32-KiB bank and
`FLASH512` with two consecutive 32-KiB banks, cumulatively allocated from
`0x0900` through `0x0930`. Other SRAM
signatures may be reported during scanning but must not be exported as if their
size or bank selection were proven.
`--rom` identifies catalog intent and selects the corresponding cumulatively
allocated bank. Catalog inspection and signature scanning may use the shared
capture-proven 16-/24-/32-MiB linear mappings, but
must remain bounded by the physical 32-MiB cartridge image.

`--save-bank SELECTOR` explicitly overrides the policy-derived selector for
investigation and recovery. It accepts only the four observed selectors
`0x0900`, `0x0910`, `0x0920`, and `0x0930`. When `--rom N` is also present, ROM
selection determines whether 32768 or 65536 bytes are accessed. Without
`--rom`, extraction reads the selected 32768-byte physical bank directly, and
save writing derives a one- through four-bank extent from the input file. The
tool must reject any direct range whose final bank would exceed `0x0930`.
Direct physical-bank access must not run ROM initialization, select a ROM
mapping, or require catalog classification.

For direct access, `--consecutive-bank N` accepts values 1 through 4 and reads
or writes `N * 32768` bytes beginning at `--save-bank`. It requires
`--save-bank`, cannot be combined with `--rom`, and must reject any range whose
last selector would exceed `0x0930`. For writing, the input size must exactly
match the requested count. Without this option, direct writing derives the
count from an exact 32-/64-/96-/128-KiB input.

`--erase` directly fills save memory with zero bytes and verifies the complete
range by read-back. Without a selector it clears all four banks. With
`--save-bank`, it clears one bank by default or the explicit 1–4-bank
`--consecutive-bank` range. An optional `--backup` must read and preserve that
exact range before the first zero write and must refuse to overwrite an
existing file. Without `--backup`, erase must display a destructive warning
and accept only interactive `y`/`yes`; refusal or unavailable input must abort
before USB access. It must reject ranges beyond `0x0930` and cannot be combined
with extraction, ROM selection, or save writing.

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

#### `Ez3CatalogParser`

Purely interprets the capture-derived single- and multi-ROM loader catalog
structures into `Ez3CatalogLayout` and `CatalogEntry` values. It accepts an
image bound from its caller but does not authorize tool-specific evidence
policy. The card reader and save reader apply their different supported-count
and address policies after parsing.

#### `GbaHeader`

Models parsed GBA title, game code, maker code, ROM version, readability, and
checksum state.

#### `CatalogEntry`

Models an EZ3 catalog entry and validates its decoded address against a caller-
provided image limit.

#### `SaveCatalogAnalyzer`

Owns save-oriented inspection after catalog discovery: mapping through the
highest allocation address, allocation-span calculation, GBA-header reads, and
chunked save-marker detection. It does not select a ROM, allocate save banks,
present output, or access save memory.

#### `SaveMemoryReader`

Owns capture-proven save-bank selection and consecutive 32-KiB save reads.
Catalog policy permits 32 KiB `SRAM_V111` and 64 KiB `FLASH512`; explicit
direct access permits one through four banks.

#### `SaveMemoryWriter`

Owns the capture-derived eight-transfer save-bank selection sequence and the
exact 32 KiB `0x92/01` command, payload, and command-echo transaction. Direct
32-/64-/96-/128-KiB writes are split into one through four consecutive bank
transactions. It is
transport-injectable so the destructive transcript can be tested offline.

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

#### `CardReaderOptions`

Parses the reader's inspection, numbered/default extraction, output, and
verbosity forms independently from device access. Its cartridge/action
decision keeps official extraction distinct from EZ3 ROM selection.

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

`CartridgeImageBuilder` is a pure, separately compiled component. It owns ROM
ordering and placement, captured loader assets and relocation, catalog bytes,
the physical-ROM-1 branch patch, and final programmed extent calculation. It
does not own authorization, USB sessions, erase/program commands, or
verification selection.

`CardWriter` is a separately compiled workflow coordinator. It owns the fixed
preflight, bridge initialization, erase, program, cleanup, and evidence-bounded
verification ordering. A `WriterBackend` boundary keeps capture-specific USB
operations injectable so orchestration and failure short-circuiting are tested
without hardware. Offline coverage must dispatch every supported verification
geometry, exercise unsupported-geometry and `--skip-verify` cleanup, and prove
that failures stop subsequent backend operations.

The concrete `LibusbWriterBackend` is also separately compiled and obtained
through a factory returning `WriterBackend`. It owns capture-derived bridge
initialization, flash setup, erase/program transactions, progress reporting,
and delegation to the explicitly named verification sessions. The writer CLI
must not duplicate those device operations.

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
9. EEPROM map selection may be automatic only when ROM structure proves the
   SDK capacity argument. It must remain explicit when the structure is
   unresolved or contradictory.
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
- shared single-ROM and 2/3/8/120-entry catalog layouts, duplicated-count
  validation, truncation, alignment, packed-start, and address-bound failures;
- whole-image hash fixtures for single, two-, three-, and eight-ROM writer
  layouts and single-ROM internal loader placement;
- EZ3 stored extents, loader-overlap blanking, ROM-1 entry reconstruction, and
  8/16/24/32-MiB linear-read mapping thresholds;
- card-reader option parsing and official/EZ3 extraction routing, including
  unnumbered EZ3 default selection and numbered extraction;
- exact `0x92` command construction;
- command/data transport ordering and timeout propagation;
- immediate, delayed, failed, and epilogue read-session transitions;
- official-cartridge probe classification and the absence of later EZ3-only
  operations after the official branch;
- a synthetic 512-block official-ROM extraction through final word address
  `0x00FF8000` using block-varying data;
- official-ROM reads whose final transfer is shorter than 64 KiB;
- every verification-policy boundary;
- exact transcripts for partial-first-window (including the 1/2/4-MiB
  checkpoints), explicit partial 12/20/28 MiB, exact 8/16/24/32 MiB, and
  tiny-tail-above-16-MiB verification;
- writer option parsing, including multi-digit structural catalog slots;
- the shared `--version` request and stable version-output format.

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
| EZ3 catalogued-ROM extraction | manager/builder layout evidence | entry reconstruction unit test | yes, ROM 1 and ROM 2 from a two-ROM card matched their originals by SHA-256 on macOS |
| Official-ROM detection and header inspection | yes | yes | yes, Golden Sun on macOS |
| Official full scan with 1/2/4/8/16/32-MiB trailing-`FF` sizing | scan yes / generic sizing heuristic | yes | 8 MiB only: Golden Sun trim/hash/boot on macOS; 1/2/4/16/32-MiB sizing not yet hardware-generalized |
| Partial first-window verification, including 1/2/4-MiB checkpoints | yes | yes | yes |
| Exact 8/16/24/32-MiB verification | yes | yes | yes |
| Tiny tail immediately above 16 MiB | yes | yes | yes |
| Representative 12/20/28-MiB verification | yes | yes | yes |
| Other arbitrary partial higher extents | no | no | no |
| `SRAM_V111` 32-KiB save extraction | yes | protocol component coverage | yes |
| EEPROM map-4/map-5 structural discriminator | TOF direct 64-Kbit initialization and Super Monkey wrapped 4-Kbit initialization recognized | focused fixtures | automatic Super Monkey map 4 and TOF map 5 both boot and save on hardware; unresolved ROMs retain explicit override |
| More than 8 active menu entries | structural slots only | parser bound | pending |
| Linux/BSD physical hardware operation | applicable | build target | pending |

Known open boundaries include the original manager's official-ROM size
algorithm, general EZ3 density detection, multi-ROM save-bank switching,
EEPROM call structures not covered by the structural detector,
FLASH1M-specific save-cycle validation, menu behavior above eight active
entries, and Linux/BSD hardware qualification. The known samples correlate a
512-byte save with map 4 and an 8-KiB save with map 5, but `EEPROM_V124` itself
carries no capacity. Version 0.12.0 follows only a recognized SDK initializer;
it never infers capacity from ROM title, ROM size, or the library string.
