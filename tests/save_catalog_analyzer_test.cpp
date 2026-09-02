#include "ezfadvance/save_catalog_analyzer.hpp"
#include "transcript_transport.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] =
            static_cast<std::uint8_t>(value >> (index * 8));
}

void expectRead(ezfadvance::test::TranscriptTransport& transcript,
                std::uint32_t address,
                std::vector<std::uint8_t> response)
{
    const auto requested_size = response.size();
    std::vector<std::uint8_t> command = {
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 0,0,0,0, 0};
    put32(command, 4, address / 2);
    put32(command, 8, static_cast<std::uint32_t>(requested_size));
    transcript.expectOut(std::move(command), timeout_ms)
              .expectInExact(std::move(response), requested_size, timeout_ms);
}

void testAnalyzesHeaderSpanAndBoundaryMarker()
{
    constexpr std::uint32_t loader_start = 0x00010020;
    constexpr std::uint32_t image_limit = 0x02000000;

    ezfadvance::Ez3CatalogLayout catalog;
    catalog.kind = ezfadvance::Ez3CatalogKind::single;
    ezfadvance::CatalogEntry entry;
    entry.name = "BOUNDARY";
    entry.start = 0;
    catalog.entries.push_back(entry);

    std::vector<std::uint8_t> header(0xC0, 0);
    const std::string title = "TEST TITLE";
    const std::string game_code = "TEST";
    std::copy(title.begin(), title.end(), header.begin() + 0xA0);
    std::copy(game_code.begin(), game_code.end(), header.begin() + 0xAC);

    std::vector<std::uint8_t> first_chunk(0x10000, 0xFF);
    first_chunk[0xFFFC] = 'S';
    first_chunk[0xFFFD] = 'R';
    first_chunk[0xFFFE] = 'A';
    first_chunk[0xFFFF] = 'M';
    std::vector<std::uint8_t> tail(0x20, 0xFF);
    const std::string suffix = "_V111";
    std::copy(suffix.begin(), suffix.end(), tail.begin());

    ezfadvance::test::TranscriptTransport transcript;
    expectRead(transcript, 0, std::move(header));
    expectRead(transcript, 0, std::move(first_chunk));
    expectRead(transcript, 0x10000, std::move(tail));

    std::vector<std::tuple<std::uint32_t, std::uint32_t, bool>> progress;
    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    ezfadvance::SaveCatalogAnalyzer analyzer(
        cartridge, image_limit,
        [&progress](std::size_t index, std::size_t count,
                    std::uint32_t scanned, std::uint32_t total,
                    bool finished) {
            assert(index == 0);
            assert(count == 1);
            progress.emplace_back(scanned, total, finished);
        });
    const auto analysis = analyzer.analyze(
        catalog, loader_start, loader_start + 0x7080 - 1);

    assert(analysis);
    assert(analysis.roms.size() == 1);
    const auto& rom = analysis.roms.front();
    assert(rom.catalog_entry.name == "BOUNDARY");
    assert(rom.header.title == title);
    assert(rom.header.game_code == game_code);
    assert(rom.allocation_span == loader_start);
    assert(rom.save_marker == "SRAM_V111");
    assert(progress.size() == 3);
    assert(progress.front() == std::make_tuple(
        0u, loader_start, false));
    assert(progress.back() == std::make_tuple(
        loader_start, loader_start, true));
    assert(transcript.complete());
}

} // namespace

int main()
{
    testAnalyzesHeaderSpanAndBoundaryMarker();
    return 0;
}
