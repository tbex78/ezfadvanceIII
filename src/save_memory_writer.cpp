#include "ezfadvance/save_memory_writer.hpp"

#include <iostream>

namespace ezfadvance {
namespace {
constexpr unsigned timeout_ms = 15000;
constexpr std::size_t save_size_32k = 0x8000;
}

SaveMemoryWriter::SaveMemoryWriter(libusb_device_handle* handle) noexcept
    : owned_transport_(new BulkTransport(handle)),
      transport_(*owned_transport_), protocol_(transport_)
{
}

SaveMemoryWriter::SaveMemoryWriter(Transport& transport) noexcept
    : transport_(transport), protocol_(transport_)
{
}

bool SaveMemoryWriter::selectBank(std::uint16_t bank_value)
{
    const auto low = static_cast<std::uint8_t>(bank_value & 0xFFu);
    const auto high = static_cast<std::uint8_t>(bank_value >> 8);
    return protocol_.tx92Two(0x55, 0xAA, "savebank 55AA") &&
           protocol_.tx92Two(0, 0, "savebank 0000 A") &&
           protocol_.tx92Two(0, 0, "savebank 0000 B") &&
           protocol_.tx92Two(low, high, "savebank selector") &&
           protocol_.tx92Two(0, 0, "savebank 0000 C") &&
           protocol_.tx92Two(0, 0, "savebank 0000 D") &&
           protocol_.tx92Two(0, 0, "savebank 0000 E") &&
           protocol_.tx92Two(0, 0, "savebank 0000 F");
}

bool SaveMemoryWriter::write32KiB(
    std::uint16_t bank_value,
    const std::vector<std::uint8_t>& save)
{
    if (save.size() != save_size_32k) {
        std::cerr << "save write requires exactly 32768 bytes\n";
        return false;
    }
    if (!selectBank(bank_value))
        return false;

    const std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x92, 0x01, 0, 0, 0, 0,
        0, 0x80, 0, 0, 0};
    if (!transport_.out(command, timeout_ms)) {
        std::cerr << "save write command OUT failed\n";
        return false;
    }
    if (!transport_.out(save, timeout_ms)) {
        std::cerr << "save write payload OUT failed\n";
        return false;
    }

    std::vector<std::uint8_t> echo;
    if (!transport_.inMax(echo, 64, timeout_ms)) {
        std::cerr << "save write command echo IN failed\n";
        return false;
    }
    if (echo != command) {
        std::cerr << "save write command echo mismatch\n";
        return false;
    }
    return true;
}

} // namespace ezfadvance
