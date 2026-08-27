#include "ezfadvance/save_memory_reader.hpp"
#include "ezfadvance/save_memory_writer.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;

void expectSelector(ezfadvance::test::TranscriptTransport& transport,
                    std::uint16_t selector)
{
    const auto command = ezfadvance::Protocol::command92Two();
    const std::vector<std::vector<std::uint8_t>> values = {
        {0x55, 0xAA}, {0, 0}, {0, 0},
        {static_cast<std::uint8_t>(selector & 0xff),
         static_cast<std::uint8_t>(selector >> 8)},
        {0, 0}, {0, 0}, {0, 0}, {0, 0}
    };
    for (const auto& value : values) {
        transport.expectOut(command, timeout_ms)
                 .expectOut(value, timeout_ms)
                 .expectInMax(command, 64, timeout_ms);
    }
}

void testCapturedWriteTranscript()
{
    ezfadvance::test::TranscriptTransport transport;
    expectSelector(transport, 0x0900);

    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    std::vector<std::uint8_t> save(0x8000);
    save[0] = 0x18;
    save[1] = 0x00;
    save[2] = 0x00;
    save[3] = 0xEA;
    transport.expectOut(command, timeout_ms)
             .expectOut(save, timeout_ms)
             .expectInMax(command, 64, timeout_ms);

    ezfadvance::SaveMemoryWriter writer(transport);
    assert(writer.write32KiB(0x0900, save));
    assert(transport.complete());
}

void testWrongSizeRejectedBeforeUsb()
{
    ezfadvance::test::TranscriptTransport transport;
    ezfadvance::SaveMemoryWriter writer(transport);
    assert(!writer.write32KiB(0x0900, std::vector<std::uint8_t>(0x7fff)));
    assert(transport.complete());
}

void testWriteEchoMismatchRejected()
{
    ezfadvance::test::TranscriptTransport transport;
    expectSelector(transport, 0x0900);
    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    const std::vector<std::uint8_t> save(0x8000, 0xA5);
    auto bad_echo = command;
    bad_echo[2] = 0x91;
    transport.expectOut(command, timeout_ms)
             .expectOut(save, timeout_ms)
             .expectInMax(bad_echo, 64, timeout_ms);

    ezfadvance::SaveMemoryWriter writer(transport);
    assert(!writer.write32KiB(0x0900, save));
    assert(transport.complete());
}

void testCapturedReadTranscript()
{
    ezfadvance::test::TranscriptTransport transport;
    expectSelector(transport, 0x0900);
    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x91, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    const std::vector<std::uint8_t> expected(0x8000, 0x3C);
    transport.expectOut(command, timeout_ms)
             .expectInExact(expected, expected.size(), timeout_ms);

    std::vector<std::uint8_t> actual;
    ezfadvance::SaveMemoryReader reader(transport);
    assert(reader.read(0x8000, actual));
    assert(actual == expected);
    assert(transport.complete());
}

} // namespace

int main()
{
    testCapturedWriteTranscript();
    testWrongSizeRejectedBeforeUsb();
    testWriteEchoMismatchRejected();
    testCapturedReadTranscript();
}
