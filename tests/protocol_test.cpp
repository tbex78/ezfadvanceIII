#include "ezfadvance/protocol.hpp"

#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>

class RecordingTransport final : public ezfadvance::Transport {
public:
    bool out(const std::uint8_t* data, std::size_t size,
             unsigned timeout_ms) const override
    {
        writes.emplace_back(data, data + size);
        timeouts.push_back(timeout_ms);
        return true;
    }

    bool inExact(std::vector<std::uint8_t>& data, std::size_t wanted,
                 unsigned timeout_ms) const override
    {
        return receive(data, wanted, timeout_ms, true);
    }

    bool inMax(std::vector<std::uint8_t>& data, std::size_t maximum,
               unsigned timeout_ms) const override
    {
        return receive(data, maximum, timeout_ms, false);
    }

    mutable std::vector<std::vector<std::uint8_t>> writes;
    mutable std::vector<unsigned> timeouts;
    mutable std::deque<std::vector<std::uint8_t>> responses;

private:
    bool receive(std::vector<std::uint8_t>& data, std::size_t size,
                 unsigned timeout_ms, bool exact) const
    {
        if (responses.empty()) return false;
        data = responses.front();
        responses.pop_front();
        timeouts.push_back(timeout_ms);
        return !exact || data.size() == size;
    }
};

int main()
{
    RecordingTransport transport;
    ezfadvance::Protocol protocol(transport);
    const auto command = ezfadvance::Protocol::command92Two();
    transport.responses.push_back(command);

    const ezfadvance::CommandEchoOptions options{1234, 0, false};
    assert(protocol.tx92Two(0x55, 0xAA, "test", options));
    assert(transport.writes.size() == 2);
    assert(transport.writes[0] == command);
    assert((transport.writes[1] == std::vector<std::uint8_t>{0x55, 0xAA}));
    assert(transport.timeouts.size() == 3);
    assert(transport.timeouts[0] == 1234);
    assert(transport.timeouts[1] == 1234);
    assert(transport.timeouts[2] == 1234);

    const auto one = ezfadvance::Protocol::command92One(7);
    assert(one.size() == 13);
    assert(one[2] == 0x92);
    assert(one[3] == 0x01);
    assert(one[4] == 7);
    assert(one[8] == 1);
}
