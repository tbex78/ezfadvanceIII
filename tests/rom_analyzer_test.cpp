#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/rom_analyzer.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> rom(std::size_t size, const std::string& marker = {})
{
    std::vector<std::uint8_t> data(size, 0);
    const auto branch = ezfadvance::CartridgeFormat::makeArmBranch(0xC0);
    data[0] = static_cast<std::uint8_t>(branch);
    data[1] = static_cast<std::uint8_t>(branch >> 8);
    data[2] = static_cast<std::uint8_t>(branch >> 16);
    data[3] = static_cast<std::uint8_t>(branch >> 24);
    for (std::size_t i = 0; i < marker.size(); ++i) data[0x100 + i] = marker[i];
    return data;
}

} // namespace

int main()
{
    const ezfadvance::RomAnalyzer analyzer;

    auto analysis = analyzer.analyze(rom(0x10000));
    assert(analysis.original_entry_target == 0xC0);
    assert(analysis.entry_type == 9);
    assert(analysis.mapping_flag == 3);
    assert(!analysis.has_eeprom_library);
    assert(!analysis.non_sram_save);

    analysis = analyzer.analyze(rom(0x100000));
    assert(analysis.entry_type == 5);

    // Generic FLASH_V is map 6 but intentionally does not trigger the
    // non-SRAM warning because the project can patch that family to SRAM.
    analysis = analyzer.analyze(rom(0x10000, "FLASH_V121"));
    assert(analysis.mapping_flag == 6);
    assert(!analysis.non_sram_save);

    analysis = analyzer.analyze(rom(0x10000, "FLASH512_V130"));
    assert(analysis.mapping_flag == 6);
    assert(analysis.non_sram_save);
    assert(analysis.non_sram_save->family == "FLASH512 (64 KiB Flash)");
    assert(analysis.non_sram_save->signature == "FLASH512_V130");

    // An EEPROM marker without a structurally proven capacity remains
    // unresolved; the writer must continue requiring --mapN=4/5.
    analysis = analyzer.analyze(rom(0x10000, "EEPROM_V124"));
    assert(analysis.has_eeprom_library);
    assert(analysis.mapping_flag == 0);
    assert(analysis.non_sram_save);
    assert(analysis.non_sram_save->family == "EEPROM");

    std::vector<std::uint8_t> invalid(0x10000, 0);
    analysis = analyzer.analyze(invalid);
    assert(!analysis.original_entry_target);

    std::cout << "ROM analyzer tests passed\n";
}
