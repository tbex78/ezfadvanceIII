#include "ezfadvance/ez3_catalog.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::size_t single_count = 0x4E8;
constexpr std::size_t single_count2 = 0x4F6;
constexpr std::size_t single_entry = 0x4F8;
constexpr std::size_t multi_count = 0x475E;
constexpr std::size_t multi_count2 = 0x476C;
constexpr std::size_t multi_entry = 0x476E;
constexpr std::uint32_t image_limit = 0x02000000u;

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] =
            static_cast<std::uint8_t>(value >> (index * 8));
}

void putEntry(std::vector<std::uint8_t>& loader, std::size_t offset,
              const std::string& name, std::uint32_t start, bool first)
{
    for (std::size_t index = 0; index < name.size() && index < 16; ++index)
        loader[offset + index] = static_cast<std::uint8_t>(name[index]);
    loader[offset + 19] = 5;
    const std::uint32_t packed =
        ((first ? 0u : start / 2u) << 8) | 4u;
    put32(loader, offset + 20, packed);
    put32(loader, offset + 24, first ? 0xD4u : start);
}

std::vector<std::uint8_t> multiLoader(std::size_t count)
{
    std::vector<std::uint8_t> loader(
        multi_entry + count * ezfadvance::Ez3CatalogParser::entry_size, 0);
    put16(loader, multi_count, static_cast<std::uint16_t>(count));
    put16(loader, multi_count2, static_cast<std::uint16_t>(count));
    for (std::size_t index = 0; index < count; ++index)
        putEntry(loader,
                 multi_entry + index * ezfadvance::Ez3CatalogParser::entry_size,
                 "ROM" + std::to_string(index + 1),
                 static_cast<std::uint32_t>(index * 0x10000u), index == 0);
    return loader;
}

} // namespace

int main()
{
    using ezfadvance::Ez3CatalogKind;
    using ezfadvance::Ez3CatalogParser;

    std::vector<std::uint8_t> single(
        single_entry + Ez3CatalogParser::entry_size, 0);
    put16(single, single_count, 1);
    put16(single, single_count2, 1);
    putEntry(single, single_entry, "SINGLE", 0, true);
    const auto parsed_single = Ez3CatalogParser::parse(single, image_limit);
    assert(parsed_single);
    assert(parsed_single->kind == Ez3CatalogKind::single);
    assert(parsed_single->entries.size() == 1);
    assert(parsed_single->entries[0].start == 0);
    assert(parsed_single->entries[0].target_or_start == 0xD4u);

    for (const std::size_t count : {2u, 3u, 8u, 120u}) {
        const auto parsed =
            Ez3CatalogParser::parse(multiLoader(count), image_limit);
        assert(parsed);
        assert(parsed->kind == Ez3CatalogKind::multi);
        assert(parsed->entries.size() == count);
        assert(parsed->entries.front().start == 0);
        assert(parsed->entries[1].start == 0x10000u);
        assert(parsed->entries.back().target_or_start ==
               static_cast<std::uint32_t>((count - 1) * 0x10000u));
    }

    auto embedded_loader_bytes = multiLoader(2);
    embedded_loader_bytes[multi_entry + 19] = 2;
    put32(embedded_loader_bytes,
          multi_entry + Ez3CatalogParser::entry_size + 20,
          ((0x00800000u / 2u) << 8) | 3u);
    const auto embedded_loader =
        Ez3CatalogParser::parse(embedded_loader_bytes, image_limit);
    assert(embedded_loader);
    assert(embedded_loader->allocationEnd(0, 0x002CC420u, image_limit) ==
           0x00800000u);
    assert(embedded_loader->allocationEnd(1, 0x002CC420u, image_limit) ==
           0x00900000u);

    // Address authorization and physical size-class geometry are distinct:
    // the save reader may accept starts only below 16 MiB while catalog type
    // sizes still derive from the full 32-MiB cartridge.
    const auto save_policy_catalog = Ez3CatalogParser::parse(
        embedded_loader_bytes, 0x01000000u);
    assert(save_policy_catalog);
    assert(save_policy_catalog->allocationEnd(
               0, 0x002CC420u, image_limit) == 0x00800000u);
    assert(save_policy_catalog->allocationEnd(
               1, 0x002CC420u, image_limit) == 0x00900000u);

    const auto appended_loader =
        Ez3CatalogParser::parse(multiLoader(2), image_limit);
    assert(appended_loader);
    assert(appended_loader->allocationEnd(1, 0x0001A880u, image_limit) ==
           0x0001A880u);
    assert(!appended_loader->allocationEnd(2, 0x0001A880u, image_limit));

    auto mismatched = multiLoader(3);
    put16(mismatched, multi_count2, 2);
    assert(!Ez3CatalogParser::parse(mismatched, image_limit));

    auto too_many = multiLoader(120);
    put16(too_many, multi_count, 121);
    put16(too_many, multi_count2, 121);
    assert(!Ez3CatalogParser::parse(too_many, image_limit));

    auto truncated = multiLoader(3);
    truncated.pop_back();
    assert(!Ez3CatalogParser::parse(truncated, image_limit));

    auto misaligned = multiLoader(2);
    put32(misaligned, multi_entry + Ez3CatalogParser::entry_size + 20,
          ((0x10002u / 2u) << 8) | 4u);
    assert(!Ez3CatalogParser::parse(misaligned, image_limit));

    auto out_of_range = multiLoader(2);
    put32(out_of_range, multi_entry + Ez3CatalogParser::entry_size + 20,
          ((0x01000000u / 2u) << 8) | 4u);
    assert(!Ez3CatalogParser::parse(out_of_range, 0x01000000u));

    // Hardware-observed FFTA + Bios_Dumper layout: ROM 2 begins exactly at
    // 16 MiB and is valid when the caller authorizes the full EZ3 image.
    const auto ffta_bios = Ez3CatalogParser::parse(out_of_range, image_limit);
    assert(ffta_bios);
    assert(ffta_bios->entries.size() == 2);
    assert(ffta_bios->entries[1].start == 0x01000000u);

    auto empty_name = multiLoader(2);
    for (std::size_t index = 0; index < 16; ++index)
        empty_name[multi_entry + Ez3CatalogParser::entry_size + index] = 0;
    assert(!Ez3CatalogParser::parse(empty_name, image_limit));

    assert(!Ez3CatalogParser::parse({}, image_limit));
    assert(!Ez3CatalogParser::parse(single, 0));
}
