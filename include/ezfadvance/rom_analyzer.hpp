#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

struct NonSramSaveInfo {
    std::string family;
    std::string signature;
};

struct RomAnalysis {
    std::optional<std::uint32_t> original_entry_target;
    std::uint8_t entry_type = 9;
    std::uint8_t mapping_flag = 0;
    bool has_eeprom_library = false;
    std::optional<NonSramSaveInfo> non_sram_save;
};

class RomAnalyzer final {
public:
    RomAnalysis analyze(const std::vector<std::uint8_t>& rom) const;
};

} // namespace ezfadvance
