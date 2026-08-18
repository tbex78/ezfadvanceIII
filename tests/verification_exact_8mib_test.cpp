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

void expectStatus(ezfadvance::test::TranscriptTransport& transcript)
{
    expect92Two(transcript, 0xFF, 0xFF);
    expect92One(transcript, 0x01, 0x04);
    expect92One(transcript, 0x00, 0x00);
    expect92One(transcript, 0x00, 0x00);
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
    constexpr std::size_t image_size =
        ezfadvance::VerificationSession::first_window_size;
    constexpr std::size_t block_size =
        ezfadvance::VerificationSession::block_size;

    std::vector<std::uint8_t> image(image_size);
    for (std::size_t i = 0; i < image.size(); ++i) {
        const std::size_t block = i / block_size;
        const std::size_t within_block = i % block_size;
        image[i] = static_cast<std::uint8_t>(
            (within_block * 37u + block * 53u) & 0xFFu);
    }

    ezfadvance::test::TranscriptTransport transcript;

    expectStatus(transcript);
    expect92Two(transcript, 0x55, 0xAA);
    expect92Two(transcript, 0x02, 0x00);
    expect92Two(transcript, 0x00, 0x40);
    expect92Two(transcript, 0x00, 0x00);

    expect92Two(transcript, 0xAA, 0x55);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);
    expect92One(transcript, 0x00, 0xAA);
    expect92One(transcript, 0x00, 0x55);
    expect92One(transcript, 0x01, 0x06);

    expectStatus(transcript);
    expect92Two(transcript, 0x55, 0xAA);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);
    expect92Two(transcript, 0x00, 0x00);

    for (std::size_t offset = 0; offset < image.size(); offset += block_size) {
        std::vector<std::uint8_t> block(
            image.begin() + static_cast<std::ptrdiff_t>(offset),
            image.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
        transcript
            .expectOut(readCommand(static_cast<std::uint32_t>(offset),
                                   static_cast<std::uint32_t>(block_size)),
                       timeout_ms)
            .expectInExact(std::move(block), block_size, timeout_ms);
    }

    std::vector<std::chrono::microseconds> delays;
    bool delay_position_ok = false;
    ezfadvance::VerificationSession verification(
        transcript,
        [&](std::chrono::microseconds duration) {
            delays.push_back(duration);
            delay_position_ok = transcript.observedOut().size() == 16 &&
                transcript.observedOut().back() ==
                    std::vector<std::uint8_t>{0x00, 0x00};
        });

    std::vector<std::size_t> verified_offsets;
    std::vector<std::size_t> verified_lengths;
    assert(verification.verifyExact8MiB(
        image,
        [&](std::size_t offset, std::size_t length) {
            verified_offsets.push_back(offset);
            verified_lengths.push_back(length);
        }));
    assert(verified_offsets.size() == image_size / block_size);
    assert(verified_lengths.size() == image_size / block_size);
    for (std::size_t block = 0; block < verified_offsets.size(); ++block) {
        assert(verified_offsets[block] == block * block_size);
        assert(verified_lengths[block] == block_size);
    }
    assert(transcript.complete());

    assert(delays.size() == 1);
    assert(delays[0] == std::chrono::microseconds(125000));
    assert(delay_position_ok);

    assert(observed(transcript, {0x00, 0x40}));
    assert(observed(transcript, {0x55, 0xAA}));
    assert(observed(transcript, {0xAA, 0x55}));
    assert(!observed(transcript, {0x00, 0x20}));
    assert(!observed(transcript, {0x00, 0x80}));
    assert(!observed(transcript, {0x00, 0xC0}));

    bool rejected_empty_delay = false;
    try {
        ezfadvance::test::TranscriptTransport unused_transport;
        ezfadvance::VerificationSession invalid_session(
            unused_transport, ezfadvance::VerificationSession::DelayCallback{});
        (void)invalid_session;
    } catch (const std::invalid_argument&) {
        rejected_empty_delay = true;
    }
    assert(rejected_empty_delay);
}
