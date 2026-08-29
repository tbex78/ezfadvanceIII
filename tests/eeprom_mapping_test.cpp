#include "ezfadvance/eeprom_mapping.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t>& rom, std::size_t off, std::uint16_t value)
{
    rom[off] = static_cast<std::uint8_t>(value);
    rom[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void putBl(std::vector<std::uint8_t>& rom, std::size_t off, std::size_t target)
{
    const std::int32_t displacement = static_cast<std::int32_t>(target) -
                                      static_cast<std::int32_t>(off + 4);
    put16(rom, off, static_cast<std::uint16_t>(0xF000u |
           ((static_cast<std::uint32_t>(displacement) >> 12) & 0x07FFu)));
    put16(rom, off + 2, static_cast<std::uint16_t>(0xF800u |
           ((static_cast<std::uint32_t>(displacement) >> 1) & 0x07FFu)));
}

void putIdentifier(std::vector<std::uint8_t>& rom, std::size_t off)
{
    const std::uint8_t bytes[] = {
        0x00, 0xB5, 0x00, 0x04, 0x00, 0x0C, 0x00, 0x22, 0x04, 0x28,
        0x07, 0xD1, 0x01, 0x49, 0x02, 0x48, 0x08, 0x60, 0x11, 0xE0,
        0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x28,
    };
    for (std::size_t i = 0; i < sizeof(bytes); ++i) rom[off + i] = bytes[i];
}

} // namespace

int main()
{
    using ezfadvance::EepromCapacity;

    // V124-style direct IdentifyEeprom(0x40).
    std::vector<std::uint8_t> large(0x400, 0);
    putIdentifier(large, 0x300);
    put16(large, 0x100, 0x2040); // movs r0, #0x40
    putBl(large, 0x102, 0x300);
    auto detection = ezfadvance::detectEepromMapping(large);
    assert(detection.capacity == EepromCapacity::bytes_8192);
    assert(ezfadvance::catalogMapForEeprom(detection.capacity) == 5);

    // V122-style caller: mode 3 in r0, capacity 4 in r1, one-level wrapper.
    std::vector<std::uint8_t> small(0x500, 0);
    putIdentifier(small, 0x400);
    put16(small, 0x300, 0xB510); // wrapper function boundary
    putBl(small, 0x320, 0x400);
    put16(small, 0x100, 0x2003); // movs r0, #3
    put16(small, 0x102, 0x2104); // movs r1, #4
    putBl(small, 0x104, 0x300);
    detection = ezfadvance::detectEepromMapping(small);
    assert(detection.capacity == EepromCapacity::bytes_512);
    assert(ezfadvance::catalogMapForEeprom(detection.capacity) == 4);

    // Marker/selector presence without a proven capacity call is unresolved.
    std::vector<std::uint8_t> unknown(0x200, 0);
    putIdentifier(unknown, 0x100);
    detection = ezfadvance::detectEepromMapping(unknown);
    assert(detection.capacity == EepromCapacity::unknown);
    assert(ezfadvance::catalogMapForEeprom(detection.capacity) == 0);

    // Contradictory proven calls must not be resolved by encounter order.
    std::vector<std::uint8_t> conflict(0x500, 0);
    putIdentifier(conflict, 0x400);
    put16(conflict, 0x100, 0x2004);
    putBl(conflict, 0x102, 0x400);
    put16(conflict, 0x200, 0x2040);
    putBl(conflict, 0x202, 0x400);
    detection = ezfadvance::detectEepromMapping(conflict);
    assert(detection.capacity == EepromCapacity::unknown);

    std::cout << "eeprom mapping tests passed\n";
}
