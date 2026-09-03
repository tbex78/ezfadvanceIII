#include <cassert>
#include <sstream>

#include "ezfadvance/rom_input_loader.hpp"

int main()
{
    using ezfadvance::RomInputLoader;

    assert(RomInputLoader::deriveCatalogName("/roms/Example Game.gba") ==
           "Example Game");
    assert(RomInputLoader::deriveCatalogName("C:\\roms\\1234567890abcdefXYZ.gba") ==
           "1234567890abcdef");
    assert(RomInputLoader::deriveCatalogName(".gba") == ".gba");
    assert(RomInputLoader::deriveCatalogName("") == "ROM");

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    RomInputLoader loader(input, output, error);
    ezfadvance::RomInfo missing;
    missing.path = "this-file-must-not-exist.gba";
    assert(!loader.load(missing));
    assert(error.str().find("Cannot open ROM") != std::string::npos);
}
