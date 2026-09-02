#include "ezfadvance/save_bank_cleaner.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <cstdint>
#include <sstream>
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
        {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    for (const auto& value : values) {
        transport.expectOut(command, timeout_ms)
                 .expectOut(value, timeout_ms)
                 .expectInMax(command, 64, timeout_ms);
    }
}

void testCapturedFourBankCleanupTranscript()
{
    ezfadvance::test::TranscriptTransport transport;
    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    const std::vector<std::uint8_t> zeros(0x8000, 0);

    for (const std::uint16_t selector : {0x0900, 0x0910, 0x0920, 0x0930}) {
        expectSelector(transport, selector);
        transport.expectOut(command, timeout_ms)
                 .expectOut(zeros, timeout_ms)
                 .expectInMax(command, 64, timeout_ms);
    }

    std::ostringstream output;
    ezfadvance::SaveBankCleaner cleaner(transport);
    assert(cleaner.clearAll(output));
    assert(output.str().find("selector 0x0900") != std::string::npos);
    assert(output.str().find("selector 0x0930") != std::string::npos);
    assert(transport.complete());
}

} // namespace

int main()
{
    testCapturedFourBankCleanupTranscript();
}
