#include "ezfadvance/usb_device.hpp"

#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  elif __has_include(<libusb.h>)
#    include <libusb.h>
#  else
#    error "libusb header not found. Install the libusb development package."
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif

#include <ostream>
#include <iostream>

namespace ezfadvance {

void reportUsbOpenFailure(const UsbOpenResult& result,
                          std::ostream& diagnostics,
                          const char* device_name)
{
    switch (result.status) {
    case UsbOpenStatus::success:
        return;
    case UsbOpenStatus::initialization_failed:
        diagnostics << "libusb_init failed: "
                    << libusb_error_name(result.libusb_error) << '\n';
        return;
    case UsbOpenStatus::device_not_found:
        diagnostics << device_name << " VID=0x0E6A PID=0x5088 not found.\n";
        return;
    case UsbOpenStatus::interface_claim_failed:
        diagnostics << "Could not claim interface 0: "
                    << libusb_error_name(result.libusb_error) << '\n';
        return;
    }
}

UsbDevice::~UsbDevice()
{
    close();
}

UsbOpenResult UsbDevice::open(std::ostream& diagnostics)
{
    close();

    int rc = libusb_init(&context_);
    if (rc != 0) {
        context_ = nullptr;
        return {UsbOpenStatus::initialization_failed, rc};
    }

    handle_ = libusb_open_device_with_vid_pid(
        context_, vendor_id, product_id);
    if (!handle_)
        return {UsbOpenStatus::device_not_found, 0};

#if defined(__linux__)
    const int detach_rc = libusb_set_auto_detach_kernel_driver(handle_, 1);
    if (detach_rc != 0) {
        diagnostics << "Warning: libusb auto-detach setup failed: "
                    << libusb_error_name(detach_rc)
                    << " (continuing to interface claim)\n";
    }
#else
    (void)diagnostics;
#endif

    rc = libusb_claim_interface(handle_, interface_number);
    if (rc != 0)
        return {UsbOpenStatus::interface_claim_failed, rc};

    interface_claimed_ = true;
    return {};
}

void UsbDevice::close() noexcept
{
    if (handle_ && interface_claimed_)
        libusb_release_interface(handle_, interface_number);
    interface_claimed_ = false;

    if (handle_)
        libusb_close(handle_);
    handle_ = nullptr;

    if (context_)
        libusb_exit(context_);
    context_ = nullptr;
}

bool BulkTransport::out(const std::uint8_t* data,
                        std::size_t size,
                        unsigned timeout_ms) const
{
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        handle_, endpoint_out, const_cast<unsigned char*>(data),
        static_cast<int>(size), &transferred, timeout_ms);

    if (rc != 0) {
        std::cerr << "BULK OUT failed: " << libusb_error_name(rc)
                  << " (" << rc << "), transferred=" << transferred << '\n';
        return false;
    }
    if (transferred != static_cast<int>(size)) {
        std::cerr << "BULK OUT short transfer: " << transferred
                  << "/" << size << '\n';
        return false;
    }
    return true;
}

bool BulkTransport::inExact(std::vector<std::uint8_t>& data,
                            std::size_t wanted,
                            unsigned timeout_ms) const
{
    if (!inMax(data, wanted, timeout_ms))
        return false;
    if (data.size() != wanted) {
        std::cerr << "BULK IN short transfer: " << data.size()
                  << "/" << wanted << '\n';
        return false;
    }
    return true;
}

bool BulkTransport::inMax(std::vector<std::uint8_t>& data,
                          std::size_t maximum,
                          unsigned timeout_ms) const
{
    data.assign(maximum, 0);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        handle_, endpoint_in, data.data(), static_cast<int>(maximum),
        &transferred, timeout_ms);

    if (rc != 0) {
        std::cerr << "BULK IN failed: " << libusb_error_name(rc)
                  << " (" << rc << "), transferred=" << transferred << '\n';
        data.clear();
        return false;
    }
    data.resize(static_cast<std::size_t>(transferred));
    return true;
}

} // namespace ezfadvance
