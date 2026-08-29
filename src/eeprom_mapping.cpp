#include "ezfadvance/eeprom_mapping.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ezfadvance {
namespace {

std::uint16_t read16(const std::vector<std::uint8_t>& rom, std::size_t off)
{
    return static_cast<std::uint16_t>(rom[off]) |
           static_cast<std::uint16_t>(rom[off + 1] << 8);
}

bool decodeThumbBl(const std::vector<std::uint8_t>& rom,
                   std::size_t off,
                   std::size_t& target)
{
    if (off + 4 > rom.size()) return false;
    const std::uint16_t hi = read16(rom, off);
    const std::uint16_t lo = read16(rom, off + 2);
    if ((hi & 0xF800u) != 0xF000u || (lo & 0xF800u) != 0xF800u)
        return false;

    std::int32_t displacement =
        static_cast<std::int32_t>(((hi & 0x07FFu) << 12) |
                                  ((lo & 0x07FFu) << 1));
    if ((displacement & 0x00400000) != 0)
        displacement |= static_cast<std::int32_t>(0xFF800000u);
    const std::int64_t destination =
        static_cast<std::int64_t>(off) + 4 + displacement;
    if (destination < 0 || destination >= static_cast<std::int64_t>(rom.size()))
        return false;
    target = static_cast<std::size_t>(destination);
    return true;
}

bool findRecentMov(const std::vector<std::uint8_t>& rom,
                   std::size_t call,
                   unsigned reg,
                   std::uint8_t& value)
{
    const std::size_t begin = call > 16 ? call - 16 : 0;
    for (std::size_t off = call; off >= begin + 2; off -= 2) {
        const std::uint16_t instruction = read16(rom, off - 2);
        if ((instruction & 0xF800u) == 0x2000u &&
            ((instruction >> 8) & 7u) == reg) {
            value = static_cast<std::uint8_t>(instruction & 0xFFu);
            return true;
        }
        if (off == begin + 2) break;
    }
    return false;
}

EepromCapacity capacityForArgument(std::uint8_t argument)
{
    if (argument == 4) return EepromCapacity::bytes_512;
    if (argument == 0x40) return EepromCapacity::bytes_8192;
    return EepromCapacity::unknown;
}

std::vector<std::size_t> findIdentifyEepromFunctions(
    const std::vector<std::uint8_t>& rom)
{
    // V124 includes a push prologue; V122 begins directly with the shared
    // normalization: truncate r0 to 16 bits, r2=0, compare with 4.
    static constexpr std::uint8_t normalized[] = {
        0x00, 0x04, 0x00, 0x0C, 0x00, 0x22, 0x04, 0x28,
    };
    std::vector<std::size_t> offsets;
    auto it = rom.begin();
    while ((it = std::search(it, rom.end(), std::begin(normalized),
                             std::end(normalized)))
           != rom.end()) {
        const std::size_t off = static_cast<std::size_t>(it - rom.begin());
        // The second accepted argument, 0x40, must occur in the same compact
        // selector. This avoids treating an unrelated function prefix as proof.
        const std::size_t end = std::min(rom.size(), off + 48);
        const std::uint8_t large_compare[] = {0x40, 0x28};
        if (std::search(it + sizeof(normalized), rom.begin() + end,
                        std::begin(large_compare), std::end(large_compare))
            != rom.begin() + end)
            offsets.push_back(off >= 2 && rom[off - 2] == 0x00 &&
                                      rom[off - 1] == 0xB5
                                  ? off - 2 : off);
        ++it;
    }
    return offsets;
}

void mergeDetection(EepromMappingDetection& result,
                    bool& conflict,
                    EepromCapacity capacity,
                    std::size_t evidence)
{
    if (capacity == EepromCapacity::unknown || conflict) return;
    if (result.capacity == EepromCapacity::unknown) {
        result.capacity = capacity;
        result.evidence_offset = evidence;
    } else if (result.capacity != capacity) {
        result = {};
        conflict = true;
    }
}

} // namespace

EepromMappingDetection detectEepromMapping(
    const std::vector<std::uint8_t>& rom)
{
    EepromMappingDetection result;
    bool conflict = false;
    const auto identifiers = findIdentifyEepromFunctions(rom);
    if (identifiers.empty()) return result;

    struct Call { std::size_t offset; std::size_t target; };
    std::vector<Call> calls;
    for (std::size_t off = 0; off + 4 <= rom.size(); off += 2) {
        std::size_t target = 0;
        if (decodeThumbBl(rom, off, target)) calls.push_back({off, target});
    }

    for (const std::size_t identifier : identifiers) {
        for (const Call& direct : calls) {
            if (direct.target != identifier) continue;

            std::uint8_t argument = 0;
            if (findRecentMov(rom, direct.offset, 0, argument))
                mergeDetection(result, conflict, capacityForArgument(argument),
                               direct.offset);

            // Super Monkey Ball Jr.'s observed wrapper accepts mode 3 in r0
            // and the EEPROM capacity argument in r1, then calls IdentifyEeprom.
            // Follow callers into the compact function containing that call.
            const std::size_t wrapper_begin = direct.offset > 64
                ? direct.offset - 64 : 0;
            for (const Call& outer : calls) {
                if (outer.target < wrapper_begin || outer.target > direct.offset)
                    continue;
                std::uint8_t mode = 0;
                std::uint8_t wrapped_argument = 0;
                if (findRecentMov(rom, outer.offset, 0, mode) && mode == 3 &&
                    findRecentMov(rom, outer.offset, 1, wrapped_argument))
                    mergeDetection(result, conflict,
                                   capacityForArgument(wrapped_argument),
                                   outer.offset);
            }
        }
    }
    return result;
}

std::uint8_t catalogMapForEeprom(EepromCapacity capacity)
{
    if (capacity == EepromCapacity::bytes_512) return 4;
    if (capacity == EepromCapacity::bytes_8192) return 5;
    return 0;
}

} // namespace ezfadvance
