#include <cassert>
#include <sstream>
#include <vector>

#include "ezfadvance/cartridge_layout_presenter.hpp"

int main()
{
    ezfadvance::RomInfo rom;
    rom.path = "sample.gba";
    rom.name = "sample";
    rom.data.resize(4);
    rom.start = 0x10000;
    rom.original_entry_target = 0xc0;
    rom.entry_type = 3;
    rom.mapping_flag = 6;
    const std::vector<ezfadvance::RomInfo> roms{rom};
    const std::vector<std::uint8_t> image{0x12, 0x34, 0x56, 0x78};
    std::ostringstream output;

    ezfadvance::CartridgeLayoutPresenter{output}.print(roms, image, 4);

    const std::string text = output.str();
    assert(text.find("ROM 1: sample") != std::string::npos);
    assert(text.find("byte start: 0x10000") != std::string::npos);
    assert(text.find("catalog type: 3 (size-class rule)") != std::string::npos);
    assert(text.find("mapping flag: 6 (signature/map rule)") != std::string::npos);
    assert(text.find("Image first 4 bytes: 12 34 56 78") !=
           std::string::npos);
}
