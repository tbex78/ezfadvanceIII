#include "ezfadvance/rom_analyzer.hpp"

#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/eeprom_mapping.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace ezfadvance {
namespace {

constexpr std::size_t program_block = 0x10000;
constexpr std::size_t maximum_image = 0x2000000;

bool contains(const std::vector<std::uint8_t>& data, const std::string& needle)
{
    return !needle.empty() && data.size() >= needle.size() &&
           std::search(data.begin(), data.end(), needle.begin(), needle.end()) !=
               data.end();
}

std::optional<std::string> asciiSignature(
    const std::vector<std::uint8_t>& rom, const char* prefix,
    std::size_t prefix_length)
{
    const auto found = std::search(
        rom.begin(), rom.end(), prefix, prefix + prefix_length);
    if (found == rom.end()) return std::nullopt;

    std::string signature;
    for (auto current = found;
         current != rom.end() && signature.size() < 20; ++current) {
        if (*current < 0x20 || *current > 0x7E) break;
        signature.push_back(static_cast<char>(*current));
    }
    if (signature.empty()) signature.assign(prefix, prefix_length);
    return signature;
}

std::optional<NonSramSaveInfo> nonSramSave(
    const std::vector<std::uint8_t>& rom)
{
    struct Candidate {
        const char* prefix;
        std::size_t length;
        const char* family;
    };
    static constexpr Candidate candidates[] = {
        {"FLASH1M_V", 9, "FLASH1M (128 KiB Flash)"},
        {"FLASH512_V", 10, "FLASH512 (64 KiB Flash)"},
        {"EEPROM_V", 8, "EEPROM"},
    };
    for (const auto& candidate : candidates) {
        if (const auto signature = asciiSignature(
                rom, candidate.prefix, candidate.length)) {
            return NonSramSaveInfo{candidate.family, *signature};
        }
    }
    return std::nullopt;
}

std::uint8_t entryType(std::size_t size)
{
    // Catalog entry type is a ROM-size class, not a save-type code. Captures
    // establish 32 MiB -> 0 through 64 KiB -> 9. Unusual sizes use the next
    // power-of-two class, matching the placement allocator.
    if (size > maximum_image)
        throw std::runtime_error("ROM file exceeds 256-Mbit cartridge capacity");
    std::size_t size_class = program_block;
    std::uint8_t type = 9;
    while (size_class < size) {
        size_class <<= 1;
        if (type == 0 || size_class > maximum_image)
            throw std::runtime_error("ROM size class exceeds cartridge geometry");
        --type;
    }
    return type;
}

} // namespace

RomAnalysis RomAnalyzer::analyze(const std::vector<std::uint8_t>& rom) const
{
    RomAnalysis result;
    if (rom.size() >= 4) {
        result.original_entry_target =
            CartridgeFormat::armBranchTarget(
                CartridgeFormat::readLe32(rom.data()));
    }
    result.entry_type = entryType(rom.size());
    result.non_sram_save = nonSramSave(rom);
    result.has_eeprom_library = contains(rom, "EEPROM_V");

    // The mapping byte is independent from ROM size. Captured SRAM/non-FLASH
    // cases use 3, FLASH/FLASH512 cases use 6, and FLASH1M uses 7. EEPROM map
    // 4/5 follows the structurally recovered SDK capacity call; an unresolved
    // call remains 0 so the CLI can require an explicit override rather than
    // guess.
    if (result.has_eeprom_library) {
        result.mapping_flag = catalogMapForEeprom(
            detectEepromMapping(rom).capacity);
    } else if (contains(rom, "FLASH1M_V")) {
        result.mapping_flag = 7;
    } else if (contains(rom, "FLASH_V") || contains(rom, "FLASH512_V")) {
        result.mapping_flag = 6;
    } else {
        result.mapping_flag = 3;
    }
    return result;
}

} // namespace ezfadvance
