#pragma once

#include <cstddef>
#include <optional>

namespace ezfadvance {

enum class SaveSelectionStatus { selected, rom_required, out_of_range };

struct SaveSelection {
    SaveSelectionStatus status = SaveSelectionStatus::out_of_range;
    std::size_t index = 0;
};

inline SaveSelection selectSaveRom(
    std::size_t rom_count,
    const std::optional<std::size_t>& requested_rom) noexcept
{
    if (rom_count == 0 ||
        (requested_rom &&
         (*requested_rom == 0 || *requested_rom > rom_count)))
        return {SaveSelectionStatus::out_of_range, 0};
    if (rom_count > 1 && !requested_rom)
        return {SaveSelectionStatus::rom_required, 0};
    return {SaveSelectionStatus::selected,
            requested_rom ? *requested_rom - 1 : 0};
}

} // namespace ezfadvance
