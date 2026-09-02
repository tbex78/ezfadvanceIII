#include "ezfadvance/ez3_card_workflow.hpp"

#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/ez3_catalog.hpp"
#include "ezfadvance/ez3_catalog_reader.hpp"
#include "ezfadvance/progress_bar.hpp"
#include "ezfadvance/read_only_cartridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace ezfadvance {
namespace {

static constexpr std::uint32_t FLASH_WINDOW_SIZE = 0x00800000u;
static constexpr std::uint32_t LINEAR16_LIMIT = 0x01000000u;
static constexpr std::uint32_t LINEAR24_LIMIT = 0x01800000u;
static constexpr std::uint32_t CARD_IMAGE_LIMIT = 0x02000000u;
static constexpr std::size_t CAPTURE_PROVEN_MAX_ROMS = 8;
using GbaHeader = ezfadvance::GbaHeader;

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


} // namespace

Ez3CardWorkflow::Ez3CardWorkflow(
    ReadOnlyCartridge& cartridge,
    std::optional<Ez3ExtractionRequest> extraction,
    bool verbose) noexcept
    : cartridge_(cartridge), extraction_(std::move(extraction)),
      verbose_(verbose)
{
}

int Ez3CardWorkflow::run()
{
    return inspect_card(cartridge_, extraction_, verbose_);
}

} // namespace ezfadvance
