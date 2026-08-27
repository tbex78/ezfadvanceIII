#include "ezfadvance/save_selection.hpp"
#include "ezfadvance/save_bank_layout.hpp"

#include <cassert>

int main()
{
    using ezfadvance::SaveSelectionStatus;
    using ezfadvance::selectSaveRom;

    assert(!ezfadvance::saveWritePathsConflict(std::nullopt, std::nullopt));
    assert(!ezfadvance::saveWritePathsConflict("input.sav", std::nullopt));
    assert(!ezfadvance::saveWritePathsConflict(std::nullopt, "backup.sav"));
    assert(!ezfadvance::saveWritePathsConflict("input.sav", "backup.sav"));
    assert(ezfadvance::saveWritePathsConflict("same.sav", "same.sav"));

    const auto single_bank = ezfadvance::allocateSaveBanks({"SRAM_V111"}, 0);
    assert(single_bank.status == ezfadvance::SaveBankLayoutStatus::selected);
    assert(single_bank.selector == 0x0900);

    const auto ffta_dump = ezfadvance::allocateSaveBanks(
        {"FLASH512", "SRAM_V111"}, 1);
    assert(ffta_dump.status == ezfadvance::SaveBankLayoutStatus::selected);
    assert(ffta_dump.selector == 0x0920);
    assert(ezfadvance::saveBankCountForMarker("FLASH1M_V103") == 4);
    assert(ezfadvance::saveBankCountForMarker("EEPROM_V124") == 1);

    assert(ezfadvance::allocateSaveBanks({"(none)", "SRAM_V111"}, 1).status ==
           ezfadvance::SaveBankLayoutStatus::unknown_predecessor_capacity);
    assert(ezfadvance::allocateSaveBanks(
               {"FLASH1M_V103", "SRAM_V111"}, 1).status ==
           ezfadvance::SaveBankLayoutStatus::capacity_exceeded);

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
    const auto first_of_two = selectSaveRom(2, {0, 1}, 1);
    assert(first_of_two.status == SaveSelectionStatus::selected);
    assert(first_of_two.index == 0);
    assert(selectSaveRom(2, {0, 1}, std::nullopt).status ==
           SaveSelectionStatus::rom_required);
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
