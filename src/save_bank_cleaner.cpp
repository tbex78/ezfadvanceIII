#include "ezfadvance/save_bank_cleaner.hpp"

#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace ezfadvance {

bool SaveBankCleaner::clearAll(std::ostream& output, bool report_progress) const
{
    return clearRange(0x0900, 4, output, report_progress);
}

bool SaveBankCleaner::clearRange(std::uint16_t first_selector,
                                 std::size_t bank_count,
                                 std::ostream& output,
                                 bool report_progress) const
{
    constexpr std::uint16_t first = 0x0900;
    constexpr std::uint16_t last = 0x0930;
    constexpr std::uint16_t stride = 0x0010;
    if (bank_count == 0 || bank_count > 4 || first_selector < first ||
        first_selector > last || (first_selector - first) % stride != 0 ||
        first_selector + (bank_count - 1) * stride > last)
        return false;

    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    const std::vector<std::uint8_t> zeros(0x8000, 0);

    for (std::size_t bank = 0; bank < bank_count; ++bank) {
        const auto selector = static_cast<std::uint16_t>(
            first_selector + bank * stride);
        if (report_progress) {
            output << "Clearing save bank " << (bank + 1) << "/"
                   << bank_count << " (selector 0x"
                   << std::hex << std::setw(4) << std::setfill('0') << selector
                   << std::dec << ", 32 KiB)...\n";
        }

        const auto label = std::string("SAVE CLEAR bank ") +
            std::to_string(bank + 1);
        if (!bank_access_.select(selector, label) ||
            !protocol_.commandDataEcho(command, zeros, label + " payload"))
            return false;
    }
    return true;
}

} // namespace ezfadvance
