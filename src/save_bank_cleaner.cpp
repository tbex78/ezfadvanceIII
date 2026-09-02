#include "ezfadvance/save_bank_cleaner.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace ezfadvance {

bool SaveBankCleaner::clearAll(std::ostream& output, bool report_progress) const
{
    static constexpr std::array<std::uint16_t, 4> selectors = {
        0x0900, 0x0910, 0x0920, 0x0930};
    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    const std::vector<std::uint8_t> zeros(0x8000, 0);

    for (std::size_t bank = 0; bank < selectors.size(); ++bank) {
        const auto selector = selectors[bank];
        if (report_progress) {
            output << "Clearing save bank " << (bank + 1) << "/4 (selector 0x"
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
