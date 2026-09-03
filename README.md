# EZF Advance III open-source alternative software

![EZF Advance III](ezadvanceIII_image1.jpg) ![EZF Advance III](ezadvanceIII_image2.jpg)

This repository provides four native C++17/libusb command-line tools for the
32-MiB / 256-Mbit EZF Advance III cartridge. The current synchronized toolset
version is **0.19.5**.

See the [project summary](ezfadvanceIII_project_summary.md),
[software specification](SOFTWARE_SPECIFICATION.md), and
[technical documentation](ezfadvanceIII_project_technical_documentation.md)
for the evidence history and protocol details.

Markdown files named for older releases, reviews, recommendations, or test
plans are retained as historical snapshots. Their words such as “current,”
“pending,” and “next” describe the named review point, not the present 0.19.5
support boundary.

## Build and test

The command-line tools require a C++17 compiler and libusb 1.0:

```sh
make
```

Offline tests do not access an EZ-Flash device and do not erase or program a
cartridge:

```sh
make test
```

The existing Makefile remains the native build path for macOS, Linux, FreeBSD,
OpenBSD, NetBSD, and DragonFly BSD. The same targets may use CMake:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

Windows 10/11 uses CMake, libusb from vcpkg, and a WinUSB/libusbK-compatible
driver for the EZF Advance III USB interface:

```powershell
vcpkg install libusb:x64-windows
cmake -S . -B build/windows -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
```

Installing a replacement USB driver changes how Windows associates with this
device; record the existing driver first if the original manager must remain
usable. macOS on Apple Silicon remains the physical-hardware baseline. Linux,
BSD, and Windows physical-device qualification remains pending.

The code is organized in layers:

- `UsbDevice` owns the libusb context, device handle, and claimed interface.
- `Transport` abstracts bulk transfers; `BulkTransport` is the libusb implementation.
- `Protocol` implements shared EZ3 command/data/echo transactions.
- `ReadOnlyCartridge` owns the capture-derived initialization and ROM-read state machine.
- `GbaHeader`, `CatalogEntry`, and `CartridgeFormat` model cartridge metadata.
- `SaveCatalogAnalyzer` owns save-oriented ROM header, allocation-span, and marker inspection.
- `SaveMemoryReader` owns capture-proven save-bank reads.
- `VerificationSession` owns all capture-supported post-program verification paths: partial first-window, explicit partial 12/20/28 MiB, exact 8/16/24/32 MiB, and tiny-tail-above-16-MiB.
- `LibusbWriterBackend` owns the capture-derived destructive USB implementation behind a factory returning the `WriterBackend` interface.
- `CartridgeImageBuilder`, `CardWriter`, `CardInspector`, `SaveExtractor`, and `CardEraser` implement the four application workflows.

## Command-line tools

### Inspect a cartridge

```sh
./ezfadvanceIII_card_reader
```

The reader distinguishes capture-supported EZ3 flash behavior from an ordinary
official GBA cartridge. An official cartridge is reported as confirmed only
when its GBA fixed header byte and complement checksum are valid.

### Extract an official GBA cartridge

```sh
./ezfadvanceIII_card_reader --extract OUTPUT.gba
```

`--dump` is an equivalent alias for `--extract`.

Add `--verbose` for per-block addresses, timing, and throughput. Extraction is
read-only: it performs a guarded full 32-MiB scan using 512 global-linear
64-KiB reads, completes read-session cleanup, and then writes the smallest
1/2/4/8/16/32-MiB extent containing all non-`FF` data. This trailing-`FF`
sizing is a generic heuristic, not a recovered original-manager algorithm.

A Golden Sun 8-MiB extraction is hardware-proven by trusted SHA-256 equality
and real-hardware boot. Other extraction sizes, including the new 1-MiB
extent, still require equivalent physical-cartridge dump/hash qualification.

### Extract a ROM stored on an EZ3 cartridge

```sh
./ezfadvanceIII_card_reader --extract N OUTPUT.gba [--verbose]
```

The equivalent `--dump N OUTPUT.gba` form follows the same routing and default
ROM-selection behavior.

`N` is the displayed one-based catalog number. If it is omitted on an EZ3
cartridge, the reader extracts catalog ROM 1. The reader exports the ROM's
catalog size-class extent, restores any overlapping loader area to `0xFF`, and
validates the reconstructed GBA header before writing. EZ3Manager replaces the
first instruction of physical ROM 1 with a loader branch, so extraction
reconstructs that instruction from `Orig. entry`. ROM 2 and later are not
modified. ROM 1 and ROM 2 extraction from a tested two-ROM card are
hardware-qualified by SHA-256 equality with both original files. A separate
single-ROM-layout extraction remains to be qualified.
The default display uses the same progress bar as official-cartridge
extraction; `--verbose` selects per-block timing and throughput diagnostics.

### Read an EZ3 save

```sh
./ezfadvanceIII_save_reader --output OUTPUT.sav
./ezfadvanceIII_save_reader --rom N --output OUTPUT.sav
./ezfadvanceIII_save_reader --rom N --save-bank 0x09X0 --output OUTPUT.sav
./ezfadvanceIII_save_reader --save-bank 0x09X0 --output OUTPUT.sav
./ezfadvanceIII_save_reader --save-bank 0x0900 --consecutive-bank 4 \
  --output ALL-BANKS.sav
```

Save/catalog processing is restricted to a cartridge positively classified as
EZ3 flash. Supported save paths are 32-KiB `SRAM_V111` and 64-KiB
`FLASH512`.
A bare invocation displays the card layout and an updating per-ROM metadata
scan percentage. Argument-driven operations suppress that diagnostic display
and silently scan only the selected ROM plus predecessors required for correct
cumulative save-bank allocation.
Single-ROM layouts select ROM 1 automatically. Multi-ROM layouts require an
explicit `--rom N` choice unless `--save-bank` requests raw physical-bank
access. A selected ROM must have a supported format.
Direct extraction reads one 32-KiB bank by default; `--consecutive-bank N`
reads 1–4 consecutive banks when the requested range remains within `0x0930`.
This explicit physical-bank path bypasses ROM initialization, mapping, and
catalog discovery so those unrelated transitions cannot alter raw save access.
Its bank and capacity are derived from the documented cumulative
catalog-order allocation policy; only the observed `FLASH512`-then-`SRAM_V111`
case is currently hardware-proven. Unknown predecessor capacity and layouts
exceeding four banks are refused.

### Write an EZ3 save

```sh
./ezfadvanceIII_save_reader \
  --write INPUT.sav \
  --backup ORIGINAL.sav \
  --yes-really-write \
  [--rom N]

./ezfadvanceIII_save_reader \
  --write INPUT.sav \
  --backup ORIGINAL.sav \
  --yes-really-write \
  --save-bank 0x0900 \
  [--consecutive-bank N]
```

Direct-bank writing accepts one through four complete 32-KiB banks. When
`--consecutive-bank N` is supplied, the input must be exactly `N * 32768`
bytes; otherwise the input size selects the number of consecutive banks. The
requested range must end at or before `0x0930`. Backup and full byte-for-byte
read-back verification cover the same complete range. Explicit physical-bank
writing uses the same ROM-independent raw path as direct extraction.
`--backup` is recommended but optional. When it is omitted, the tool displays
a destructive-operation warning and accepts only an explicit interactive
`y`/`yes` response before opening the USB device. `--yes-really-write` remains
mandatory.

### Erase EZ3 saves

Clear and verify all four 32-KiB save banks:

```sh
./ezfadvanceIII_save_reader --erase --backup saves-before-erase.sav
```

Clear only a selected contiguous range:

```sh
./ezfadvanceIII_save_reader --erase \
  --save-bank 0x0910 --consecutive-bank 2
```

With `--save-bank` but no count, only that bank is cleared. `--backup` saves
the exact selected range before the first zero write and refuses to overwrite
an existing file. If it is omitted, the tool warns about permanent data loss
and accepts only an explicit interactive `y`/`yes` response before opening the
USB device. Erase mode cannot be combined with extraction, ROM selection, or
save writing.

### Build or write a multi-ROM image

Dry-run layout inspection does not access USB:

```sh
./ezfadvanceIII_multirom_writer ROM1.gba [ROM2.gba ...]
```

Erase, program, and verify the constructed image:

```sh
./ezfadvanceIII_multirom_writer --yes-really-write ROM1.gba [ROM2.gba ...]
```

The default destructive-write display uses progress bars. Add `--verbose` for
per-sector and per-block diagnostics. `--skip-verify` deliberately omits ROM
readback comparison but still performs status/reset cleanup. EEPROM images may
require an evidence-backed `--mapN=4` or `--mapN=5` override; the writer does
not patch save routines automatically.

Capture-supported verification currently covers:

- every constructed extent below 8 MiB, with 1-, 2-, and 4-MiB checkpoints
  explicitly hardware-proven;
- exact 8, 16, 24, and 32 MiB, all hardware-proven;
- explicit partial 12, 20, and 28 MiB, all hardware-proven;
- the dedicated tiny-tail case immediately above 16 MiB.

The 2-MiB checkpoint is independently confirmed with two exactly 2-MiB
single-ROM sources. Each produced a `0x200700` constructed image, programmed
the `0x700` loader tail at `0x200000`, verified 33 padded 64-KiB blocks through
exclusive end `0x210000`, and booted on a real GBA. This is 2-MiB source-ROM
evidence, not an exactly 2-MiB constructed image or an official-cartridge
extraction result.

Other partial higher-window extents are programmed normally but full readback
verification is skipped rather than using an inferred selector.

### Wipe an EZ3 cartridge

```sh
./ezfadvanceIII_wipe_card --yes-really-wipe
```

This is a destructive full-card erase followed by capture-derived blank
verification. A preventive wipe before a large write is optional and is never
performed automatically.

## Project status

This project is a working community-developed alternative for EZF Advance III
hardware. Its core workflows are covered by offline tests, and supported write
and read-back verification paths have been exercised successfully on real
hardware. The representative 12-/20-/28-MiB partial higher-window checkpoints
are capture-, transcript-, and hardware-proven. Development and reverse
engineering are still ongoing, so hardware
and cartridge configurations outside the documented and tested paths may not
behave as expected.

Hardware tests are intentionally separate because writer and wipe operations
are destructive. Always run a dry-run writer command first and review its
layout before supplying `--yes-really-write`.

## Disclaimer

### Independent project / no affiliation or endorsement

This is an **independent, unofficial, community-developed project**. It is **not affiliated with, associated with, authorized by, endorsed by, sponsored by, supported by, or otherwise connected with Nintendo Co., Ltd., any Nintendo affiliate, the EZ-Flash Team, or any related manufacturer, developer, distributor, or rights holder**.

Nintendo, Game Boy Advance, EZ-Flash, EZF Advance III, and any other product names, trademarks, service marks, logos, or brands referenced by this project remain the property of their respective owners. Their use in this repository is solely for identification, compatibility, interoperability, technical documentation, and descriptive purposes and does not imply any affiliation, endorsement, sponsorship, approval, or support.

**Neither Nintendo nor the EZ-Flash Team provides support for this project.** Questions, bug reports, compatibility issues, device problems, or damage arising from this software should not be directed to Nintendo, the EZ-Flash Team, or their respective affiliates, employees, distributors, or support channels.

### Project origin and purpose

This project was started because the original software and drivers for the **EZF Advance III** are available for and operational with **Windows XP**, an operating system that is now very old and no longer a practical or desirable platform for many users.

The purpose of this project is therefore to research, document, and develop an independent alternative that can help preserve continued use of existing EZF Advance III hardware on modern Unix-like systems, without requiring the original Windows XP environment. The project is focused on compatibility and interoperability with hardware that users already own; it is not intended to represent, replace, or imply official software, drivers, support, or endorsement from Nintendo or the EZ-Flash Team.

We hope that someone with the necessary technical knowledge and interest will **fork this repository and continue the project further**. This repository is shared as a working community-developed alternative and as a record of the work already done, in the hope that others may improve, correct, document, and extend it.

This software and project are provided **“AS IS” and “AS AVAILABLE,” without warranty of any kind**, express or implied.

This project remains under active development and may contain bugs, incomplete features, incorrect assumptions, or unexpected behavior, particularly on hardware and configurations that have not yet been tested. Use of this software may cause data loss, corruption, malfunction, permanent damage, or otherwise render an **EZF Advance III device partially or completely unusable (“bricked”)**.

A substantial portion of this project was created through **“vibe coding,” reverse engineering, experimentation, and the use of AI-assisted development tools, including ChatGPT and Codex**. As a result, the code may contain errors, inaccurate implementations, undocumented behavior, or functionality that has not been thoroughly tested or independently verified.

The owner of this Git repository **does not claim to possess the technical expertise, engineering qualifications, or detailed knowledge necessary to guarantee the correctness or safety of the software**. The repository owner may also be unable to provide technical support, debugging assistance, device recovery assistance, repair instructions, or further development support if the software causes problems or damages an EZF Advance III device.

By downloading, installing, modifying, executing, flashing, or otherwise using this software, you acknowledge and accept that you do so **entirely at your own risk**.

To the maximum extent permitted by applicable law, the author(s), contributor(s), and maintainer(s) of this project shall not be liable for any direct, indirect, incidental, special, consequential, or other damages arising from or related to the use or inability to use this software, including, without limitation, damage to hardware, loss or corruption of data, loss of functionality, device failure, or the permanent bricking of an EZF Advance III device.

**You are solely responsible for understanding the risks, making appropriate backups where possible, verifying the software before use, and determining whether you are willing to accept the possibility of permanently damaging your EZF Advance III device.**

Do not use this software on any device that you are not prepared to potentially damage or lose. Use this project only if you fully understand and accept these risks.
