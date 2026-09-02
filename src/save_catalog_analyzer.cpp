#include "ezfadvance/save_catalog_analyzer.hpp"

#include "ezfadvance/save_selection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ezfadvance {
namespace {

bool containsPattern(const std::vector<std::uint8_t>& data,
                     const std::string& pattern)
{
    return !pattern.empty() && data.size() >= pattern.size() &&
        std::search(data.begin(), data.end(),
                    pattern.begin(), pattern.end()) != data.end();
}

} // namespace

SaveCatalogAnalyzer::SaveCatalogAnalyzer(ReadOnlyCartridge& cartridge,
                                         std::uint32_t image_limit,
                                         SaveScanProgress progress) noexcept
    : cartridge_(cartridge), image_limit_(image_limit),
      progress_(std::move(progress))
{
}

SaveCatalogAnalysis SaveCatalogAnalyzer::analyze(
    const Ez3CatalogLayout& catalog,
    std::uint32_t loader_start,
    std::uint32_t loader_end,
    std::size_t entry_count)
{
    const std::size_t analyzed_count = entry_count == 0
        ? catalog.entries.size()
        : std::min(entry_count, catalog.entries.size());
    std::uint32_t highest_scan_address = loader_end;
    for (std::size_t index = 0; index < analyzed_count; ++index) {
        const auto end = catalog.allocationEnd(
            index, loader_start, image_limit_);
        if (end && *end > catalog.entries[index].start)
            highest_scan_address = std::max(highest_scan_address, *end - 1);
    }

    SaveCatalogAnalysis analysis;
    if (!cartridge_.prepareLinearForAddress(highest_scan_address))
        return analysis;

    analysis.mapping_prepared = true;
    analysis.roms.reserve(analyzed_count);
    for (std::size_t index = 0; index < analyzed_count; ++index) {
        SaveCatalogRom rom;
        rom.catalog_entry = catalog.entries[index];
        rom.header = cartridge_.readGbaHeader(rom.catalog_entry.start)
                         .value_or(GbaHeader{});

        const auto end = catalog.allocationEnd(
            index, loader_start, image_limit_);
        rom.allocation_span = end
            ? static_cast<std::uint32_t>(boundedSaveScanSpan(
                  rom.catalog_entry.start, *end, image_limit_))
            : 0;
        if (rom.allocation_span != 0) {
            rom.save_marker = detectSaveMarker(
                rom.catalog_entry.start, rom.allocation_span,
                index, analyzed_count);
        }
        analysis.roms.push_back(std::move(rom));
    }
    return analysis;
}

std::string SaveCatalogAnalyzer::detectSaveMarker(std::uint32_t start,
                                                  std::uint32_t span,
                                                  std::size_t rom_index,
                                                  std::size_t rom_count)
{
    static const std::vector<std::string> patterns = {
        "SRAM_V111",
        "SRAM_V112",
        "SRAM_V",
        std::string("SRAM\0", 5),
        "FLASH1M",
        "FLASH512",
        "FLASH_V",
        "EEPROM_V"
    };

    constexpr std::size_t chunk_size = 0x10000;
    constexpr std::size_t overlap = 32;
    std::vector<std::uint8_t> carry;

    std::uint32_t position = 0;
    if (progress_) progress_(rom_index, rom_count, 0, span, false);
    while (position < span) {
        const auto length = std::min<std::size_t>(
            chunk_size, static_cast<std::size_t>(span - position));
        std::vector<std::uint8_t> chunk;
        if (!cartridge_.read(start + position, chunk, length)) {
            if (progress_) progress_(rom_index, rom_count, position, span, true);
            return {};
        }

        std::vector<std::uint8_t> joined;
        joined.reserve(carry.size() + chunk.size());
        joined.insert(joined.end(), carry.begin(), carry.end());
        joined.insert(joined.end(), chunk.begin(), chunk.end());

        for (const auto& pattern : patterns) {
            if (containsPattern(joined, pattern)) {
                if (progress_)
                    progress_(rom_index, rom_count,
                              position + static_cast<std::uint32_t>(length),
                              span, true);
                return pattern.size() == 5 && pattern[4] == '\0'
                    ? "SRAM"
                    : pattern;
            }
        }

        const auto keep = std::min(overlap, joined.size());
        carry.assign(joined.end() - static_cast<std::ptrdiff_t>(keep),
                     joined.end());
        position += static_cast<std::uint32_t>(length);
        if (progress_)
            progress_(rom_index, rom_count, position, span,
                      position == span);
    }
    return {};
}

} // namespace ezfadvance
