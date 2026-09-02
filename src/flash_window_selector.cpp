#include "ezfadvance/flash_window_selector.hpp"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <thread>
#include <utility>

namespace ezfadvance {
namespace {

void sleepForMicroseconds(unsigned microseconds)
{
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

} // namespace

FlashWindowSelector::FlashWindowSelector(Transport& transport,
                                         FlashWindowTiming timing,
                                         Delay delay)
    : protocol_(transport), timing_(timing),
      delay_(delay ? std::move(delay) : Delay(sleepForMicroseconds))
{
}

FlashWindowTiming FlashWindowSelector::writerTiming() noexcept
{
    return {15000, 750, 125000};
}

FlashWindowTiming FlashWindowSelector::wipeTiming() noexcept
{
    return {5000, 0, 0};
}

bool FlashWindowSelector::txTwo(unsigned first, unsigned second,
                                const char* label) const
{
    return protocol_.tx92Two(
        static_cast<std::uint8_t>(first),
        static_cast<std::uint8_t>(second), label,
        {timing_.timeout_ms, timing_.command_data_settle_us, true});
}

bool FlashWindowSelector::txOne(unsigned selector, unsigned value,
                                const char* label) const
{
    return protocol_.tx92One(
        static_cast<std::uint8_t>(selector),
        static_cast<std::uint8_t>(value), label,
        {timing_.timeout_ms, timing_.command_data_settle_us, true});
}

bool FlashWindowSelector::select(unsigned window, std::ostream* report) const
{
    if (window > 3)
        return false;

    const unsigned bank_mode = window == 0 ? 0x00 : 0x02;
    const unsigned bank_select_high = window * 0x40;

    if (!txTwo(0x55, 0xAA, "FLASH WINDOW 55AA") ||
        !txTwo(bank_mode, 0x00, "FLASH WINDOW MODE") ||
        !txTwo(0x00, bank_select_high, "FLASH WINDOW SELECT") ||
        !txTwo(0x00, 0x00, "FLASH WINDOW 0000 A"))
        return false;

    if (timing_.pre_unlock_settle_us != 0) {
        if (report) {
            *report << "    window " << window << " pre-AA55 unlock settle: "
                    << timing_.pre_unlock_settle_us << " us\n";
        }
        delay_(timing_.pre_unlock_settle_us);
    }

    return txTwo(0xAA, 0x55, "FLASH WINDOW AA55") &&
           txTwo(0x00, 0x00, "FLASH WINDOW 0000 B") &&
           txTwo(0x00, 0x00, "FLASH WINDOW 0000 C") &&
           txTwo(0x00, 0x00, "FLASH WINDOW 0000 D") &&
           txOne(0x00, 0xAA, "FLASH WINDOW AA") &&
           txOne(0x00, 0x55, "FLASH WINDOW 55") &&
           txOne(0x01, 0x06, "FLASH WINDOW 06");
}

bool FlashWindowSelector::finishOperation() const
{
    return txTwo(0xFF, 0xFF, "FLASH STATUS FFFF") &&
           txOne(0x01, 0x04, "FLASH STATUS 04") &&
           txOne(0x00, 0x00, "FLASH STATUS 00 A") &&
           txOne(0x00, 0x00, "FLASH STATUS 00 B");
}

} // namespace ezfadvance
