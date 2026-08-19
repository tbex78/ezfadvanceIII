#include "ezfadvance/read_only_cartridge.hpp"
#include "transcript_transport.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;
constexpr std::size_t block_size = 0x10000;
constexpr std::size_t rom_space_size = 0x2000000;

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void expectCommandData(ezfadvance::test::TranscriptTransport& transcript,
                       std::vector<std::uint8_t> command,
                       std::vector<std::uint8_t> data)
{
    transcript.expectOut(command, timeout_ms)
              .expectOut(std::move(data), timeout_ms)
              .expectInMax(std::move(command), 64, timeout_ms);
}

void expect92Two(ezfadvance::test::TranscriptTransport& transcript,
                 std::uint8_t first, std::uint8_t second,
                 std::uint32_t address = 0)
{
    auto command = ezfadvance::Protocol::command92Two();
    writeLe32(command, 4, address);
    expectCommandData(transcript, std::move(command), {first, second});
}

void expect92One(ezfadvance::test::TranscriptTransport& transcript,
                 std::uint8_t selector, std::uint8_t value)
{
    expectCommandData(transcript, ezfadvance::Protocol::command92One(selector),
                      {value});
}

void expectPrefix(ezfadvance::test::TranscriptTransport& transcript)
{
    expect92Two(transcript, 0x55,0xAA);
    for (unsigned i = 0; i < 3; ++i) expect92Two(transcript, 0,0);
    expect92Two(transcript, 0xAA,0x55);
    for (unsigned i = 0; i < 3; ++i) expect92Two(transcript, 0,0);
    expect92One(transcript, 0,0xAA);
    expect92One(transcript, 0,0x55);
    expect92One(transcript, 1,0x06);
}

void expectReset(ezfadvance::test::TranscriptTransport& transcript, bool f0)
{
    expect92Two(transcript, f0 ? 0xF0 : 0xFF, f0 ? 0x00 : 0xFF);
    expect92One(transcript, 1,0x04);
    expect92One(transcript, 0,0x00);
    expect92One(transcript, 0,0x00);
}

void expectSub2(ezfadvance::test::TranscriptTransport& transcript,
                const std::array<std::uint8_t, 4>& response)
{
    transcript.expectOut({0x5A,0xA5,0x91,0x02, 0,0,0,0,
                          4,0,0,0,0}, timeout_ms)
              .expectInExact(std::vector<std::uint8_t>(response.begin(),
                                                       response.end()),
                             4, timeout_ms);
}

void testProbeClassification()
{
    using ezfadvance::CartridgeKind;
    using Probe = std::array<std::uint8_t, 4>;
    const Probe captured_rom = {0xEE,0x00,0x00,0xEA};

    assert(ezfadvance::ReadOnlyCartridge::classifyProbeBehavior(
               captured_rom, captured_rom) == CartridgeKind::official_gba_rom);
    assert(ezfadvance::ReadOnlyCartridge::classifyProbeBehavior(
               captured_rom, Probe{0x1C,0x00,0xB8,0x00}) ==
           CartridgeKind::ez3_flash);
    assert(ezfadvance::ReadOnlyCartridge::classifyProbeBehavior(
               captured_rom, Probe{0x1C,0x00,0xB9,0x00}) ==
           CartridgeKind::ez3_flash);
    assert(ezfadvance::ReadOnlyCartridge::classifyProbeBehavior(
               captured_rom, Probe{0x12,0x34,0x56,0x78}) ==
           CartridgeKind::unknown);

    // Classification depends on probe behavior, not an ARM branch value.
    const Probe another_rom = {0x00,0x00,0xA0,0xE1};
    assert(ezfadvance::ReadOnlyCartridge::classifyProbeBehavior(
               another_rom, another_rom) == CartridgeKind::official_gba_rom);
}

void testOfficialDetectionStopsEz3Path()
{
    ezfadvance::test::TranscriptTransport transcript;
    const std::array<std::uint8_t, 4> rom_word = {0xEE,0x00,0x00,0xEA};
    const std::vector<std::uint8_t> c97 =
        {0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> c98 =
        {0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> c99 =
        {0x5A,0xA5,0x99,0,1,0,0,0,0,0,0,0,0};
    transcript.expectOut(c97, timeout_ms).expectInExact({0}, 1, timeout_ms)
              .expectOut(c98, timeout_ms).expectInExact({1}, 1, timeout_ms)
              .expectOut(c99, timeout_ms).expectInExact(c99, c99.size(), timeout_ms);

    expectPrefix(transcript);
    expect92Two(transcript, 0xAA,0x00,0x555);
    expect92Two(transcript, 0x55,0x00,0x2AA);
    expect92Two(transcript, 0x90,0x00,0x555);
    expectSub2(transcript, rom_word);
    expect92Two(transcript, 0x90,0x00);
    expectReset(transcript, true);
    expectPrefix(transcript);
    expect92Two(transcript, 0x90,0x00);
    expectSub2(transcript, rom_word);
    expectReset(transcript, false);

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    assert(cartridge.initialize());
    assert(cartridge.kind() == ezfadvance::CartridgeKind::official_gba_rom);
    // Completing here proves no 0040/0080/00c0/0200 probe or 0x95 prime
    // occurred after official-cartridge classification.
    assert(transcript.complete());
}

void testFull32MiBExtractionTranscript()
{
    ezfadvance::test::TranscriptTransport transcript;
    for (std::size_t offset = 0; offset < rom_space_size;
         offset += block_size) {
        std::vector<std::uint8_t> command = {
            0x5A,0xA5,0x91,0x00, 0,0,0,0,
            0x00,0x00,0x01,0x00, 0x00};
        writeLe32(command, 4, static_cast<std::uint32_t>(offset / 2));
        std::vector<std::uint8_t> response(block_size,
            static_cast<std::uint8_t>(offset / block_size));
        transcript.expectOut(std::move(command), timeout_ms)
                  .expectInExact(std::move(response), block_size, timeout_ms);
    }

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    std::vector<std::uint8_t> image;
    assert(cartridge.read(0, image, rom_space_size));
    assert(image.size() == rom_space_size);
    for (std::size_t block = 0; block < rom_space_size / block_size; ++block)
        assert(image[block * block_size] == static_cast<std::uint8_t>(block));
    assert(transcript.complete());

    const auto& writes = transcript.observedOut();
    assert(writes.size() == 512);
    assert(writes.front()[4] == 0 && writes.front()[5] == 0);
    // Final byte offset 0x01ff0000 encodes word address 0x00ff8000.
    assert(writes.back()[4] == 0x00);
    assert(writes.back()[5] == 0x80);
    assert(writes.back()[6] == 0xFF);
    assert(writes.back()[7] == 0x00);
}

} // namespace

int main()
{
    testProbeClassification();
    testOfficialDetectionStopsEz3Path();
    testFull32MiBExtractionTranscript();
}
