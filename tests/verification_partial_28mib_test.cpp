#include "ezfadvance/protocol.hpp"
#include "ezfadvance/verification_session.hpp"
#include "transcript_transport.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;
constexpr std::size_t image_size = 0x1C00000;
constexpr std::size_t block_size =
    ezfadvance::VerificationSession::block_size;

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> readCommand(std::uint32_t byte_address)
{
    std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x91, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    writeLe32(command, 4, byte_address / 2);
    writeLe32(command, 8, static_cast<std::uint32_t>(block_size));
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

void expectStatus(ezfadvance::test::TranscriptTransport& transcript)
{
    expect92Two(transcript, 0xFF, 0xFF);
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
    std::vector<std::uint8_t> image(image_size);
    for (std::size_t i = 0; i < image.size(); ++i) {
        const std::size_t block = i / block_size;
        const std::size_t within_block = i % block_size;
        image[i] = static_cast<std::uint8_t>(
            (within_block * 29u + block * 71u) & 0xFFu);
    }

    ezfadvance::test::TranscriptTransport transcript;
    expectStatus(transcript);
    expect92One(transcript, 0x01, 0x04);
    expect92One(transcript, 0x00, 0x00);
    expect92One(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x55, 0xAA);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);

    for (std::size_t offset = 0; offset < image.size(); offset += block_size) {
        std::vector<std::uint8_t> block(
            image.begin() + static_cast<std::ptrdiff_t>(offset),
            image.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
        transcript
            .expectOut(readCommand(static_cast<std::uint32_t>(offset)),
                       timeout_ms)
            .expectInExact(std::move(block), block_size, timeout_ms);
    }

    std::vector<std::chrono::microseconds> delays;
    ezfadvance::VerificationSession verification(
        transcript,
        [&](std::chrono::microseconds duration) { delays.push_back(duration); });

    std::vector<std::size_t> verified_offsets;
    std::vector<std::size_t> verified_lengths;
    assert(verification.verifyPartial28MiB(
        image,
        [&](std::size_t offset, std::size_t length) {
            verified_offsets.push_back(offset);
            verified_lengths.push_back(length);
        }));

    assert(verified_offsets.size() == 448);
    assert(verified_lengths.size() == 448);
    for (std::size_t block = 0; block < verified_offsets.size(); ++block) {
        assert(verified_offsets[block] == block * block_size);
        assert(verified_lengths[block] == block_size);
    }
    assert(verified_offsets.front() == 0x00000000);
    assert(verified_offsets.back() == 0x01BF0000);
    assert(verified_offsets.back() + verified_lengths.back() == image_size);
    assert(transcript.complete());
    assert(delays.empty());

    assert(observed(transcript, {0x55, 0xAA}));
    assert(observed(transcript, ezfadvance::Protocol::command92One(0x00)));
    assert(observed(transcript, ezfadvance::Protocol::command92One(0x01)));
    assert(observed(transcript, {0x04}));
    assert(!observed(transcript, {0x02, 0x00}));
    assert(!observed(transcript, {0x00, 0x20}));
    assert(!observed(transcript, {0x00, 0x40}));
    assert(!observed(transcript, {0x00, 0x80}));
    assert(!observed(transcript, {0x00, 0xC0}));
    assert(!observed(transcript, {0xAA, 0x55}));
    assert(!observed(transcript, {0xAA}));
    assert(!observed(transcript, {0x55}));
    assert(!observed(transcript, {0x06}));

    bool rejected_wrong_size = false;
    try {
        ezfadvance::test::TranscriptTransport unused_transport;
        ezfadvance::VerificationSession invalid_session(
            unused_transport, [](std::chrono::microseconds) {});
        (void)invalid_session.verifyPartial28MiB(
            std::vector<std::uint8_t>(image_size - 1));
    } catch (const std::invalid_argument&) {
        rejected_wrong_size = true;
    }
    assert(rejected_wrong_size);
}
