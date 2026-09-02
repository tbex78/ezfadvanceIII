#include "ezfadvance/save_memory_writer.hpp"

#include <iostream>

namespace ezfadvance {
namespace {
constexpr unsigned timeout_ms = 15000;
constexpr std::size_t save_size_32k = 0x8000;
}

SaveMemoryWriter::SaveMemoryWriter(libusb_device_handle* handle) noexcept
    : owned_transport_(new BulkTransport(handle)),
      transport_(*owned_transport_), bank_access_(transport_)
{
}

SaveMemoryWriter::SaveMemoryWriter(Transport& transport) noexcept
    : transport_(transport), bank_access_(transport_)
{
}

bool SaveMemoryWriter::write32KiB(
    std::uint16_t bank_value,
    const std::vector<std::uint8_t>& save)
{
    if (save.size() != save_size_32k) {
        std::cerr << "save write requires exactly 32768 bytes\n";
        return false;
    }
    if (!bank_access_.select(bank_value))
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

bool SaveMemoryWriter::write(
    std::uint16_t first_bank,
    const std::vector<std::uint8_t>& save)
{
    if (save.empty() || save.size() % save_size_32k != 0 ||
        save.size() > 4 * save_size_32k) {
        std::cerr << "save write requires 1 to 4 complete 32768-byte banks\n";
        return false;
    }

    for (std::size_t offset = 0; offset < save.size(); offset += save_size_32k) {
        const auto bank = static_cast<std::uint16_t>(
            first_bank + (offset / save_size_32k) * 0x10u);
        const std::vector<std::uint8_t> chunk(
            save.begin() + static_cast<std::ptrdiff_t>(offset),
            save.begin() + static_cast<std::ptrdiff_t>(offset + save_size_32k));
        if (!write32KiB(bank, chunk))
            return false;
    }
    return true;
}

} // namespace ezfadvance
