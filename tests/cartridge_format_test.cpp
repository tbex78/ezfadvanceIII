#include "ezfadvance/cartridge_format.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

int main()
{
    using ezfadvance::CartridgeFormat;
    using ezfadvance::CatalogEntry;
    using ezfadvance::GbaHeader;

    const std::uint8_t little_endian[] = {0x78, 0x56, 0x34, 0x12};
    assert(CartridgeFormat::readLe16(little_endian) == 0x5678);
    assert(CartridgeFormat::readLe32(little_endian) == 0x12345678);

    assert(!CartridgeFormat::armBranchTarget(0xFFFFFFFFu));
    assert(CartridgeFormat::armBranchTarget(0xEA00003Eu) == 0x100u);

    std::vector<std::uint8_t> header(0xC0, 0);
    const char title[] = "TEST TITLE  ";
    const char game[] = "TEST";
    const char maker[] = "01";
    for (std::size_t i = 0; i < 12; ++i) header[0xA0 + i] = title[i];
    for (std::size_t i = 0; i < 4; ++i) header[0xAC + i] = game[i];
    for (std::size_t i = 0; i < 2; ++i) header[0xB0 + i] = maker[i];
    header[0xB2] = 0x96;
    header[0xBC] = 7;
    std::uint8_t checksum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i)
        checksum = static_cast<std::uint8_t>(checksum - header[i]);
    header[0xBD] = static_cast<std::uint8_t>(checksum - 0x19);

    const GbaHeader parsed_header = GbaHeader::parse(header);
    assert(parsed_header.readable);
    assert(parsed_header.checksum_ok);
    assert(parsed_header.title == "TEST TITLE");
    assert(parsed_header.game_code == "TEST");
    assert(parsed_header.maker_code == "01");
    assert(parsed_header.version == 7);
    assert(CartridgeFormat::validGbaRomHeader(header));

    auto invalid_fixed_value = header;
    invalid_fixed_value[0xB2] = 0;
    assert(!CartridgeFormat::validGbaRomHeader(invalid_fixed_value));
    auto invalid_checksum = header;
    invalid_checksum[0xBD] ^= 1;
    assert(!CartridgeFormat::validGbaRomHeader(invalid_checksum));
    assert(!CartridgeFormat::validGbaRomHeader(
        std::vector<std::uint8_t>(0xBF, 0)));

    std::vector<std::uint8_t> raw_rom(0x2000000, 0xFF);
    assert(!CartridgeFormat::trimmedGbaRomSize(raw_rom));
    raw_rom[0] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x100000);
    raw_rom[0x0FFFFF] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x100000);
    raw_rom[0x100000] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x200000);
    raw_rom[0x200000] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x400000);
    raw_rom[0x400000] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x800000);
    raw_rom[0x800000] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x1000000);
    raw_rom[0x1000000] = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x2000000);
    raw_rom.back() = 0;
    assert(CartridgeFormat::trimmedGbaRomSize(raw_rom) == 0x2000000);

    std::vector<std::uint8_t> loader(28, 0);
    const char name[] = "CATALOG";
    for (std::size_t i = 0; i < 7; ++i) loader[i] = name[i];
    loader[19] = 5;
    loader[20] = 4;
    loader[21] = 0x00;
    loader[22] = 0x00;
    loader[23] = 0x08;
    loader[24] = 0xD4;

    const CatalogEntry first = CatalogEntry::parse(loader, 0, true);
    assert(first.name == "CATALOG");
    assert(first.type == 5);
    assert(first.mapping == 4);
    assert(first.start == 0);
    assert(first.target_or_start == 0xD4);
    assert(first.plausible(0x02000000u, true));
    assert(first.storedEnd(0x02000000u) == 0x000FFFFFu);

    const CatalogEntry later = CatalogEntry::parse(loader, 0, false);
    assert(later.start == 0x100000u);
    assert(later.plausible(0x02000000u, false));

    std::vector<std::uint8_t> first_rom = {0xAA, 0xBB, 0xCC, 0xDD};
    assert(CartridgeFormat::restoreEz3Entry(first_rom, first, true));
    assert(CartridgeFormat::readLe32(first_rom.data()) ==
           CartridgeFormat::makeArmBranch(0xD4));

    std::vector<std::uint8_t> later_rom = {0x11, 0x22, 0x33, 0x44};
    assert(CartridgeFormat::restoreEz3Entry(later_rom, later, false));
    assert(CartridgeFormat::readLe32(later_rom.data()) == 0x44332211u);

    std::vector<std::uint8_t> reconstructed(0x200, 0x5A);
    assert(CartridgeFormat::reconstructEz3Rom(
        reconstructed, first, true, 0, 0x100, 0x20));
    assert(CartridgeFormat::readLe32(reconstructed.data()) ==
           CartridgeFormat::makeArmBranch(0xD4));
    for (std::size_t i = 0x100; i < 0x120; ++i)
        assert(reconstructed[i] == 0xFF);
    assert(reconstructed[0xFF] == 0x5A);
    assert(reconstructed[0x120] == 0x5A);

    std::vector<std::uint8_t> reconstructed_later(0x100, 0x66);
    reconstructed_later[0] = 0x11;
    reconstructed_later[1] = 0x22;
    reconstructed_later[2] = 0x33;
    reconstructed_later[3] = 0x44;
    assert(CartridgeFormat::reconstructEz3Rom(
        reconstructed_later, later, false, 0x100000, 0x100040, 0x10));
    assert(CartridgeFormat::readLe32(reconstructed_later.data()) ==
           0x44332211u);
    for (std::size_t i = 0x40; i < 0x50; ++i)
        assert(reconstructed_later[i] == 0xFF);

    CatalogEntry invalid_type = later;
    invalid_type.type = 10;
    assert(!invalid_type.storedEnd(0x02000000u));
    CatalogEntry overflowing = first;
    overflowing.type = 0;
    overflowing.start = 0x10000;
    assert(!overflowing.storedEnd(0x02000000u));

    assert(CartridgeFormat::requiredLinearReadLimit(0x007FFFFFu) ==
           0x00800000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x00800000u) ==
           0x01000000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x00FFFFFFu) ==
           0x01000000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x01000000u) ==
           0x01800000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x017FFFFFu) ==
           0x01800000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x01800000u) ==
           0x02000000u);
    assert(CartridgeFormat::requiredLinearReadLimit(0x01FFFFFFu) ==
           0x02000000u);
    assert(!CartridgeFormat::requiredLinearReadLimit(0x02000000u));

    bool threw = false;
    try {
        CatalogEntry::parse(loader, 1, false);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}
