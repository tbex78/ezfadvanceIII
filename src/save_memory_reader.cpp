#include "ezfadvance/save_memory_reader.hpp"

#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace ezfadvance {
namespace {
constexpr unsigned timeout_ms = 15000;
}

SaveMemoryReader::SaveMemoryReader(libusb_device_handle* handle) noexcept
    : owned_transport_(new BulkTransport(handle)),
      transport_(*owned_transport_), protocol_(transport_)
{
}

SaveMemoryReader::SaveMemoryReader(Transport& transport) noexcept
    : transport_(transport), protocol_(transport_)
{
}

bool SaveMemoryReader::selectBank(std::uint16_t bank_value)
{
    const auto low = static_cast<std::uint8_t>(bank_value & 0xFFu);
    const auto high = static_cast<std::uint8_t>(bank_value >> 8);
    return protocol_.tx92Two(0x55,0xAA,"savebank 55AA") &&
           protocol_.tx92Two(0,0,"savebank 0000 A") &&
           protocol_.tx92Two(0,0,"savebank 0000 B") &&
           protocol_.tx92Two(low,high,"savebank selector") &&
           protocol_.tx92Two(0,0,"savebank 0000 C") &&
           protocol_.tx92Two(0,0,"savebank 0000 D") &&
           protocol_.tx92Two(0,0,"savebank 0000 E") &&
           protocol_.tx92Two(0,0,"savebank 0000 F");
}

bool SaveMemoryReader::readBank32KiB(std::uint16_t bank_value,
                                     std::vector<std::uint8_t>& output)
{
    if (!selectBank(bank_value))
        return false;
    const std::vector<std::uint8_t> command = {
        0x5A,0xA5,0x91,0x01, 0,0,0,0, 0,0x80,0,0, 0};
    if (!transport_.out(command,timeout_ms)) {
        std::cerr << "save read command OUT failed\n";
        return false;
    }
    return transport_.inExact(output,0x8000,timeout_ms);
}

bool SaveMemoryReader::read(std::size_t save_size,
                            std::vector<std::uint8_t>& save,
                            std::uint16_t first_bank)
{
    if (save_size != 0x8000 && save_size != 0x10000)
        throw std::runtime_error(
            "capture reader supports only 32-KiB or 64-KiB reads");

    std::vector<std::uint8_t> first;
    std::cout << "Reading save bank 1"
              << (save_size == 0x10000 ? "/2" : "")
              << " (selector 0x" << std::hex << std::setw(4)
              << std::setfill('0') << first_bank << std::dec
              << ", 32 KiB)...\n";
    if (!readBank32KiB(first_bank,first)) return false;

    save = first;
    if (save_size == 0x10000) {
        std::vector<std::uint8_t> second;
        const auto second_bank = static_cast<std::uint16_t>(first_bank + 0x10u);
        std::cout << "Reading save bank 2/2 (selector 0x" << std::hex
                  << std::setw(4) << std::setfill('0') << second_bank
                  << std::dec << ", 32 KiB)...\n";
        if (!readBank32KiB(second_bank,second)) return false;
        save.insert(save.end(),second.begin(),second.end());
    }
    return save.size() == save_size;
}

} // namespace ezfadvance
