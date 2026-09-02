#include "ezfadvance/save_access_planner.hpp"

#include <cassert>

int main()
{
    using ezfadvance::SaveAccessPlanStatus;
    using ezfadvance::SaveAccessPlanner;
    using ezfadvance::SaveAccessRequest;
    using ezfadvance::SaveBankSelector;

    const auto first_bank = SaveBankSelector::parse("0x0900");
    const auto last_bank = SaveBankSelector::parse("0x0930");
    assert(first_bank && last_bank);

    const auto direct = SaveAccessPlanner(
        {std::nullopt, first_bank, 4, std::nullopt}).plan({"FLASH1M"});
    assert(direct);
    assert(direct.direct && direct.explicit_override);
    assert(!direct.selected_rom);
    assert(direct.access_size == 0x20000);
    assert(direct.selector == 0x0900);

    const auto direct_from_write = SaveAccessPlanner(
        {std::nullopt, first_bank, std::nullopt, 0x18000}).plan({});
    assert(direct_from_write && direct_from_write.access_size == 0x18000);

    const auto direct_overflow = SaveAccessPlanner(
        {std::nullopt, last_bank, 2, std::nullopt}).plan({});
    assert(direct_overflow.status ==
           SaveAccessPlanStatus::direct_range_exceeded);

    const auto single = SaveAccessPlanner({}).plan({"SRAM_V111"});
    assert(single && single.selected_rom == 0);
    assert(single.access_size == 0x8000 && single.selector == 0x0900);

    const auto second = SaveAccessPlanner({2}).plan(
        {"FLASH512_V130", "SRAM_V111"});
    assert(second && second.selected_rom == 1);
    assert(second.access_size == 0x8000 && second.selector == 0x0920);

    const auto overridden = SaveAccessPlanner({1, last_bank}).plan(
        {"FLASH512_V130"});
    assert(overridden.status ==
           SaveAccessPlanStatus::explicit_range_exceeded);

    assert(SaveAccessPlanner({}).plan(
               {"SRAM_V111", "SRAM_V111"}).status ==
           SaveAccessPlanStatus::rom_required);
    assert(SaveAccessPlanner({3}).plan({"SRAM_V111"}).status ==
           SaveAccessPlanStatus::rom_out_of_range);
    assert(SaveAccessPlanner({1}).plan({"EEPROM_V124"}).status ==
           SaveAccessPlanStatus::requested_rom_unsupported);
    assert(SaveAccessPlanner({}).plan({"EEPROM_V124"}).status ==
           SaveAccessPlanStatus::no_supported_rom);

    const auto unknown_predecessor = SaveAccessPlanner({2}).plan(
        {"(none)", "SRAM_V111"});
    assert(unknown_predecessor.status ==
           SaveAccessPlanStatus::unknown_predecessor_capacity);
    assert(unknown_predecessor.first_unknown_rom == 0);

    assert(SaveAccessPlanner({2}).plan(
               {"FLASH1M_V103", "SRAM_V111"}).status ==
           SaveAccessPlanStatus::capacity_exceeded);
}
