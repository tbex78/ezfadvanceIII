#include "ezfadvance/save_selection.hpp"

#include <cassert>

int main()
{
    using ezfadvance::SaveSelectionStatus;
    using ezfadvance::selectSaveRom;

    const auto single_default = selectSaveRom(1, {0}, std::nullopt);
    assert(single_default.status == SaveSelectionStatus::selected);
    assert(single_default.index == 0);

    const auto single_explicit = selectSaveRom(1, {0}, 1);
    assert(single_explicit.status == SaveSelectionStatus::selected);
    assert(single_explicit.index == 0);

    assert(selectSaveRom(2, {1}, std::nullopt).status ==
           SaveSelectionStatus::rom_required);
    const auto second = selectSaveRom(2, {1}, 2);
    assert(second.status == SaveSelectionStatus::selected);
    assert(second.index == 1);
    assert(selectSaveRom(2, {1}, 1).status ==
           SaveSelectionStatus::requested_rom_mismatch);
    assert(selectSaveRom(2, {}, 1).status ==
           SaveSelectionStatus::no_supported_candidate);
    assert(selectSaveRom(2, {0, 1}, 1).status ==
           SaveSelectionStatus::multiple_supported_candidates);
    assert(selectSaveRom(2, {1}, 0).status == SaveSelectionStatus::out_of_range);
    assert(selectSaveRom(2, {1}, 3).status == SaveSelectionStatus::out_of_range);
    assert(selectSaveRom(0, {}, std::nullopt).status ==
           SaveSelectionStatus::out_of_range);

    assert(ezfadvance::boundedSaveScanSpan(0x00f00000, 0x02000000,
                                           0x01000000) == 0x00100000);
    assert(ezfadvance::boundedSaveScanSpan(0x01000000, 0x02000000,
                                           0x01000000) == 0);
    assert(ezfadvance::boundedSaveScanSpan(0x1000, 0x1000,
                                           0x01000000) == 0);
}
