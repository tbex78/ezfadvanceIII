#include "ezfadvance/protocol.hpp"
#include "ezfadvance/verification_session.hpp"
#include "transcript_transport.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> readCommand(std::uint32_t byte_address,
                                      std::uint32_t length)
{
    std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x91, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00
    };
    writeLe32(command, 4, byte_address / 2);
    writeLe32(command, 8, length);
    return command;
}

void expect92Two(ezfadvance::test::TranscriptTransport& transcript,
                 std::uint8_t first, std::uint8_t second)
{
    const auto command = ezfadvance::Protocol::command92Two();
    transcript.expectOut(command, timeout_ms)
              .expectOut({first, second}, timeout_ms)
              .expectInMax(command, 64, timeout_ms);
}

void expect92One(ezfadvance::test::TranscriptTransport& transcript,
                 std::uint8_t selector, std::uint8_t value)
{
    const auto command = ezfadvance::Protocol::command92One(selector);
    transcript.expectOut(command, timeout_ms)
              .expectOut({value}, timeout_ms)
              .expectInMax(command, 64, timeout_ms);
}

bool observed(const ezfadvance::test::TranscriptTransport& transcript,
              const std::vector<std::uint8_t>& payload)
{
    const auto& writes = transcript.observedOut();
    return std::find(writes.begin(), writes.end(), payload) != writes.end();
}

} // namespace

int main()
{
    constexpr std::size_t block_size =
        ezfadvance::VerificationSession::block_size;

    // A one-byte tail in the second block proves rounded verification extent,
    // erased-FF comparison, and consecutive global-linear 0x91 addressing.
    std::vector<std::uint8_t> image(block_size + 1);
    for (std::size_t i = 0; i < image.size(); ++i)
        image[i] = static_cast<std::uint8_t>(i);

    std::vector<std::uint8_t> first_block(
        image.begin(), image.begin() + static_cast<std::ptrdiff_t>(block_size));
    std::vector<std::uint8_t> second_block(block_size, 0xFF);
    second_block[0] = image[block_size];

    ezfadvance::test::TranscriptTransport transcript;

    // Capture-proven partial-first-window transition: FFFF, 04, 00, 00.
    expect92Two(transcript, 0xFF, 0xFF);
    expect92One(transcript, 0x01, 0x04);
    expect92One(transcript, 0x00, 0x00);
    expect92One(transcript, 0x00, 0x00);

    transcript
        .expectOut(readCommand(0x00000000u, block_size), timeout_ms)
        .expectInExact(first_block, block_size, timeout_ms)
        .expectOut(readCommand(0x00010000u, block_size), timeout_ms)
        .expectInExact(second_block, block_size, timeout_ms);

    ezfadvance::VerificationSession verification(transcript);
    std::size_t verified_blocks = 0;
    assert(verification.verifyPartialFirstWindow(
        image,
        [&](std::size_t, std::size_t) { ++verified_blocks; }));
    assert(verified_blocks == 2);
    assert(transcript.complete());

    // Negative evidence: the partial path must not emit an exact-window
    // selector or an unlock/mapping transition.
    assert(!observed(transcript, {0x00, 0x20}));
    assert(!observed(transcript, {0x00, 0x40}));
    assert(!observed(transcript, {0x00, 0x80}));
    assert(!observed(transcript, {0x00, 0xC0}));
    assert(!observed(transcript, {0x55, 0xAA}));
    assert(!observed(transcript, {0xAA, 0x55}));
}
