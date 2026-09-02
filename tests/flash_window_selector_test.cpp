#include "ezfadvance/flash_window_selector.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <cstdint>
#include <sstream>
#include <vector>

namespace {

void expectTwo(ezfadvance::test::TranscriptTransport& transport,
               unsigned timeout, std::uint8_t first, std::uint8_t second)
{
    const auto command = ezfadvance::Protocol::command92Two();
    transport.expectOut(command, timeout)
             .expectOut({first, second}, timeout)
             .expectInMax(command, 64, timeout);
}

void expectOne(ezfadvance::test::TranscriptTransport& transport,
               unsigned timeout, std::uint8_t selector, std::uint8_t value)
{
    const auto command = ezfadvance::Protocol::command92One(selector);
    transport.expectOut(command, timeout)
             .expectOut({value}, timeout)
             .expectInMax(command, 64, timeout);
}

void expectWindow(ezfadvance::test::TranscriptTransport& transport,
                  unsigned timeout, unsigned window)
{
    expectTwo(transport, timeout, 0x55, 0xAA);
    expectTwo(transport, timeout, window == 0 ? 0x00 : 0x02, 0x00);
    expectTwo(transport, timeout, 0x00,
              static_cast<std::uint8_t>(window * 0x40));
    expectTwo(transport, timeout, 0x00, 0x00);
    expectTwo(transport, timeout, 0xAA, 0x55);
    expectTwo(transport, timeout, 0x00, 0x00);
    expectTwo(transport, timeout, 0x00, 0x00);
    expectTwo(transport, timeout, 0x00, 0x00);
    expectOne(transport, timeout, 0x00, 0xAA);
    expectOne(transport, timeout, 0x00, 0x55);
    expectOne(transport, timeout, 0x01, 0x06);
}

void expectStatus(ezfadvance::test::TranscriptTransport& transport,
                  unsigned timeout)
{
    expectTwo(transport, timeout, 0xFF, 0xFF);
    expectOne(transport, timeout, 0x01, 0x04);
    expectOne(transport, timeout, 0x00, 0x00);
    expectOne(transport, timeout, 0x00, 0x00);
}

void testWriterProfileAndAllWindows()
{
    const auto timing = ezfadvance::FlashWindowSelector::writerTiming();
    assert(timing.timeout_ms == 15000);
    assert(timing.command_data_settle_us == 750);
    assert(timing.pre_unlock_settle_us == 125000);

    ezfadvance::test::TranscriptTransport transport;
    for (unsigned window = 0; window < 4; ++window)
        expectWindow(transport, timing.timeout_ms, window);
    expectStatus(transport, timing.timeout_ms);

    std::vector<unsigned> delays;
    ezfadvance::FlashWindowSelector selector(
        transport, timing,
        [&](unsigned microseconds) { delays.push_back(microseconds); });
    std::ostringstream report;
    for (unsigned window = 0; window < 4; ++window)
        assert(selector.select(window, &report));
    assert(selector.finishOperation());
    assert(delays == std::vector<unsigned>({125000, 125000, 125000, 125000}));
    assert(report.str().find("window 3 pre-AA55") != std::string::npos);
    assert(transport.complete());
}

void testWipeProfileHasNoSettleDelay()
{
    const auto timing = ezfadvance::FlashWindowSelector::wipeTiming();
    assert(timing.timeout_ms == 5000);
    assert(timing.command_data_settle_us == 0);
    assert(timing.pre_unlock_settle_us == 0);

    ezfadvance::test::TranscriptTransport transport;
    expectWindow(transport, timing.timeout_ms, 2);
    expectStatus(transport, timing.timeout_ms);
    unsigned delay_calls = 0;
    ezfadvance::FlashWindowSelector selector(
        transport, timing, [&](unsigned) { ++delay_calls; });
    assert(selector.select(2));
    assert(selector.finishOperation());
    assert(delay_calls == 0);
    assert(transport.complete());
}

void testInvalidWindowRejectedBeforeUsb()
{
    ezfadvance::test::TranscriptTransport transport;
    ezfadvance::FlashWindowSelector selector(
        transport, ezfadvance::FlashWindowSelector::wipeTiming());
    assert(!selector.select(4));
    assert(transport.complete());
}

} // namespace

int main()
{
    testWriterProfileAndAllWindows();
    testWipeProfileHasNoSettleDelay();
    testInvalidWindowRejectedBeforeUsb();
}
