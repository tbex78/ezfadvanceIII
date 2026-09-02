#pragma once

#include <cstddef>
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
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

inline bool saveWritePathsConflict(
    const std::optional<std::string>& input,
    const std::optional<std::string>& backup) noexcept
{
    return input && backup && *input == *backup;
}

inline bool confirmsSaveWriteWithoutBackup(std::string_view answer) noexcept
{
    while (!answer.empty() &&
           std::isspace(static_cast<unsigned char>(answer.front())))
        answer.remove_prefix(1);
    while (!answer.empty() &&
           std::isspace(static_cast<unsigned char>(answer.back())))
        answer.remove_suffix(1);
    if (answer.size() != 1 && answer.size() != 3) return false;
    std::string normalized(answer);
    for (char& character : normalized)
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    return normalized == "y" || normalized == "yes";
}

inline std::size_t directSaveAccessSize(
    const std::optional<std::size_t>& write_size) noexcept
{
    return write_size.value_or(0x8000);
}

inline bool isDirectSaveBankAccess(
    const std::optional<std::size_t>& requested_rom,
    bool has_requested_bank) noexcept
{
    return has_requested_bank && !requested_rom;
}

inline std::optional<std::size_t> parseConsecutiveBankCount(
    std::string_view text) noexcept
{
    std::size_t count = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), count);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || count < 1 || count > 4)
        return std::nullopt;
    return count;
}

inline SaveSelection selectSaveRom(
    std::size_t rom_count,
    const std::vector<std::size_t>& supported_candidates,
    const std::optional<std::size_t>& requested_rom) noexcept
{
    if (rom_count == 0 ||
        (requested_rom &&
         (*requested_rom == 0 || *requested_rom > rom_count)))
        return {SaveSelectionStatus::out_of_range, 0};
    if (rom_count > 1 && !requested_rom)
        return {SaveSelectionStatus::rom_required, 0};
    if (supported_candidates.empty())
        return {SaveSelectionStatus::no_supported_candidate, 0};
    const std::size_t selected = requested_rom ? *requested_rom - 1 : 0;
    bool supported = false;
    for (const auto candidate : supported_candidates) {
        if (candidate == selected) {
            supported = true;
            break;
        }
    }
    if (!supported)
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
