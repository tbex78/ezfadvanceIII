# EZF Advance III Software Specification

## 1. Purpose

This document specifies the architecture, behavior, safety properties, and
validation requirements of the EZF Advance III command-line toolset.

The software provides an independent, capture-derived interface to an
EZ-Flash Advance III Game Boy Advance flash cartridge on modern Unix-like
systems. It reproduces only behavior supported by USB captures and real-device
tests. Unproven flash mappings and save-slot operations must not be guessed.

## 2. Supported platforms

The native targets are:

- macOS
- Linux
- FreeBSD
- OpenBSD
- NetBSD
- DragonFly BSD

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

### 3.2 Card inspector

Executable: `ezfadvanceIII_card_reader`

Responsibilities:

- initialize the bridge through the capture-proven read path;
- read and identify the EZ3 loader;
- parse single- and multi-ROM catalogs;
- select a proven linear read mapping when required;
- read and validate GBA headers;
- report catalog and ROM metadata.

The inspector is read-only. It must not send flash erase commands or ROM
program payloads.

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

### 3.4 Card eraser

Executable: `ezfadvanceIII_wipe_card`

Responsibilities:

- require the explicit `--yes-really-wipe` authorization option;
- verify cartridge readiness before sending any erase command;
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

The manager-compatible startup sequence is:

1. send `0x97` and require response `00`;
2. send `0x98` and require response `01`;
3. send `0x99` with parameter `01` and require the 13-byte command echo.

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
transitions, and 16/24/32 MiB linear-read mappings. It deliberately exposes no
erase or program operation.

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

#### `WriterOptions`

Parses writer authorization, verification, verbosity, catalog overrides, and
ROM paths independently from image construction and device access.

### 6.3 Application services

- `CartridgeImageBuilder` constructs and aligns writer images.
- `CardWriter` coordinates preflight, erase, program, cleanup, and verification.
- `CardInspector` coordinates catalog and ROM inspection.
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
regions, but the resulting programmed extent must not exceed 32 MiB.

The minimum programmed extent is 64 KiB. A larger constructed extent is
rounded up to a `0x100`-byte boundary.

## 8. Verification policy

| Image extent | Verification behavior |
|---|---|
| Below 8 MiB | Partial first-window verification |
| Exactly 8 MiB | Full verification |
| Partial 8–16 MiB | Skip as unsupported |
| Exactly 16 MiB | Full verification |
| Up to one 64 KiB block above 16 MiB | Dedicated tiny-tail verification |
| Other partial 16–24 MiB | Skip as unsupported |
| Exactly 24 MiB | Full verification |
| Partial 24–32 MiB | Skip as unsupported |
| Exactly 32 MiB | Full verification |

Unsupported verification geometry must not cause the writer to invent a read
mapping. It must perform status cleanup, report that full verification was
skipped, and distinguish that outcome from verification success.

## 9. Safety requirements

1. Dry-run writer execution must not initialize libusb or access the device.
2. Writer erase/program operations require `--yes-really-write`.
3. Full-card erase requires `--yes-really-wipe`.
4. Wipe must require the proven `0x98 -> 01` readiness response before erase.
5. Failed writer preflight must occur before any erase or program operation.
6. Read-only programs must not expose destructive methods.
7. Unsupported mappings and save configurations must fail conservatively.
8. No ROM save routine may be silently patched.
9. EEPROM map selection must remain explicit when map `4` versus `5` cannot be
   derived from evidence.
10. Resource cleanup must occur on every return path through RAII.

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
- every verification-policy boundary.

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
- initialization and read prime succeed;
- catalog and GBA header information match the programmed card;
- the process exits successfully;
- no erase or programming operation occurs.

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

- readiness succeeds before erase;
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
