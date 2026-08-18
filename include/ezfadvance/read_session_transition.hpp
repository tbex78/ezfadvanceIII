#pragma once

#include "ezfadvance/usb_device.hpp"

#include <chrono>
#include <functional>

namespace ezfadvance {

// Narrow owner of the capture-derived 0x98 readiness behavior shared by the
// read-only cartridge startup and its post-read readiness transition.
class ReadSessionTransition final {
public:
    using DelayCallback =
        std::function<void(std::chrono::milliseconds duration)>;

    ReadSessionTransition(Transport& transport, DelayCallback delay);

    bool waitUntilReady(unsigned attempts = 5);
    bool finishReadinessTransition();

private:
    bool pollReady();

    Transport& transport_;
    DelayCallback delay_;
};

} // namespace ezfadvance
