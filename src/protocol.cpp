#include "ezfadvance/protocol.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace ezfadvance {
namespace {

void preciseDelay(unsigned microseconds)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(microseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        // Intentional busy-wait matching the legacy DLL's command/data gap.
    }
}

void printHex(const std::vector<std::uint8_t>& bytes)
{
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i && i % 16 == 0)
            std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]) << ' ';
    }
    std::cout << std::dec << '\n';
}

} // namespace

Protocol::Protocol(libusb_device_handle* handle)
    : owned_transport_(new BulkTransport(handle)),
      transport_(*owned_transport_)
{
}

Protocol::Protocol(Transport& transport) noexcept
    : transport_(transport)
{
}

std::vector<std::uint8_t> Protocol::command92Two()
{
    return {0x5A,0xA5,0x92,0x02,0x00,0x00,0x00,0x00,
            0x02,0x00,0x00,0x00,0x00};
}

std::vector<std::uint8_t> Protocol::command92One(std::uint8_t selector)
{
    return {0x5A,0xA5,0x92,0x01,selector,0x00,0x00,0x00,
            0x01,0x00,0x00,0x00,0x00};
}

bool Protocol::commandDataEcho(const std::vector<std::uint8_t>& command,
                               const std::vector<std::uint8_t>& data,
                               const std::string& label,
                               CommandEchoOptions options) const
{
    if (!transport_.out(command, options.timeout_ms)) {
        std::cerr << label << ": command OUT failed\n";
        return false;
    }
    if (options.settle_us != 0)
        preciseDelay(options.settle_us);
    if (!transport_.out(data, options.timeout_ms)) {
        std::cerr << label << ": data OUT failed\n";
        return false;
    }

    std::vector<std::uint8_t> response;
    if (!transport_.inMax(response, 64, options.timeout_ms)) {
        std::cerr << label << ": echo IN failed\n";
        return false;
    }
    if (response == command)
        return true;

    std::cerr << label << ": command echo mismatch\n";
    if (options.print_mismatch_bytes) {
        std::cerr << "Expected:\n";
        printHex(command);
        std::cerr << "Received:\n";
        printHex(response);
    }
    return false;
}

bool Protocol::tx92Two(std::uint8_t first,
                       std::uint8_t second,
                       const std::string& label,
                       CommandEchoOptions options) const
{
    return commandDataEcho(command92Two(), {first, second}, label, options);
}

bool Protocol::tx92One(std::uint8_t selector,
                       std::uint8_t value,
                       const std::string& label,
                       CommandEchoOptions options) const
{
    return commandDataEcho(command92One(selector), {value}, label, options);
}

} // namespace ezfadvance
