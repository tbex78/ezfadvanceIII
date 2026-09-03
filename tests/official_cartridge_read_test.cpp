#include "ezfadvance/read_only_cartridge.hpp"
#include "transcript_transport.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
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

void expectPrefix(ezfadvance::test::TranscriptTransport& transcript)
{
    expect92Two(transcript, 0x55,0xAA);
    for (unsigned i = 0; i < 3; ++i) expect92Two(transcript, 0,0);
    expect92Two(transcript, 0xAA,0x55);
    for (unsigned i = 0; i < 3; ++i) expect92Two(transcript, 0,0);
}

void expectReset(ezfadvance::test::TranscriptTransport& transcript, bool f0)
{
    expect92Two(transcript, f0 ? 0xF0 : 0xFF, f0 ? 0x00 : 0xFF);
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
               captured_rom, captured_rom) == CartridgeKind::unknown);
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
               another_rom, another_rom) == CartridgeKind::unknown);

    assert(ezfadvance::hasEz3Catalog(CartridgeKind::ez3_flash));
    assert(!ezfadvance::hasEz3Catalog(CartridgeKind::official_gba_rom));
    assert(!ezfadvance::hasEz3Catalog(CartridgeKind::unknown));

    std::vector<std::uint8_t> erased_header(0xC0, 0xFF);
    assert(ezfadvance::ReadOnlyCartridge::classifyContentHeader(erased_header) ==
           CartridgeKind::ez3_flash);
    erased_header[0] = 0x00;
    assert(ezfadvance::ReadOnlyCartridge::classifyContentHeader(erased_header) ==
           CartridgeKind::unknown);
    assert(ezfadvance::ReadOnlyCartridge::classifyContentHeader(
               std::vector<std::uint8_t>(4, 0xFF)) == CartridgeKind::unknown);
}

void testOfficialDetectionStopsEz3Path()
{
    ezfadvance::test::TranscriptTransport transcript;
    const std::array<std::uint8_t, 4> rom_word = {0x00,0x00,0xA0,0xE1};
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

    auto branch_command = std::vector<std::uint8_t>{
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 4,0,0,0, 0};
    transcript.expectOut(branch_command, timeout_ms)
              .expectInExact(std::vector<std::uint8_t>(rom_word.begin(),
                                                       rom_word.end()),
                             4, timeout_ms);
    std::vector<std::uint8_t> header(0xC0, 0);
    header[0xB2] = 0x96;
    std::uint8_t checksum = 0;
    for (std::size_t index = 0xA0; index <= 0xBC; ++index)
        checksum = static_cast<std::uint8_t>(checksum - header[index]);
    header[0xBD] = static_cast<std::uint8_t>(checksum - 0x19);
    auto header_command = std::vector<std::uint8_t>{
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 0xC0,0,0,0, 0};
    transcript.expectOut(header_command, timeout_ms)
              .expectInExact(header, header.size(), timeout_ms);

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    assert(cartridge.initialize());
    assert(cartridge.kind() == ezfadvance::CartridgeKind::official_gba_rom);
    // Completing here proves no 0040/0080/00c0/0200 probe or 0x95 prime
    // occurred after structural official-cartridge classification.
    assert(transcript.complete());
    for (const auto& command : transcript.observedOut())
        assert(command != ezfadvance::Protocol::command92One(0) &&
               command != ezfadvance::Protocol::command92One(1));
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

void testReadUsesFinalPartialBlock()
{
    ezfadvance::test::TranscriptTransport transcript;
    const std::uint32_t start = 0x00100000u;
    std::vector<std::uint8_t> first_command = {
        0x5A,0xA5,0x91,0x00, 0,0,0,0,
        0x00,0x00,0x01,0x00, 0x00};
    writeLe32(first_command, 4, start / 2);
    std::vector<std::uint8_t> final_command = {
        0x5A,0xA5,0x91,0x00, 0,0,0,0,
        0x02,0x00,0x00,0x00, 0x00};
    writeLe32(final_command, 4, (start + block_size) / 2);
    transcript.expectOut(first_command, timeout_ms)
              .expectInExact(std::vector<std::uint8_t>(block_size, 0xA5),
                             block_size, timeout_ms)
              .expectOut(final_command, timeout_ms)
              .expectInExact({0x12, 0x34}, 2, timeout_ms);

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    std::vector<std::uint8_t> image;
    assert(cartridge.read(start, image, block_size + 2));
    assert(image.size() == block_size + 2);
    assert(image.front() == 0xA5);
    assert(image[block_size] == 0x12);
    assert(image[block_size + 1] == 0x34);
    assert(transcript.complete());
}

void testDecodedCartridgeReads()
{
    ezfadvance::test::TranscriptTransport transcript;

    auto branch_command = std::vector<std::uint8_t>{
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 4,0,0,0, 0};
    const auto instruction =
        ezfadvance::CartridgeFormat::makeArmBranch(0x00100000);
    const std::vector<std::uint8_t> branch_bytes = {
        static_cast<std::uint8_t>(instruction),
        static_cast<std::uint8_t>(instruction >> 8),
        static_cast<std::uint8_t>(instruction >> 16),
        static_cast<std::uint8_t>(instruction >> 24)};
    transcript.expectOut(branch_command, timeout_ms)
              .expectInExact(branch_bytes, branch_bytes.size(), timeout_ms);

    constexpr std::uint32_t header_address = 0x00200000;
    auto header_command = std::vector<std::uint8_t>{
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 0xC0,0,0,0, 0};
    writeLe32(header_command, 4, header_address / 2);
    std::vector<std::uint8_t> header_bytes(0xC0, 0);
    const std::string title = "HEADER TEST";
    std::copy(title.begin(), title.end(), header_bytes.begin() + 0xA0);
    transcript.expectOut(header_command, timeout_ms)
              .expectInExact(header_bytes, header_bytes.size(), timeout_ms);

    ezfadvance::ReadOnlyCartridge cartridge(transcript);
    const auto branch = cartridge.readArmBranch();
    assert(branch);
    const std::array<std::uint8_t, 4> expected_branch = {
        branch_bytes[0], branch_bytes[1], branch_bytes[2], branch_bytes[3]};
    assert(branch->bytes == expected_branch);
    assert(branch->target == 0x00100000);
    const auto header = cartridge.readGbaHeader(header_address);
    assert(header);
    assert(header->title == title);
    assert(transcript.complete());
}

void expectSafeLinearMapping(ezfadvance::test::TranscriptTransport& transcript,
                             unsigned mapping_mib)
{
    expect92Two(transcript, 0xFF, 0xFF);
    expect92Two(transcript, 0x55, 0xAA);
    if (mapping_mib == 32) {
        expect92Two(transcript, 0, 0);
        expect92Two(transcript, 0, 0);
        expect92Two(transcript, 2, 0);
    } else {
        expect92Two(transcript, 2, 0);
        expect92Two(transcript, 0, mapping_mib == 16 ? 0x80 : 0xC0);
        expect92Two(transcript, 0, 0);
    }
    expect92Two(transcript, 0xAA, 0x55);
    expect92Two(transcript, 0, 0);
    expect92Two(transcript, 0, 0);
    expect92Two(transcript, 0, 0);
    expect92Two(transcript, 0xFF, 0xFF);
    expect92Two(transcript, 0x55, 0xAA);
    expect92Two(transcript, 0, 0);
    expect92Two(transcript, 0, 0);
    expect92Two(transcript, 0, 0);
}

void testHighMappingsContainNoSaveWrites()
{
    for (const unsigned mapping_mib : {16u, 24u, 32u}) {
        ezfadvance::test::TranscriptTransport transcript;
        expectSafeLinearMapping(transcript, mapping_mib);
        ezfadvance::ReadOnlyCartridge cartridge(transcript);
        const bool ok = mapping_mib == 16 ? cartridge.prepareLinear16MiB()
                      : mapping_mib == 24 ? cartridge.prepareLinear24MiB()
                                          : cartridge.prepareLinear32MiB();
        assert(ok);
        assert(transcript.complete());
        for (const auto& command : transcript.observedOut())
            assert(command != ezfadvance::Protocol::command92One(0) &&
                   command != ezfadvance::Protocol::command92One(1));
    }
}

void testRepeatedMappingRequestIsIdempotent()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectSafeLinearMapping(transcript, 16);
    ezfadvance::ReadOnlyCartridge cartridge(transcript);

    std::ostringstream output;
    auto* original_buffer = std::cout.rdbuf(output.rdbuf());
    assert(cartridge.prepareLinearForAddress(0x00FFFFFFu));
    assert(cartridge.prepareLinearForAddress(0x00FFFFFFu));
    std::cout.rdbuf(original_buffer);

    const std::string message =
        "Loader/ROM lies above 8 MiB; selecting proven 16-MiB linear read mapping...";
    const auto first = output.str().find(message);
    assert(first != std::string::npos);
    assert(output.str().find(message, first + message.size()) ==
           std::string::npos);
    assert(transcript.complete());
}

} // namespace

int main()
{
    testProbeClassification();
    testOfficialDetectionStopsEz3Path();
    testFull32MiBExtractionTranscript();
    testReadUsesFinalPartialBlock();
    testDecodedCartridgeReads();
    testHighMappingsContainNoSaveWrites();
    testRepeatedMappingRequestIsIdempotent();
}
