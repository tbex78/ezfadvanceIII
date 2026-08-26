#pragma once

#include "ezfadvance/card_writer.hpp"

#include <memory>

struct libusb_device_handle;

namespace ezfadvance {

// Creates the concrete capture-derived writer backend. The returned backend
// does not own the libusb handle; UsbDevice must outlive it.
std::unique_ptr<WriterBackend> makeLibusbWriterBackend(
    libusb_device_handle* handle,
    bool verbose);

} // namespace ezfadvance
