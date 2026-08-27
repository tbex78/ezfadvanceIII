#pragma once

#include "ezfadvance/protocol.hpp"

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

    bool read(std::size_t save_size, std::vector<std::uint8_t>& save);

private:
    bool selectBank(std::uint16_t bank_value);
    bool readBank32KiB(std::uint16_t bank_value,
                       std::vector<std::uint8_t>& output);

    std::unique_ptr<BulkTransport> owned_transport_;
    Transport& transport_;
    Protocol protocol_;
};

} // namespace ezfadvance
