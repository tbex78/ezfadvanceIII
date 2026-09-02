#include "ezfadvance/save_access_planner.hpp"

#include "ezfadvance/save_bank_layout.hpp"
#include "ezfadvance/save_selection.hpp"

#include <utility>

namespace ezfadvance {

SaveAccessPlanner::SaveAccessPlanner(SaveAccessRequest request) noexcept
    : request_(std::move(request))
{
}

SaveAccessPlan SaveAccessPlanner::plan(
    const std::vector<std::string>& save_markers) const
{
    const bool direct = isDirectSaveBankAccess(
        request_.rom_number, request_.explicit_bank.has_value());
    if (direct) {
        const std::size_t access_size = request_.consecutive_bank_count
            ? *request_.consecutive_bank_count * SaveBankSelector::bank_size
            : directSaveAccessSize(request_.write_size);
        if (!request_.explicit_bank->accommodates(access_size)) {
            return {SaveAccessPlanStatus::direct_range_exceeded,
                    std::nullopt, access_size,
                    request_.explicit_bank->value(), true, true, 0};
        }
        return {SaveAccessPlanStatus::selected, std::nullopt, access_size,
                request_.explicit_bank->value(), true, true, 0};
    }

    std::vector<std::size_t> supported;
    for (std::size_t index = 0; index < save_markers.size(); ++index) {
        if (supportedSaveSizeForMarker(save_markers[index]))
            supported.push_back(index);
    }
    const auto selection = selectSaveRom(
        save_markers.size(), supported, request_.rom_number);
    switch (selection.status) {
    case SaveSelectionStatus::out_of_range:
        return {SaveAccessPlanStatus::rom_out_of_range};
    case SaveSelectionStatus::rom_required:
        return {SaveAccessPlanStatus::rom_required};
    case SaveSelectionStatus::no_supported_candidate:
        return {SaveAccessPlanStatus::no_supported_rom};
    case SaveSelectionStatus::multiple_supported_candidates:
        return {SaveAccessPlanStatus::ambiguous_selection};
    case SaveSelectionStatus::requested_rom_mismatch:
        return {SaveAccessPlanStatus::requested_rom_unsupported,
                selection.index};
    case SaveSelectionStatus::selected:
        break;
    }

    const auto access_size =
        supportedSaveSizeForMarker(save_markers[selection.index]);
    if (!access_size) {
        return {SaveAccessPlanStatus::selected_size_unknown,
                selection.index};
    }

    if (request_.explicit_bank) {
        if (!request_.explicit_bank->accommodates(*access_size)) {
            return {SaveAccessPlanStatus::explicit_range_exceeded,
                    selection.index, *access_size,
                    request_.explicit_bank->value(), false, true, 0};
        }
        return {SaveAccessPlanStatus::selected, selection.index, *access_size,
                request_.explicit_bank->value(), false, true, 0};
    }

    const auto layout = allocateSaveBanks(save_markers, selection.index);
    if (layout.status == SaveBankLayoutStatus::unknown_predecessor_capacity) {
        return {SaveAccessPlanStatus::unknown_predecessor_capacity,
                selection.index, *access_size, 0, false, false,
                layout.first_unknown_rom};
    }
    if (layout.status == SaveBankLayoutStatus::capacity_exceeded) {
        return {SaveAccessPlanStatus::capacity_exceeded,
                selection.index, *access_size};
    }
    if (layout.status != SaveBankLayoutStatus::selected)
        return {SaveAccessPlanStatus::rom_out_of_range};

    return {SaveAccessPlanStatus::selected, selection.index, *access_size,
            layout.selector};
}

} // namespace ezfadvance
