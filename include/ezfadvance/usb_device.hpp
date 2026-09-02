#pragma once

#include <cstdint>
#include <iosfwd>
#include <cstddef>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace ezfadvance {

enum class UsbOpenStatus {
    success,
    initialization_failed,
    device_not_found,
    interface_claim_failed
};

struct UsbOpenResult {
    UsbOpenStatus status = UsbOpenStatus::success;
    int libusb_error = 0;

    explicit operator bool() const noexcept
    {
        return status == UsbOpenStatus::success;
    }
};

void reportUsbOpenFailure(const UsbOpenResult& result,
                          std::ostream& diagnostics,
                          const char* device_name = "EZF Advance III USB device");

// Owns the complete libusb session for one EZ-Flash Advance III device.
// Destruction releases the claimed interface, closes the handle, and exits
// the libusb context in the same order used by the original programs.
class UsbDevice final {
public:
    static constexpr std::uint16_t vendor_id = 0x0E6A;
    static constexpr std::uint16_t product_id = 0x5088;
    static constexpr int interface_number = 0;

    UsbDevice() = default;
    ~UsbDevice();

    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;
    UsbDevice(UsbDevice&&) = delete;
    UsbDevice& operator=(UsbDevice&&) = delete;

    UsbOpenResult open(std::ostream& diagnostics);
    libusb_device_handle* handle() const noexcept { return handle_; }

private:
    void close() noexcept;

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool interface_claimed_ = false;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool out(const std::uint8_t* data, std::size_t size,
                     unsigned timeout_ms) const = 0;
    virtual bool inExact(std::vector<std::uint8_t>& data, std::size_t wanted,
                         unsigned timeout_ms) const = 0;
    virtual bool inMax(std::vector<std::uint8_t>& data, std::size_t maximum,
                       unsigned timeout_ms) const = 0;

    bool out(const std::vector<std::uint8_t>& data,
             unsigned timeout_ms) const
    {
        return out(data.data(), data.size(), timeout_ms);
    }
};

// Non-owning libusb implementation of the transport abstraction.
class BulkTransport final : public Transport {
public:
    static constexpr unsigned char endpoint_out = 0x02;
    static constexpr unsigned char endpoint_in = 0x81;

    explicit BulkTransport(libusb_device_handle* handle) noexcept
        : handle_(handle)
    {
    }

    bool out(const std::uint8_t* data,
             std::size_t size,
             unsigned timeout_ms) const override;

    using Transport::out;

    bool inExact(std::vector<std::uint8_t>& data,
                 std::size_t wanted,
                 unsigned timeout_ms) const override;

    bool inMax(std::vector<std::uint8_t>& data,
               std::size_t maximum,
               unsigned timeout_ms) const override;

private:
    libusb_device_handle* handle_;
};

} // namespace ezfadvance
