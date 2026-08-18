#include "ezfadvance/read_session_transition.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace ezfadvance {
namespace {

constexpr unsigned timeout_ms = 15000;
const std::vector<std::uint8_t> readiness_command = {
    0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};

} // namespace

ReadSessionTransition::ReadSessionTransition(Transport& transport,
                                             DelayCallback delay)
    : transport_(transport), delay_(std::move(delay))
{
    if (!delay_)
        throw std::invalid_argument("read-session delay callback is empty");
}

bool ReadSessionTransition::pollReady()
{
    std::vector<std::uint8_t> response;
    return transport_.out(readiness_command, timeout_ms) &&
           transport_.inExact(response, 1, timeout_ms) &&
           response.size() == 1 && response[0] == 1;
}

bool ReadSessionTransition::waitUntilReady(unsigned attempts)
{
    if (attempts == 0)
        return false;

    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        std::vector<std::uint8_t> response;
        if (!transport_.out(readiness_command, timeout_ms) ||
            !transport_.inExact(response, 1, timeout_ms) ||
            response.size() != 1)
            return false;
        if (response[0] == 1)
            return true;
        if (response[0] != 0)
            return false;
        if (attempt != attempts)
            delay_(std::chrono::milliseconds(100));
    }
    return false;
}

bool ReadSessionTransition::finishReadinessTransition()
{
    for (unsigned poll = 0; poll < 3; ++poll) {
        if (!pollReady())
            return false;
    }
    delay_(std::chrono::milliseconds(1000));
    return true;
}

} // namespace ezfadvance
