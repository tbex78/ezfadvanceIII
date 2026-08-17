#pragma once

#include "ezfadvance/usb_device.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace ezfadvance {

struct CommandEchoOptions {
    unsigned timeout_ms = 15000;
    unsigned settle_us = 750;
    bool print_mismatch_bytes = true;
};

// Shared low-level EZ3 command protocol. Higher-level capture-derived state
// sequences remain in their applications until transcript tests cover them.
class Protocol final {
public:
    explicit Protocol(libusb_device_handle* handle);
    explicit Protocol(Transport& transport) noexcept;

    static std::vector<std::uint8_t> command92Two();
    static std::vector<std::uint8_t> command92One(std::uint8_t selector);

    bool commandDataEcho(const std::vector<std::uint8_t>& command,
                         const std::vector<std::uint8_t>& data,
                         const std::string& label,
                         CommandEchoOptions options = {}) const;

    bool tx92Two(std::uint8_t first,
                 std::uint8_t second,
                 const std::string& label,
                 CommandEchoOptions options = {}) const;

    bool tx92One(std::uint8_t selector,
                 std::uint8_t value,
                 const std::string& label,
                 CommandEchoOptions options = {}) const;

private:
    std::unique_ptr<BulkTransport> owned_transport_;
    Transport& transport_;
};

} // namespace ezfadvance
