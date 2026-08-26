#include "ezfadvance/save_selection.hpp"

#include <cassert>

int main()
{
    using ezfadvance::SaveSelectionStatus;
    using ezfadvance::selectSaveRom;

    const auto single_default = selectSaveRom(1, std::nullopt);
    assert(single_default.status == SaveSelectionStatus::selected);
    assert(single_default.index == 0);

    const auto single_explicit = selectSaveRom(1, 1);
    assert(single_explicit.status == SaveSelectionStatus::selected);
    assert(single_explicit.index == 0);

    assert(selectSaveRom(2, std::nullopt).status ==
           SaveSelectionStatus::rom_required);
    const auto second = selectSaveRom(2, 2);
    assert(second.status == SaveSelectionStatus::selected);
    assert(second.index == 1);
    assert(selectSaveRom(2, 0).status == SaveSelectionStatus::out_of_range);
    assert(selectSaveRom(2, 3).status == SaveSelectionStatus::out_of_range);
    assert(selectSaveRom(0, std::nullopt).status ==
           SaveSelectionStatus::out_of_range);
}
