#include "ezfadvance/save_output_path.hpp"

#include <cassert>
#include <vector>

int main()
{
    std::vector<ezfadvance::SaveCatalogRom> roms(1);
    roms[0].catalog_entry.name = "menu name  ";
    roms[0].header.game_code = "ABCD";

    assert(ezfadvance::SaveOutputPath::resolve(
               "chosen.sav", 0, 0x0900, roms) == "chosen.sav");
    assert(ezfadvance::SaveOutputPath::resolve(
               std::nullopt, std::nullopt, 0x0920, roms) ==
           "save-bank-0920.sav");
    assert(ezfadvance::SaveOutputPath::resolve(
               std::nullopt, 0, 0x0900, roms) == "ABCD.sav");

    roms[0].header.game_code.clear();
    assert(ezfadvance::SaveOutputPath::resolve(
               std::nullopt, 0, 0x0900, roms) == "menu_name.sav");
}
