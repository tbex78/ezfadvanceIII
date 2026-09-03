#pragma once

#include "ezfadvance/save_catalog_analyzer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

// Resolves the save dump filename independently from USB and presentation.
class SaveOutputPath final {
public:
    static std::string resolve(
        const std::optional<std::string>& requested_path,
        const std::optional<std::size_t>& selected_rom,
        std::uint16_t selector,
        const std::vector<SaveCatalogRom>& roms);

private:
    static std::string safeComponent(std::string value);
};

} // namespace ezfadvance
