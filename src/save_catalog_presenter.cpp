#include "ezfadvance/save_catalog_presenter.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace ezfadvance {
namespace {

std::string hex32(std::uint32_t value)
{
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::setw(8) << std::setfill('0')
              << value;
    return formatted.str();
}

} // namespace

SaveCatalogPresenter::SaveCatalogPresenter(std::ostream& output) noexcept
    : output_(output)
{
}

SaveScanProgress SaveCatalogPresenter::scanProgress()
{
    return [this](std::size_t index, std::size_t count,
                  std::uint32_t scanned, std::uint32_t total, bool finished) {
        const unsigned percent = total == 0
            ? 100u
            : static_cast<unsigned>(
                  (static_cast<std::uint64_t>(scanned) * 100u) / total);
        output_ << '\r' << "Scanning save metadata for ROM "
                << (index + 1) << '/' << count << ": "
                << std::setw(3) << percent << '%' << std::flush;
        if (finished) output_ << '\n';
    };
}

void SaveCatalogPresenter::showSummary(bool single_rom, std::size_t rom_count,
                                       std::uint32_t loader_start) const
{
    output_ << "\n========================================\n"
            << "EZF ADVANCE III SAVE TOOL - CARD CONTENTS\n"
            << "========================================\n"
            << "Layout       : " << (single_rom ? "single ROM" : "multi ROM")
            << "\nROM count    : " << rom_count
            << "\nLoader/menu  : " << hex32(loader_start) << "\n\n";
}

void SaveCatalogPresenter::showRoms(
    const SaveCatalogAnalysis& analysis) const
{
    for (std::size_t index = 0; index < analysis.roms.size(); ++index) {
        const auto& rom = analysis.roms[index];
        output_ << "\nROM " << (index + 1) << "\n"
                << "  Catalog name : " << rom.catalog_entry.name << "\n"
                << "  Start        : " << hex32(rom.catalog_entry.start) << "\n"
                << "  Alloc. span  : " << rom.allocation_span << " bytes\n"
                << "  EZ type      : "
                << static_cast<unsigned>(rom.catalog_entry.type) << "\n"
                << "  Mapping flag : "
                << static_cast<unsigned>(rom.catalog_entry.mapping) << "\n"
                << "  GBA title    : "
                << (rom.header.title.empty() ? "(blank)" : rom.header.title)
                << "\n  Game code    : "
                << (rom.header.game_code.empty() ? "(blank)"
                                                 : rom.header.game_code)
                << "\n  Save marker  : "
                << (rom.save_marker.empty() ? "(none found)"
                                            : rom.save_marker)
                << "\n";
    }
}

} // namespace ezfadvance
