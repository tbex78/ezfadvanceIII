#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

enum class SaveBankLayoutStatus {
    selected,
    selected_rom_out_of_range,
    unknown_predecessor_capacity,
    capacity_exceeded
};

struct SaveBankLayout {
    SaveBankLayoutStatus status = SaveBankLayoutStatus::selected_rom_out_of_range;
    std::uint16_t selector = 0;
    std::size_t first_unknown_rom = 0;
};

inline std::optional<std::size_t> saveBankCountForMarker(
    const std::string& marker) noexcept
{
    if (marker.rfind("FLASH1M", 0) == 0)
        return 4;
    if (marker.rfind("FLASH512", 0) == 0 ||
        marker.rfind("FLASH_V", 0) == 0)
        return 2;
    if (marker.rfind("SRAM", 0) == 0 ||
        marker.rfind("EEPROM", 0) == 0)
        return 1;
    return std::nullopt;
}

// Corrected cumulative allocation for the four capture-proven 32-KiB save
// banks. Unlike the original manager's standalone importer, later ROMs do not
// restart at selector 0x0900 and overwrite their predecessors.
inline SaveBankLayout allocateSaveBanks(
    const std::vector<std::string>& markers,
    std::size_t selected_rom,
    std::size_t available_banks = 4) noexcept
{
    if (selected_rom >= markers.size())
        return {SaveBankLayoutStatus::selected_rom_out_of_range, 0, 0};

    std::size_t bank = 0;
    for (std::size_t rom = 0; rom <= selected_rom; ++rom) {
        const auto count = saveBankCountForMarker(markers[rom]);
        if (!count)
            return {SaveBankLayoutStatus::unknown_predecessor_capacity, 0, rom};
        if (bank > available_banks || *count > available_banks - bank)
            return {SaveBankLayoutStatus::capacity_exceeded, 0, rom};
        if (rom == selected_rom) {
            return {SaveBankLayoutStatus::selected,
                    static_cast<std::uint16_t>(0x0900u + bank * 0x10u), 0};
        }
        bank += *count;
    }
    return {SaveBankLayoutStatus::selected_rom_out_of_range, 0, 0};
}

} // namespace ezfadvance
