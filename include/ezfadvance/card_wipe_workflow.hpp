#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <vector>

#include "ezfadvance/flash_window_selector.hpp"
#include "ezfadvance/save_bank_cleaner.hpp"
#include "ezfadvance/usb_device.hpp"

namespace ezfadvance {

class CardWipeWorkflow final {
public:
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    CardWipeWorkflow(Transport& transport,
                     FlashWindowSelector& flash_windows,
                     SaveBankCleaner& save_bank_cleaner,
                     std::ostream& output,
                     std::ostream& error,
                     Sleep sleep = {});

    bool execute();

private:
    bool cartridgeReadyPreflight();
    bool eraseBank(unsigned bank);
    bool eraseSector(std::uint32_t address,
                     unsigned bank,
                     std::size_t sector_index,
                     std::size_t sector_count);
    bool finalCleanup();
    bool verifyBlankLikeCapture();
    bool readRegion(const std::vector<std::uint8_t>& command,
                    std::size_t expected_length,
                    std::vector<std::uint8_t>& result);
    bool tx92Two(std::uint8_t first,
                 std::uint8_t second,
                 const char* label);

    Transport& transport_;
    FlashWindowSelector& flash_windows_;
    SaveBankCleaner& save_bank_cleaner_;
    std::ostream& output_;
    std::ostream& error_;
    Sleep sleep_;
};

} // namespace ezfadvance
