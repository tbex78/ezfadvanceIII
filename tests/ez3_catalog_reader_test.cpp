#include "ezfadvance/ez3_catalog_reader.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;
constexpr std::uint32_t image_limit = 0x02000000u;
constexpr std::uint32_t loader_start = 0x00100000u;

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

std::vector<std::uint8_t> branchBytes(std::uint32_t target)
{
    const auto instruction = ezfadvance::CartridgeFormat::makeArmBranch(target);
    return {
        static_cast<std::uint8_t>(instruction),
        static_cast<std::uint8_t>(instruction >> 8),
        static_cast<std::uint8_t>(instruction >> 16),
        static_cast<std::uint8_t>(instruction >> 24)};
}

std::vector<std::uint8_t> singleLoader()
{
    std::vector<std::uint8_t> loader(
        ezfadvance::Ez3CatalogParser::loader_read_size, 0);
    put16(loader, 0x4E8, 1);
    put16(loader, 0x4F6, 1);
    const std::string name = "SINGLE";
    for (std::size_t index = 0; index < name.size(); ++index)
        loader[0x4F8 + index] = static_cast<std::uint8_t>(name[index]);
    loader[0x4F8 + 19] = 5;
    put32(loader, 0x4F8 + 20, 4);
    put32(loader, 0x4F8 + 24, 0xD4);
    return loader;
}

std::vector<std::uint8_t> twoRomLoader()
{
    std::vector<std::uint8_t> loader(
        ezfadvance::Ez3CatalogParser::loader_read_size, 0);
    put16(loader, 0x475E, 2);
    put16(loader, 0x476C, 2);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto offset = 0x476E +
            index * ezfadvance::Ez3CatalogParser::entry_size;
        loader[offset] = static_cast<std::uint8_t>('A' + index);
        loader[offset + 19] = 5;
        put32(loader, offset + 20,
              index == 0 ? 4 : ((0x00080000u / 2u) << 8) | 4u);
        put32(loader, offset + 24, index == 0 ? 0xD4 : 0x00080000u);
    }
    return loader;
}

void expectRead(ezfadvance::test::TranscriptTransport& transcript,
                std::uint32_t address,
                std::vector<std::uint8_t> response)
{
    const auto requested_size = response.size();
    std::vector<std::uint8_t> command = {
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 0,0,0,0, 0};
    put32(command, 4, address / 2);
    put32(command, 8, static_cast<std::uint32_t>(response.size()));
    transcript.expectOut(std::move(command), timeout_ms)
              .expectInExact(std::move(response),
                             requested_size,
                             timeout_ms);
}

void testLoadsSingleCatalog()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectRead(transcript, 0, branchBytes(loader_start));
    expectRead(transcript, loader_start, singleLoader());

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    ezfadvance::Ez3CatalogReader reader(cartridge, image_limit, 8);
    const auto result = reader.read();

    assert(result);
    assert(result.status == ezfadvance::Ez3CatalogReadStatus::loaded);
    assert(result.loader_start == loader_start);
    assert(result.loader_end == loader_start +
        ezfadvance::Ez3CatalogParser::loader_read_size - 1);
    assert(result.catalog->isSingle());
    assert(result.catalog->entries.size() == 1);
    assert(result.catalog->entries.front().name == "SINGLE");
    assert(transcript.complete());
}

void testReportsMissingBranch()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectRead(transcript, 0, {0xFF,0xFF,0xFF,0xFF});

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    ezfadvance::Ez3CatalogReader reader(cartridge, image_limit, 8);
    const auto result = reader.read();

    assert(result.status == ezfadvance::Ez3CatalogReadStatus::missing_branch);
    assert(result.first_bytes[0] == 0xFF);
    assert(!result.catalog);
    assert(transcript.complete());
}

void testRejectsOutOfRangeBranch()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectRead(transcript, 0, branchBytes(image_limit));

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    ezfadvance::Ez3CatalogReader reader(cartridge, image_limit, 8);
    const auto result = reader.read();

    assert(result.status ==
           ezfadvance::Ez3CatalogReadStatus::branch_out_of_range);
    assert(result.loader_start == image_limit);
    assert(!result.catalog);
    assert(transcript.complete());
}

void testEnforcesCallerEntryLimit()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectRead(transcript, 0, branchBytes(loader_start));
    expectRead(transcript, loader_start, twoRomLoader());

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    ezfadvance::Ez3CatalogReader reader(cartridge, image_limit, 1);
    const auto result = reader.read();

    assert(result.status ==
           ezfadvance::Ez3CatalogReadStatus::entry_limit_exceeded);
    assert(!result.catalog);
    assert(transcript.complete());
}

} // namespace

int main()
{
    testLoadsSingleCatalog();
    testReportsMissingBranch();
    testRejectsOutOfRangeBranch();
    testEnforcesCallerEntryLimit();
    return 0;
}
