#include "ezfadvance/card_wipe_workflow.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <sstream>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 5000;

void testResetFailureStopsBeforeErase()
{
    ezfadvance::test::TranscriptTransport transport;
    transport.expectOut({0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0}, timeout_ms)
             .expectInMax({1}, 1, timeout_ms);
    ezfadvance::FlashWindowSelector windows(
        transport, ezfadvance::FlashWindowSelector::wipeTiming());
    ezfadvance::SaveBankCleaner cleaner(transport);
    std::ostringstream output;
    std::ostringstream error;
    ezfadvance::CardWipeWorkflow workflow(
        transport, windows, cleaner, output, error,
        [](std::chrono::milliseconds) {});

    assert(!workflow.execute());
    assert(error.str().find("No erase operation was attempted") !=
           std::string::npos);
    assert(transport.complete());
}

void testUnexpectedReadinessStopsBeforeErase()
{
    ezfadvance::test::TranscriptTransport transport;
    transport.expectOut({0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0}, timeout_ms)
             .expectInMax({0}, 1, timeout_ms)
             .expectOut({0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0}, timeout_ms)
             .expectInMax({2}, 1, timeout_ms);
    ezfadvance::FlashWindowSelector windows(
        transport, ezfadvance::FlashWindowSelector::wipeTiming());
    ezfadvance::SaveBankCleaner cleaner(transport);
    std::ostringstream output;
    std::ostringstream error;
    ezfadvance::CardWipeWorkflow workflow(
        transport, windows, cleaner, output, error,
        [](std::chrono::milliseconds) {});

    assert(!workflow.execute());
    assert(error.str().find("Unexpected 0x98 readiness value") !=
           std::string::npos);
    assert(transport.complete());
}

} // namespace

int main()
{
    testResetFailureStopsBeforeErase();
    testUnexpectedReadinessStopsBeforeErase();
}
