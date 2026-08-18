#include "ezfadvance/protocol.hpp"
#include "transcript_transport.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <vector>

class RecordingTransport final : public ezfadvance::Transport {
public:
    bool out(const std::uint8_t* data, std::size_t size,
             unsigned timeout_ms) const override
    {
        writes.emplace_back(data, data + size);
        timeouts.push_back(timeout_ms);
        ++out_calls;
        return out_calls != fail_out_call;
    }

    bool inExact(std::vector<std::uint8_t>& data, std::size_t wanted,
                 unsigned timeout_ms) const override
    {
        return receive(data, wanted, timeout_ms, true);
    }

    bool inMax(std::vector<std::uint8_t>& data, std::size_t maximum,
               unsigned timeout_ms) const override
    {
        maxima.push_back(maximum);
        return receive(data, maximum, timeout_ms, false);
    }

    mutable std::vector<std::vector<std::uint8_t>> writes;
    mutable std::vector<unsigned> timeouts;
    mutable std::vector<std::size_t> maxima;
    mutable std::deque<std::vector<std::uint8_t>> responses;
    mutable std::size_t out_calls = 0;
    std::size_t fail_out_call = std::numeric_limits<std::size_t>::max();
    bool fail_in = false;

private:
    bool receive(std::vector<std::uint8_t>& data, std::size_t size,
                 unsigned timeout_ms, bool exact) const
    {
        timeouts.push_back(timeout_ms);
        if (fail_in || responses.empty()) return false;
        data = responses.front();
        responses.pop_front();
        return !exact || data.size() == size;
    }
};

namespace {

const ezfadvance::CommandEchoOptions no_delay{1234, 0, false};

void testCommandLayouts()
{
    assert((ezfadvance::Protocol::command92Two() ==
        std::vector<std::uint8_t>{
            0x5A, 0xA5, 0x92, 0x02, 0x00, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 0x00, 0x00}));

    assert((ezfadvance::Protocol::command92One(0x00) ==
        std::vector<std::uint8_t>{
            0x5A, 0xA5, 0x92, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x00, 0x00}));
    assert((ezfadvance::Protocol::command92One(0xFF) ==
        std::vector<std::uint8_t>{
            0x5A, 0xA5, 0x92, 0x01, 0xFF, 0x00, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x00, 0x00}));
}

void testTranscriptDiagnostics()
{
    {
        ezfadvance::test::TranscriptTransport transport;
        transport.expectOut({0x01}, 10);
        assert(!transport.complete());
        assert(transport.error().find("incomplete") != std::string::npos);
    }
    {
        ezfadvance::test::TranscriptTransport transport;
        assert(!transport.out(std::vector<std::uint8_t>{0x01}, 10));
        assert(transport.error().find("after transcript end") !=
               std::string::npos);
    }
    {
        ezfadvance::test::TranscriptTransport transport;
        transport.expectOut({0x01}, 10);
        assert(!transport.out(std::vector<std::uint8_t>{0x02}, 10));
        assert(transport.error().find("transfer 1 OUT mismatch") !=
               std::string::npos);
        assert(transport.error().find("first differing byte: 0") !=
               std::string::npos);
        assert(transport.error().find("expected (1 bytes)") !=
               std::string::npos);
        assert(transport.error().find("received (1 bytes)") !=
               std::string::npos);
    }
    {
        ezfadvance::test::TranscriptTransport transport;
        transport.expectOut({0x01}, 10);
        assert(!transport.out(std::vector<std::uint8_t>{0x01}, 11));
        assert(transport.error().find("expected timeout") !=
               std::string::npos);
    }
}

void testTranscriptInputModes()
{
    ezfadvance::test::TranscriptTransport transport;
    transport.expectInExact({0x10, 0x20}, 2, 30)
             .expectInMax({0x30}, 64, 40);

    std::vector<std::uint8_t> response;
    assert(transport.inExact(response, 2, 30));
    assert((response == std::vector<std::uint8_t>{0x10, 0x20}));
    assert(transport.inMax(response, 64, 40));
    assert((response == std::vector<std::uint8_t>{0x30}));
    assert(transport.complete());
}

void testMatchingEchoAndTimeouts()
{
    const auto command = ezfadvance::Protocol::command92Two();
    ezfadvance::test::TranscriptTransport transport;
    transport.expectOut(command, 1234)
             .expectOut({0x55, 0xAA}, 1234)
             .expectInMax(command, 64, 1234);
    ezfadvance::Protocol protocol(transport);

    assert(protocol.tx92Two(0x55, 0xAA, "match", no_delay));
    assert(transport.complete());
    assert(transport.error().empty());
}

void testSelectorAndValuePreserved()
{
    const auto command = ezfadvance::Protocol::command92One(0xC7);
    ezfadvance::test::TranscriptTransport transport;
    transport.expectOut(command, 1234)
             .expectOut({0xE3}, 1234)
             .expectInMax(command, 64, 1234);
    ezfadvance::Protocol protocol(transport);

    assert(protocol.tx92One(0xC7, 0xE3, "selector", no_delay));
    assert(transport.complete());
}

void testEchoMismatch()
{
    RecordingTransport transport;
    ezfadvance::Protocol protocol(transport);
    auto response = ezfadvance::Protocol::command92Two();
    response[4] = 1;
    transport.responses.push_back(response);
    assert(!protocol.tx92Two(0, 0, "mismatch", no_delay));
}

void testShortEcho()
{
    RecordingTransport transport;
    ezfadvance::Protocol protocol(transport);
    transport.responses.push_back({0x5A, 0xA5, 0x92});
    assert(!protocol.tx92Two(0, 0, "short echo", no_delay));
}

void testCommandOutFailureStopsSequence()
{
    RecordingTransport transport;
    transport.fail_out_call = 1;
    ezfadvance::Protocol protocol(transport);
    assert(!protocol.tx92Two(0, 0, "command failure", no_delay));
    assert(transport.writes.size() == 1);
    assert(transport.timeouts.size() == 1);
    assert(transport.maxima.empty());
}

void testDataOutFailureStopsBeforeInput()
{
    RecordingTransport transport;
    transport.fail_out_call = 2;
    ezfadvance::Protocol protocol(transport);
    assert(!protocol.tx92Two(0, 0, "data failure", no_delay));
    assert(transport.writes.size() == 2);
    assert(transport.timeouts.size() == 2);
    assert(transport.maxima.empty());
}

void testInputFailure()
{
    RecordingTransport transport;
    transport.fail_in = true;
    ezfadvance::Protocol protocol(transport);
    assert(!protocol.tx92Two(0, 0, "input failure", no_delay));
    assert(transport.writes.size() == 2);
    assert(transport.timeouts.size() == 3);
    assert((transport.maxima == std::vector<std::size_t>{64}));
}

void testCustomSettleDelay()
{
    RecordingTransport transport;
    ezfadvance::Protocol protocol(transport);
    transport.responses.push_back(ezfadvance::Protocol::command92Two());
    const ezfadvance::CommandEchoOptions options{77, 2000, false};

    const auto started = std::chrono::steady_clock::now();
    assert(protocol.tx92Two(0, 0, "delay", options));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(elapsed >= std::chrono::microseconds(2000));
    assert((transport.timeouts == std::vector<unsigned>{77, 77, 77}));
}

} // namespace

int main()
{
    testCommandLayouts();
    testTranscriptDiagnostics();
    testTranscriptInputModes();
    testMatchingEchoAndTimeouts();
    testSelectorAndValuePreserved();
    testEchoMismatch();
    testShortEcho();
    testCommandOutFailureStopsSequence();
    testDataOutFailureStopsBeforeInput();
    testInputFailure();
    testCustomSettleDelay();
}
