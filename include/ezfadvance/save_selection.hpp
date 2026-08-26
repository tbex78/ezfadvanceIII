#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace ezfadvance {

enum class SaveSelectionStatus {
    selected,
    rom_required,
    out_of_range,
    no_supported_candidate,
    multiple_supported_candidates,
    requested_rom_mismatch
};

struct SaveSelection {
    SaveSelectionStatus status = SaveSelectionStatus::out_of_range;
    std::size_t index = 0;
};

inline SaveSelection selectSaveRom(
    std::size_t rom_count,
    const std::vector<std::size_t>& supported_candidates,
    const std::optional<std::size_t>& requested_rom) noexcept
{
    if (rom_count == 0 ||
        (requested_rom &&
         (*requested_rom == 0 || *requested_rom > rom_count)))
        return {SaveSelectionStatus::out_of_range, 0};
    if (supported_candidates.empty())
        return {SaveSelectionStatus::no_supported_candidate, 0};
    if (supported_candidates.size() != 1)
        return {SaveSelectionStatus::multiple_supported_candidates, 0};
    if (rom_count > 1 && !requested_rom)
        return {SaveSelectionStatus::rom_required, 0};
    const std::size_t selected = requested_rom ? *requested_rom - 1 : 0;
    if (selected != supported_candidates.front())
        return {SaveSelectionStatus::requested_rom_mismatch, selected};
    return {SaveSelectionStatus::selected, selected};
}

inline std::size_t boundedSaveScanSpan(std::size_t start,
                                       std::size_t allocation_end,
                                       std::size_t evidence_limit) noexcept
{
    if (start >= evidence_limit || allocation_end <= start) return 0;
    const std::size_t bounded_end =
        allocation_end < evidence_limit ? allocation_end : evidence_limit;
    return bounded_end - start;
}

} // namespace ezfadvance
