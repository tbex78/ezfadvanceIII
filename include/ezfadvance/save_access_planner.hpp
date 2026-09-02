#pragma once

#include "ezfadvance/save_bank_selector.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

struct SaveAccessRequest {
    std::optional<std::size_t> rom_number;
    std::optional<SaveBankSelector> explicit_bank;
    std::optional<std::size_t> consecutive_bank_count;
    std::optional<std::size_t> write_size;
};

enum class SaveAccessPlanStatus {
    selected,
    rom_out_of_range,
    rom_required,
    no_supported_rom,
    ambiguous_selection,
    requested_rom_unsupported,
    selected_size_unknown,
    direct_range_exceeded,
    explicit_range_exceeded,
    unknown_predecessor_capacity,
    capacity_exceeded
};

struct SaveAccessPlan {
    SaveAccessPlanStatus status = SaveAccessPlanStatus::rom_out_of_range;
    std::optional<std::size_t> selected_rom;
    std::size_t access_size = 0;
    std::uint16_t selector = 0;
    bool direct = false;
    bool explicit_override = false;
    std::size_t first_unknown_rom = 0;

    explicit operator bool() const noexcept
    {
        return status == SaveAccessPlanStatus::selected;
    }
};

class SaveAccessPlanner final {
public:
    explicit SaveAccessPlanner(SaveAccessRequest request) noexcept;

    SaveAccessPlan plan(const std::vector<std::string>& save_markers) const;

private:
    SaveAccessRequest request_;
};

} // namespace ezfadvance
