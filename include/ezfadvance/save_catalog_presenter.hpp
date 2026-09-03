#pragma once

#include "ezfadvance/save_catalog_analyzer.hpp"

#include <cstdint>
#include <iosfwd>

namespace ezfadvance {

// Owns the console representation of catalog/save metadata so the save
// application workflow does not also carry formatting and progress behavior.
class SaveCatalogPresenter final {
public:
    explicit SaveCatalogPresenter(std::ostream& output) noexcept;

    SaveScanProgress scanProgress();
    void showSummary(bool single_rom, std::size_t rom_count,
                     std::uint32_t loader_start) const;
    void showRoms(const SaveCatalogAnalysis& analysis) const;

private:
    std::ostream& output_;
};

} // namespace ezfadvance
