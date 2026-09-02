#pragma once

#include "ezfadvance/save_bank_access.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ezfadvance {

// Capture-proven read-only access to the EZ3 save-memory windows.
class SaveMemoryReader final {
public:
    explicit SaveMemoryReader(libusb_device_handle* handle) noexcept;
    explicit SaveMemoryReader(Transport& transport) noexcept;

    bool read(std::size_t save_size, std::vector<std::uint8_t>& save,
              std::uint16_t first_bank = 0x0900);

private:
    bool readBank32KiB(std::uint16_t bank_value,
                       std::vector<std::uint8_t>& output);

    std::unique_ptr<BulkTransport> owned_transport_;
    Transport& transport_;
    SaveBankAccess bank_access_;
};

} // namespace ezfadvance
