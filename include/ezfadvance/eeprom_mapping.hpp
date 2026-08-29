#ifndef EZFADVANCE_EEPROM_MAPPING_HPP
#define EZFADVANCE_EEPROM_MAPPING_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ezfadvance {

enum class EepromCapacity {
    unknown,
    bytes_512,
    bytes_8192,
};

struct EepromMappingDetection {
    EepromCapacity capacity = EepromCapacity::unknown;
    std::size_t evidence_offset = 0;
};

// Finds Nintendo SDK IdentifyEeprom code and follows direct Thumb calls (or
// the observed one-level wrapper) to recover its 4-Kbit/64-Kbit argument.
// Unknown and contradictory structures deliberately remain unresolved.
EepromMappingDetection detectEepromMapping(
    const std::vector<std::uint8_t>& rom);

// Returns catalog map 4 for 512-byte EEPROM, map 5 for 8-KiB EEPROM, and 0
// when the ROM structure does not prove either capacity.
std::uint8_t catalogMapForEeprom(EepromCapacity capacity);

} // namespace ezfadvance

#endif
