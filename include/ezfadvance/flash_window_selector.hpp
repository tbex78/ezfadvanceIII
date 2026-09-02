#pragma once

#include "ezfadvance/protocol.hpp"

#include <functional>
#include <iosfwd>

namespace ezfadvance {

struct FlashWindowTiming {
    unsigned timeout_ms = 15000;
    unsigned command_data_settle_us = 750;
    unsigned pre_unlock_settle_us = 125000;
};

class FlashWindowSelector final {
public:
    using Delay = std::function<void(unsigned microseconds)>;

    FlashWindowSelector(Transport& transport,
                        FlashWindowTiming timing,
                        Delay delay = {});

    static FlashWindowTiming writerTiming() noexcept;
    static FlashWindowTiming wipeTiming() noexcept;

    bool select(unsigned window,
                std::ostream* report = nullptr) const;
    bool finishOperation() const;

private:
    bool txTwo(unsigned first, unsigned second,
               const char* label) const;
    bool txOne(unsigned selector, unsigned value,
               const char* label) const;

    Protocol protocol_;
    FlashWindowTiming timing_;
    Delay delay_;
};

} // namespace ezfadvance
