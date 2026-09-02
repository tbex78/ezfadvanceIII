#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  elif __has_include(<libusb.h>)
#    include <libusb.h>
#  else
#    error "libusb header not found. Install the libusb development package."
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/platform.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/card_reader_options.hpp"
#include "ezfadvance/ez3_catalog.hpp"
#include "ezfadvance/ez3_catalog_reader.hpp"
#include "ezfadvance/read_only_cartridge.hpp"
#include "ezfadvance/progress_bar.hpp"
#include "ezfadvance/version.hpp"

// EZF Advance III card reader 0.9.0, read-only inspector.
// 0.6.2 removes hard-coded project-version text from runtime output.
// Card-read protocol behavior remains unchanged from 0.5.13.
//
// Ported from the historical card-reader v1 implementation. The USB/card
// read/probe behavior is intentionally unchanged. This program never sends
// erase (0x96) commands and never sends ROM-program payloads. Its 0x92 traffic
// is limited to the original EZ3Manager flash-probe / mapping sequences needed
// for capture-derived card inspection.
//
// Supported native targets:
//   macOS
//   Linux
//   FreeBSD
//   OpenBSD
//   NetBSD
//   DragonFly BSD
//   Windows 10/11
//
// Windows 10/11 is supported through libusb with a compatible WinUSB/libusbK
// device driver. Unix-like targets retain their existing native paths.
//
// Recognized catalog layouts:
//   single ROM: loader-relative header 0x4E8, entry 0x4F8
//   multi ROM : loader-relative header 0x475E, entries 0x476E (28 bytes each)
//
// 0.5.13 improves blank-card diagnostics only. If the first four card bytes
// are FF FF FF FF, the reader now explicitly reports that the cartridge
// appears empty/erased and that no EZ3 loader/catalog is present at byte 0.
// USB probing, mapping, and read behavior are unchanged.
//
// 0.5.12 aligns the diagnostic reader with the writer's current catalog/layout
// knowledge and the newer 5-, 6-, 7-, and 8-ROM captures:
//   * 1..8 active catalog entries are capture-proven; the loader contains 120
//     structurally available 28-byte catalog slots, used here only as a safe
//     parser bound rather than a claim that 120-ROM menu operation is proven;
//   * full 32-MiB / 256-Mbit card geometry;
//   * exact 16-MiB, 24-MiB, and 32-MiB linear read mappings;
//   * the 32-MiB sequence is corrected to the exact 256-Mbit transition used by
//     the current writer and independently reconfirmed by the 6/7/8-ROM captures.
// The utility remains strictly read-only: no erase commands and no ROM
// programming are performed.

static constexpr uint32_t FLASH_WINDOW_SIZE = 0x00800000u; // 8 MiB
static constexpr uint32_t LINEAR16_LIMIT     = 0x01000000u; // 16 MiB
static constexpr uint32_t LINEAR24_LIMIT     = 0x01800000u; // 24 MiB / 192 Mbit
static constexpr uint32_t CARD_IMAGE_LIMIT   = 0x02000000u; // 32 MiB / 256 Mbit

static constexpr size_t CAPTURE_PROVEN_MAX_ROMS = 8;

using GbaHeader = ezfadvance::GbaHeader;

static int inspect_official_cartridge(ezfadvance::ReadOnlyCartridge& cartridge)
{
    std::vector<std::uint8_t> header_bytes;
    const bool read_ok = cartridge.read(0, header_bytes, 0xC0);
    const bool header_ok = read_ok &&
        ezfadvance::CartridgeFormat::validGbaRomHeader(header_bytes);
    const GbaHeader header = read_ok ? GbaHeader::parse(header_bytes)
                                     : GbaHeader{};
    if (!header_ok) {
        std::cerr << "Official-ROM probe behavior was detected, but the GBA "
                     "fixed header byte/checksum validation failed; the "
                     "cartridge is not confirmed as an official GBA ROM.\n";
        if (!cartridge.finishSession()) {
            std::cerr << "Read-session cleanup after header rejection failed.\n";
            return 2;
        }
        return 3;
    }
    std::cout << "\n========================================\n"
              << "OFFICIAL GBA CARTRIDGE\n"
              << "========================================\n"
              << "GBA title    : " << (header.title.empty() ? "(blank)" : header.title) << '\n'
              << "Game code    : " << (header.game_code.empty() ? "(blank)" : header.game_code) << '\n'
              << "Maker code   : " << (header.maker_code.empty() ? "(blank)" : header.maker_code) << '\n'
              << "ROM version  : " << static_cast<unsigned>(header.version) << '\n'
              << "Header check : " << (header.checksum_ok ? "OK" : "FAILED") << '\n';
    if (!cartridge.finishSession()) return 2;
    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

static int extract_official_cartridge(ezfadvance::ReadOnlyCartridge& cartridge,
                                      const std::string& output_path,
                                      bool verbose)
{
    constexpr std::size_t dump_size = CARD_IMAGE_LIMIT;
    constexpr std::size_t block_size = 0x10000;
    std::vector<std::uint8_t> image(dump_size);
    std::cout << "Reading the full 32-MiB GBA address space as "
              << (dump_size / block_size) << " x 64-KiB blocks...\n";

    bool read_ok = true;
    bool header_checked = false;
    bool header_ok = false;
    const auto extraction_started = std::chrono::steady_clock::now();
    {
        ezfadvance::ProgressBar progress("Extract", dump_size);
        if (!verbose) progress.update(0);
        for (std::size_t offset = 0; offset < dump_size && read_ok;
             offset += block_size) {
            const auto block_started = std::chrono::steady_clock::now();
            read_ok = cartridge.read(static_cast<std::uint32_t>(offset),
                                     image.data() + offset, block_size);
            if (!read_ok) continue;
            if (offset == 0) {
                header_checked = true;
                std::vector<std::uint8_t> first_block(
                    image.begin(), image.begin() + block_size);
                header_ok = ezfadvance::CartridgeFormat::validGbaRomHeader(
                    first_block);
                if (!header_ok) {
                    read_ok = false;
                    continue;
                }
                if (verbose)
                    std::cout << "Validated GBA header checksum and fixed "
                                 "value 0x96 before full extraction.\n";
            }
            if (verbose) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - block_started).count();
                const double kib_per_second = seconds > 0.0 ?
                    (static_cast<double>(block_size) / 1024.0) / seconds : 0.0;
                std::cout << "read card byte 0x" << std::hex
                          << std::setw(8) << std::setfill('0') << offset
                          << " length 0x" << block_size << std::dec
                          << std::setfill(' ') << ": " << std::fixed
                          << std::setprecision(3) << seconds << " s, "
                          << std::setprecision(1) << kib_per_second
                          << " KiB/s\n";
            } else {
                progress.update(offset + block_size);
            }
        }
    }
    if (verbose && read_ok) {
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - extraction_started).count();
        const double mib_per_second = seconds > 0.0 ?
            (static_cast<double>(dump_size) / (1024.0 * 1024.0)) / seconds : 0.0;
        std::cout << "ROM read complete: " << dump_size << " bytes in "
                  << std::fixed << std::setprecision(3) << seconds << " s ("
                  << std::setprecision(2) << mib_per_second << " MiB/s).\n";
    }
    const bool finish_ok = cartridge.finishSession();
    if (header_checked && !header_ok) {
        std::cerr << "Official cartridge classification was not sufficient: "
                     "the GBA header checksum/fixed value is invalid. "
                     "Extraction was refused.\n";
        if (!finish_ok)
            std::cerr << "Read-session cleanup also failed.\n";
        return 3;
    }
    if (!read_ok) {
        std::cerr << "Official cartridge extraction failed during ROM read.\n";
        if (!finish_ok)
            std::cerr << "Read-session cleanup also failed.\n";
        return 2;
    }
    if (!finish_ok) {
        std::cerr << "ROM data was read, but read-session cleanup failed; "
                     "no output file was written.\n";
        return 2;
    }

    const auto output_size =
        ezfadvance::CartridgeFormat::trimmedGbaRomSize(image);
    if (!output_size) {
        std::cerr << "Could not derive a supported 1/2/4/8/16/32-MiB ROM size "
                     "from the trailing 0xFF region; no output was written.\n";
        return 3;
    }
    const std::size_t removed = image.size() - *output_size;
    image.resize(*output_size);
    std::cout << "Detected ROM extent: " << (image.size() / (1024 * 1024))
              << " MiB; removed " << (removed / (1024 * 1024))
              << " MiB of trailing 0xFF address-space padding.\n";

    std::ofstream output(output_path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "Could not create output file: " << output_path << '\n';
        return 2;
    }
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    output.close();
    if (!output) {
        std::cerr << "Failed while writing output file: " << output_path << '\n';
        return 2;
    }

    std::cout << "Wrote " << image.size() << " bytes to " << output_path
              << "\nThe output was rounded up to the smallest supported "
                 "1/2/4/8/16/32-MiB size containing all non-0xFF data.\n"
              << "No erase or ROM programming operation was performed.\n";
    return 0;
}

using CatalogEntry = ezfadvance::CatalogEntry;

enum class ReadMappingMode {
    Lower8MiB,
    Linear16MiB,
    Linear24MiB,
    Linear32MiB
};

static const char* mapping_mode_name(ReadMappingMode mode)
{
    switch (mode) {
        case ReadMappingMode::Lower8MiB:   return "lower 8-MiB";
        case ReadMappingMode::Linear16MiB: return "16-MiB linear";
        case ReadMappingMode::Linear24MiB: return "24-MiB linear";
        case ReadMappingMode::Linear32MiB: return "32-MiB linear";
    }
    return "unknown";
}

static std::string hex32(uint32_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

static std::optional<uint32_t> stored_end(const CatalogEntry& entry)
{
    return entry.storedEnd(CARD_IMAGE_LIMIT);
}

struct Ez3ExtractionRequest {
    std::size_t rom_number = 0;
    std::string output_path;
};

static bool write_binary_file(const std::string& path,
                              const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "Could not create output file: " << path << '\n';
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "Failed while writing output file: " << path << '\n';
        return false;
    }
    return true;
}

static int extract_ez3_rom(ezfadvance::ReadOnlyCartridge& cartridge,
                           const std::vector<CatalogEntry>& entries,
                           uint32_t loader_start,
                           bool is_single,
                           const Ez3ExtractionRequest& request,
                           bool verbose)
{
    const auto fail_after_cleanup = [&cartridge](int status) {
        if (!cartridge.finishSession()) {
            std::cerr << "Read-session cleanup also failed.\n";
            return 2;
        }
        return status;
    };
    if (request.rom_number == 0 || request.rom_number > entries.size()) {
        std::cerr << "ROM number " << request.rom_number
                  << " is outside the catalog range 1.." << entries.size()
                  << ".\n";
        return fail_after_cleanup(1);
    }

    const std::size_t index = request.rom_number - 1;
    const CatalogEntry& entry = entries[index];
    const auto inclusive_end = stored_end(entry);
    if (!inclusive_end || *inclusive_end < entry.start) {
        std::cerr << "ROM " << request.rom_number
                  << " has an invalid catalog size class or extent.\n";
        return fail_after_cleanup(3);
    }
    const std::size_t size =
        static_cast<std::size_t>(*inclusive_end - entry.start) + 1;

    // Inspection selects a mapping from loader/header addresses. Extraction
    // must additionally cover the selected ROM's complete stored extent.
    const auto required_limit =
        ezfadvance::CartridgeFormat::requiredLinearReadLimit(*inclusive_end);
    if (!required_limit)
        return fail_after_cleanup(3);
    if (!cartridge.prepareLinearForAddress(*inclusive_end))
        return fail_after_cleanup(2);

    std::vector<std::uint8_t> image;
    std::cout << "Extracting ROM " << request.rom_number << " from "
              << hex32(entry.start) << " through " << hex32(*inclusive_end)
              << " (" << size << " bytes)...\n";
    image.resize(size);
    constexpr std::size_t block_size = 0x10000;
    bool read_ok = true;
    const auto extraction_started = std::chrono::steady_clock::now();
    {
        ezfadvance::ProgressBar progress("Extract", size);
        if (!verbose) progress.update(0);
        for (std::size_t offset = 0; offset < size && read_ok;
             offset += block_size) {
            const std::size_t length = std::min(block_size, size - offset);
            const auto block_started = std::chrono::steady_clock::now();
            read_ok = cartridge.read(
                entry.start + static_cast<std::uint32_t>(offset),
                image.data() + offset, length);
            if (!read_ok) continue;
            if (verbose) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - block_started).count();
                const double kib_per_second = seconds > 0.0 ?
                    (static_cast<double>(length) / 1024.0) / seconds : 0.0;
                std::cout << "read card byte "
                          << hex32(entry.start + static_cast<std::uint32_t>(offset))
                          << " length 0x" << std::hex << length << std::dec
                          << ": " << std::fixed << std::setprecision(3)
                          << seconds << " s, " << std::setprecision(1)
                          << kib_per_second << " KiB/s\n";
            } else {
                progress.update(offset + length);
            }
        }
    }
    if (!read_ok) {
        std::cerr << "ROM extraction failed during card read.\n";
        return fail_after_cleanup(2);
    }
    if (verbose) {
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - extraction_started).count();
        const double mib_per_second = seconds > 0.0 ?
            (static_cast<double>(size) / (1024.0 * 1024.0)) / seconds : 0.0;
        std::cout << "ROM read complete: " << size << " bytes in "
                  << std::fixed << std::setprecision(3) << seconds << " s ("
                  << std::setprecision(2) << mib_per_second << " MiB/s).\n";
    }

    // Original EZ3Manager places the loader in erased ROM space when it can.
    // Reconstruct that overlap as erased bytes rather than exporting loader
    // code as part of the game. The known single loader is 0x660 bytes; the
    // maximum capture-derived multi loader extent is 0x7080 bytes.
    const std::size_t loader_length = is_single ? 0x660u :
        ezfadvance::Ez3CatalogParser::loader_read_size;
    const std::uint64_t rom_begin = entry.start;
    const std::uint64_t rom_end = rom_begin + image.size();
    const std::uint64_t loader_begin = loader_start;
    const std::uint64_t loader_end = loader_begin + loader_length;
    const std::uint64_t overlap_begin = std::max(rom_begin, loader_begin);
    const std::uint64_t overlap_end = std::min(rom_end, loader_end);
    if (overlap_begin < overlap_end) {
        std::cout << "Restored overlapping loader area "
                  << hex32(static_cast<uint32_t>(overlap_begin)) << ".."
                  << hex32(static_cast<uint32_t>(overlap_end - 1))
                  << " to 0xFF.\n";
    }

    if (!ezfadvance::CartridgeFormat::reconstructEz3Rom(
            image, entry, index == 0, entry.start, loader_start,
            loader_length)) {
        std::cerr << "Could not reconstruct ROM 1's original entry branch.\n";
        return fail_after_cleanup(3);
    }
    if (index == 0)
        std::cout << "Restored ROM 1 entry branch to catalog target "
                  << hex32(entry.target_or_start) << ".\n";

    if (!ezfadvance::CartridgeFormat::validGbaRomHeader(image)) {
        std::cerr << "Reconstructed ROM header validation failed; no output "
                     "file was written.\n";
        return fail_after_cleanup(3);
    }
    if (!cartridge.finishSession()) {
        std::cerr << "ROM data was read, but read-session cleanup failed; "
                     "no output file was written.\n";
        return 2;
    }
    if (!write_binary_file(request.output_path, image))
        return 2;

    std::cout << "Wrote " << image.size() << " bytes to "
              << request.output_path
              << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

static void print_rom(size_t index,
                      const CatalogEntry& e,
                      const GbaHeader& g,
                      std::optional<uint32_t> allocated_end)
{
    std::cout << "\nROM " << index << "\n"
              << "  Catalog name : " << e.name << "\n"
              << "  Start        : " << hex32(e.start) << "\n";

    if (allocated_end && *allocated_end >= e.start) {
        const uint32_t span = *allocated_end - e.start;
        std::cout << "  Alloc. span  : " << span << " bytes (0x"
                  << std::hex << span << std::dec << ")\n";
    }

    std::cout << "  Catalog type : " << static_cast<unsigned>(e.type) << " (size class)\n"
              << "  Catalog map  : " << static_cast<unsigned>(e.mapping) << "\n";

    if (index == 1)
        std::cout << "  Orig. entry  : " << hex32(e.target_or_start) << "\n";
    else
        std::cout << "  Stored start : " << hex32(e.target_or_start) << "\n";

    const auto end = stored_end(e);
    std::cout << "  Stored end   : "
              << (end ? hex32(*end) : "(invalid size class/extent)") << "\n";

    if (g.readable) {
        std::cout << "  GBA title    : " << (g.title.empty() ? "(blank)" : g.title) << "\n"
                  << "  Game code    : " << (g.game_code.empty() ? "(blank)" : g.game_code) << "\n"
                  << "  Maker code   : " << (g.maker_code.empty() ? "(blank)" : g.maker_code) << "\n"
                  << "  ROM version  : " << static_cast<unsigned>(g.version) << "\n"
                  << "  Header check : " << (g.checksum_ok ? "OK" : "FAILED") << "\n";
    }
}

static int inspect_card(ezfadvance::ReadOnlyCartridge& cartridge,
                        const std::optional<Ez3ExtractionRequest>& extraction,
                        bool verbose)
{
    ezfadvance::Ez3CatalogReader catalog_reader(
        cartridge, CARD_IMAGE_LIMIT,
        ezfadvance::Ez3CatalogParser::structural_slot_count);
    const auto discovery = catalog_reader.read();
    if (discovery.status == ezfadvance::Ez3CatalogReadStatus::read_failed)
        return 2;
    if (discovery.status == ezfadvance::Ez3CatalogReadStatus::missing_branch) {
        const bool first_word_erased =
            std::all_of(discovery.first_bytes.begin(), discovery.first_bytes.end(),
                        [](uint8_t b) { return b == 0xFF; });

        if (first_word_erased) {
            std::cout
                << "Card appears empty / erased.\n"
                << "The first four card bytes are FF FF FF FF, which is the "
                   "erased flash state.\n"
                << "No EZ3 loader branch or ROM catalog is present at card "
                   "byte 0.\n"
                << "There is no programmed EZ3 ROM image to inspect.\n";
        } else {
            std::cerr
                << "Card byte 0 is not an ARM unconditional branch.\n"
                << "The card is not blank at byte 0, but it does not look like "
                   "a recognized capture-derived EZ3 single/multi-ROM image.\n";
        }
        return 3;
    }

    if (discovery.status ==
        ezfadvance::Ez3CatalogReadStatus::branch_out_of_range) {
        std::cerr << "Patched branch points to " << hex32(discovery.loader_start)
                  << ", outside the currently understood 32-MiB / 256-Mbit layout.\n";
        return 3;
    }

    const uint32_t loader_start = discovery.loader_start;

    ReadMappingMode mapping_mode = ReadMappingMode::Lower8MiB;
    if (discovery.loader_end >= LINEAR24_LIMIT) {
        mapping_mode = ReadMappingMode::Linear32MiB;
    } else if (discovery.loader_end >= LINEAR16_LIMIT) {
        mapping_mode = ReadMappingMode::Linear24MiB;
    } else if (discovery.loader_end >= FLASH_WINDOW_SIZE) {
        mapping_mode = ReadMappingMode::Linear16MiB;
    }
    if (!discovery) {
        std::cerr << "Found loader branch at " << hex32(loader_start)
                  << " but no recognized capture-derived single/multi-ROM catalog.\n";
        return 3;
    }
    const auto& catalog = *discovery.catalog;
    const bool is_single = catalog.isSingle();
    const std::size_t rom_count = catalog.entries.size();
    const std::vector<CatalogEntry>& entries = catalog.entries;

    // If any cataloged ROM header lies beyond the currently selected read
    // window, upgrade to the smallest capture-proven linear mapping that covers
    // that address. The 24-MiB mapping is now independently capture-proven.
    uint32_t highest_start = 0;
    for (const auto& e : entries)
        highest_start = std::max(highest_start, e.start);

    if (highest_start >= LINEAR24_LIMIT &&
        mapping_mode != ReadMappingMode::Linear32MiB) {
        if (!cartridge.prepareLinearForAddress(highest_start)) return 2;
        mapping_mode = ReadMappingMode::Linear32MiB;
    } else if (highest_start >= LINEAR16_LIMIT &&
               mapping_mode != ReadMappingMode::Linear24MiB &&
               mapping_mode != ReadMappingMode::Linear32MiB) {
        if (!cartridge.prepareLinearForAddress(highest_start)) return 2;
        mapping_mode = ReadMappingMode::Linear24MiB;
    } else if (highest_start >= FLASH_WINDOW_SIZE &&
               mapping_mode == ReadMappingMode::Lower8MiB) {
        if (!cartridge.prepareLinearForAddress(highest_start)) return 2;
        mapping_mode = ReadMappingMode::Linear16MiB;
    }

    std::cout << "\n========================================\n"
              << "EZF ADVANCE III CARD CONTENTS\n"
              << "========================================\n"
              << "Layout       : " << (is_single ? "single ROM" : "multi ROM") << "\n"
              << "ROM count    : " << rom_count << "\n"
              << "Loader/menu  : " << hex32(loader_start) << "\n"
              << "Read mapping : " << mapping_mode_name(mapping_mode) << "\n";

    if (!is_single) {
        if (rom_count <= CAPTURE_PROVEN_MAX_ROMS) {
            std::cout << "Catalog proof : count " << rom_count
                      << " is within the capture-proven 2.."
                      << CAPTURE_PROVEN_MAX_ROMS << " active-entry range\n";
        } else {
            std::cout << "Catalog proof : count " << rom_count
                      << " is structurally parseable but exceeds the currently "
                         "capture-proven 8-entry range\n";
        }
    }

    for (size_t i=0;i<entries.size();++i) {
        const std::optional<uint32_t> span_end = is_single ? std::nullopt :
            catalog.allocationEnd(i, loader_start, CARD_IMAGE_LIMIT);
        const GbaHeader g =
            cartridge.readGbaHeader(entries[i].start).value_or(GbaHeader{});
        print_rom(i+1,entries[i],g,span_end);
    }

    if (extraction)
        return extract_ez3_rom(cartridge, entries, loader_start, is_single,
                               *extraction, verbose);

    if (!cartridge.finishSession()) {
        std::cerr << "Card inspection succeeded, but the capture-derived "
                     "read-session close transition failed.\n";
        return 2;
    }

    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

class CardInspector final {
public:
    CardInspector(ezfadvance::ReadOnlyCartridge& cartridge,
                  std::optional<Ez3ExtractionRequest> extraction,
                  bool verbose) noexcept
        : cartridge_(cartridge), extraction_(std::move(extraction)), verbose_(verbose)
    {
    }

    int run() { return inspect_card(cartridge_, extraction_, verbose_); }

private:
    ezfadvance::ReadOnlyCartridge& cartridge_;
    std::optional<Ez3ExtractionRequest> extraction_;
    bool verbose_ = false;
};

static void usage(const char* argv0)
{
    std::cout << "Usage:\n"
              << "  " << argv0 << "\n"
              << "  " << argv0 << " --version\n"
              << "  " << argv0 << " --extract OUTPUT.gba [--verbose]\n"
              << "  " << argv0 << " --extract N OUTPUT.gba [--verbose]\n\n"
              << "Read-only EZF Advance III card inspector (" << ezfadvance::hostPlatformName() << ").\n"
              << "--extract reads 32 MiB and writes a trimmed .gba dump of a detected "
                 "official GBA cartridge.\n"
              << "--verbose replaces the extraction progress bar with per-block "
                 "address and timing diagnostics.\n";
    std::cout << "For EZ3 flash, --extract defaults to catalog ROM 1; adding "
                 "N selects another displayed ROM number. ROM 1's original "
                 "entry is reconstructed.\n";
}

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_card_reader");
        return 0;
    }

    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    ezfadvance::CardReaderOptions options;
    const auto parse_status = ezfadvance::parseCardReaderOptions(
        arguments, options, std::cerr);
    if (parse_status == ezfadvance::CardReaderParseStatus::help) {
        usage(argv[0]);
        return 0;
    }
    if (parse_status == ezfadvance::CardReaderParseStatus::error) {
        usage(argv[0]);
        return 1;
    }

    if (options.extract) {
        std::ifstream existing(options.output_path, std::ios::binary);
        if (existing.good()) {
            std::cerr << "Refusing to overwrite existing output file: "
                      << options.output_path << '\n';
            return 1;
        }
    }

    ezfadvance::UsbDevice device;
    const auto open_result = device.open(std::cerr);
    if (!open_result) {
        ezfadvance::reportUsbOpenFailure(open_result, std::cerr);
        return 1;
    }
    ezfadvance::ReadOnlyCartridge cartridge(device.handle());

    std::cout << "EZF Advance III opened on " << ezfadvance::hostPlatformName()
              << "; interface 0 claimed.\n";

    int result = 2;
    if (cartridge.initialize()) {
        const auto decision = ezfadvance::decideCardReaderAction(
            cartridge.kind(), options);
        switch (decision.action) {
        case ezfadvance::CardReaderAction::inspect_official:
            result = inspect_official_cartridge(cartridge);
            break;
        case ezfadvance::CardReaderAction::extract_official:
            result = extract_official_cartridge(
                cartridge, options.output_path, options.verbose);
            break;
        case ezfadvance::CardReaderAction::inspect_ez3: {
            CardInspector inspector(cartridge, std::nullopt, options.verbose);
            result = inspector.run();
            break;
        }
        case ezfadvance::CardReaderAction::extract_ez3: {
            if (!options.rom_number) {
                std::cout << "No EZ3 ROM number specified; defaulting to "
                             "catalog ROM 1.\n";
            }
            CardInspector inspector(
                cartridge, Ez3ExtractionRequest{decision.rom_number,
                                        options.output_path},
                options.verbose);
            result = inspector.run();
            break;
        }
        case ezfadvance::CardReaderAction::reject:
            std::cerr << decision.error << '\n';
            result = cartridge.finishSession() ? 1 : 2;
            break;
        }
    }
    else
        std::cerr << "Card initialization/classification failed.\n";

    return result;
}
