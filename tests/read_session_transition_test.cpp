#include "ezfadvance/read_session_transition.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {

constexpr unsigned timeout_ms = 15000;
const std::vector<std::uint8_t> c98 = {
    0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};

void expectPoll(ezfadvance::test::TranscriptTransport& transcript,
                std::uint8_t response,
                bool out_succeeds = true,
                bool in_succeeds = true)
{
    transcript.expectOut(c98, timeout_ms, out_succeeds);
    if (out_succeeds)
        transcript.expectInExact({response}, 1, timeout_ms, in_succeeds);
}

void testImmediateReadiness()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectPoll(transcript, 1);
    std::vector<std::chrono::milliseconds> delays;
    ezfadvance::ReadSessionTransition transition(
        transcript, [&](auto duration) { delays.push_back(duration); });
    assert(transition.waitUntilReady());
    assert(delays.empty());
    assert(transcript.complete());
}

void testBoundedRetriesThenReady()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectPoll(transcript, 0);
    expectPoll(transcript, 0);
    expectPoll(transcript, 1);
    std::vector<std::chrono::milliseconds> delays;
    ezfadvance::ReadSessionTransition transition(
        transcript, [&](auto duration) { delays.push_back(duration); });
    assert(transition.waitUntilReady());
    assert((delays == std::vector<std::chrono::milliseconds>{
        std::chrono::milliseconds(100), std::chrono::milliseconds(100)}));
    assert(transcript.complete());
}

void testRetryExhaustion()
{
    ezfadvance::test::TranscriptTransport transcript;
    for (unsigned i = 0; i < 5; ++i)
        expectPoll(transcript, 0);
    std::vector<std::chrono::milliseconds> delays;
    ezfadvance::ReadSessionTransition transition(
        transcript, [&](auto duration) { delays.push_back(duration); });
    assert(!transition.waitUntilReady());
    assert(delays.size() == 4);
    for (const auto delay : delays)
        assert(delay == std::chrono::milliseconds(100));
    assert(transcript.complete());
}

void testUnexpectedResponseAndTransportFailure()
{
    {
        ezfadvance::test::TranscriptTransport transcript;
        expectPoll(transcript, 2);
        unsigned delays = 0;
        ezfadvance::ReadSessionTransition transition(
            transcript, [&](auto) { ++delays; });
        assert(!transition.waitUntilReady());
        assert(delays == 0);
        assert(transcript.complete());
    }
    {
        ezfadvance::test::TranscriptTransport transcript;
        expectPoll(transcript, 0, false);
        unsigned delays = 0;
        ezfadvance::ReadSessionTransition transition(
            transcript, [&](auto) { ++delays; });
        assert(!transition.waitUntilReady());
        assert(delays == 0);
        assert(transcript.complete());
    }
}

void testThreePollEpilogueAndSingleDelay()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectPoll(transcript, 1);
    expectPoll(transcript, 1);
    expectPoll(transcript, 1);
    std::vector<std::chrono::milliseconds> delays;
    ezfadvance::ReadSessionTransition transition(
        transcript, [&](auto duration) { delays.push_back(duration); });
    assert(transition.finishReadinessTransition());
    assert((delays == std::vector<std::chrono::milliseconds>{
        std::chrono::milliseconds(1000)}));
    assert(transcript.complete());
}

void testFailedEpilogueHasNoDelay()
{
    ezfadvance::test::TranscriptTransport transcript;
    expectPoll(transcript, 1);
    expectPoll(transcript, 0);
    unsigned delays = 0;
    ezfadvance::ReadSessionTransition transition(
        transcript, [&](auto) { ++delays; });
    assert(!transition.finishReadinessTransition());
    assert(delays == 0);
    assert(transcript.complete());
}

} // namespace

int main()
{
    testImmediateReadiness();
    testBoundedRetriesThenReady();
    testRetryExhaustion();
    testUnexpectedResponseAndTransportFailure();
    testThreePollEpilogueAndSingleDelay();
    testFailedEpilogueHasNoDelay();
    return 0;
}
