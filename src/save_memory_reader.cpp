#include "ezfadvance/save_memory_reader.hpp"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace ezfadvance {
namespace {
constexpr unsigned timeout_ms = 15000;
}

SaveMemoryReader::SaveMemoryReader(libusb_device_handle* handle) noexcept
    : owned_transport_(new BulkTransport(handle)),
      transport_(*owned_transport_), bank_access_(transport_)
{
}

SaveMemoryReader::SaveMemoryReader(Transport& transport) noexcept
    : transport_(transport), bank_access_(transport_)
{
}

bool SaveMemoryReader::readBank32KiB(std::uint16_t bank_value,
                                     std::vector<std::uint8_t>& output)
{
    if (!bank_access_.select(bank_value))
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
    if (save_size == 0 || save_size > 4 * 0x8000 ||
        save_size % 0x8000 != 0)
        throw std::runtime_error(
            "capture reader supports one to four consecutive 32-KiB banks");

    const auto bank_count = save_size / 0x8000;
    save.clear();
    save.reserve(save_size);
    for (std::size_t index = 0; index < bank_count; ++index) {
        std::vector<std::uint8_t> bank_data;
        const auto bank = static_cast<std::uint16_t>(
            first_bank + index * 0x10u);
        std::cout << "Reading save bank " << (index + 1)
                  << (bank_count > 1 ? "/" + std::to_string(bank_count) : "")
                  << " (selector 0x" << std::hex << std::setw(4)
                  << std::setfill('0') << bank << std::dec
                  << ", 32 KiB)...\n";
        if (!readBank32KiB(bank, bank_data)) return false;
        save.insert(save.end(), bank_data.begin(), bank_data.end());
    }
    return save.size() == save_size;
}

} // namespace ezfadvance
