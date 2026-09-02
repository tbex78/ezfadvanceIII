#pragma once

#include "ezfadvance/save_bank_access.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ezfadvance {

// Capture-proven write access to consecutive 32-KiB EZ3 save windows.
class SaveMemoryWriter final {
public:
    explicit SaveMemoryWriter(libusb_device_handle* handle) noexcept;
    explicit SaveMemoryWriter(Transport& transport) noexcept;

    bool write32KiB(std::uint16_t bank_value,
                    const std::vector<std::uint8_t>& save);
    bool write(std::uint16_t first_bank,
               const std::vector<std::uint8_t>& save);

private:
    std::unique_ptr<BulkTransport> owned_transport_;
    Transport& transport_;
    SaveBankAccess bank_access_;
};

} // namespace ezfadvance
