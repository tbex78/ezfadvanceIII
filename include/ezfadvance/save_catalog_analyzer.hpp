#pragma once

#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/ez3_catalog.hpp"
#include "ezfadvance/read_only_cartridge.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ezfadvance {

struct SaveCatalogRom {
    CatalogEntry catalog_entry;
    GbaHeader header;
    std::uint32_t allocation_span = 0;
    std::string save_marker;
};

struct SaveCatalogAnalysis {
    bool mapping_prepared = false;
    std::vector<SaveCatalogRom> roms;

    explicit operator bool() const noexcept { return mapping_prepared; }
};

// Reads the ROM metadata needed by the save application after EZ3 catalog
// discovery. Presentation, ROM selection, and save-memory I/O remain caller
// responsibilities.
class SaveCatalogAnalyzer final {
public:
    SaveCatalogAnalyzer(ReadOnlyCartridge& cartridge,
                        std::uint32_t image_limit) noexcept;

    SaveCatalogAnalysis analyze(const Ez3CatalogLayout& catalog,
                                std::uint32_t loader_start,
                                std::uint32_t loader_end);

private:
    std::string detectSaveMarker(std::uint32_t start,
                                 std::uint32_t span);

    ReadOnlyCartridge& cartridge_;
    std::uint32_t image_limit_;
};

} // namespace ezfadvance
