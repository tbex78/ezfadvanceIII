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

    const CatalogEntry later = CatalogEntry::parse(loader, 0, false);
    assert(later.start == 0x100000u);
    assert(later.plausible(0x02000000u, false));

    bool threw = false;
    try {
        CatalogEntry::parse(loader, 1, false);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}
