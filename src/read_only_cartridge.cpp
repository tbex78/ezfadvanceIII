#include "ezfadvance/read_only_cartridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace ezfadvance {
namespace {

constexpr unsigned timeout_ms = 15000;

void writeLe32(std::vector<std::uint8_t>& bytes,
               std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void printHex(const std::uint8_t* bytes, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        if (i && i % 16 == 0) std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]) << ' ';
    }
    std::cout << std::dec << '\n';
}

} // namespace

ReadOnlyCartridge::ReadOnlyCartridge(libusb_device_handle* handle) noexcept
    : transport_(handle), protocol_(transport_)
{
}

bool ReadOnlyCartridge::read(std::uint32_t byte_address,
                             std::uint8_t* destination,
                             std::size_t length)
{
    if (byte_address & 1u)
        throw std::runtime_error("read address must be word-aligned");
    std::size_t done = 0;
    while (done < length) {
        const auto piece = std::min<std::size_t>(0x10000, length - done);
        const auto address = byte_address + static_cast<std::uint32_t>(done);
        std::vector<std::uint8_t> command = {
            0x5A,0xA5,0x91,0x00, 0,0,0,0, 0,0,0,0, 0x00};
        writeLe32(command, 4, address / 2u);
        writeLe32(command, 8, static_cast<std::uint32_t>(piece));
        if (!transport_.out(command, timeout_ms)) return false;
        std::vector<std::uint8_t> input;
        if (!transport_.inExact(input, piece, timeout_ms)) return false;
        std::copy(input.begin(), input.end(), destination + done);
        done += piece;
    }
    return true;
}

bool ReadOnlyCartridge::read(std::uint32_t byte_address,
                             std::vector<std::uint8_t>& output,
                             std::size_t length)
{
    output.assign(length, 0);
    return read(byte_address, output.data(), output.size());
}

bool ReadOnlyCartridge::startup()
{
    const std::vector<std::uint8_t> c97 = {0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> c98 = {0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> c99 = {0x5A,0xA5,0x99,0,1,0,0,0,0,0,0,0,0};
    std::vector<std::uint8_t> response;
    return transport_.out(c97, timeout_ms) &&
           transport_.inExact(response, 1, timeout_ms) && response[0] == 0 &&
           transport_.out(c98, timeout_ms) &&
           transport_.inExact(response, 1, timeout_ms) && response[0] == 1 &&
           transport_.out(c99, timeout_ms) &&
           transport_.inExact(response, c99.size(), timeout_ms) && response == c99;
}

bool ReadOnlyCartridge::probeUnlockTail()
{
    return protocol_.tx92Two(0xAA,0x55,"probe AA55") &&
           protocol_.tx92Two(0,0,"probe zero 1") &&
           protocol_.tx92Two(0,0,"probe zero 2") &&
           protocol_.tx92Two(0,0,"probe zero 3") &&
           protocol_.tx92One(0,0xAA,"probe selector0 AA") &&
           protocol_.tx92One(0,0x55,"probe selector0 55") &&
           protocol_.tx92One(1,0x06,"probe selector1 06");
}

bool ReadOnlyCartridge::probePrefix(std::uint8_t a0, std::uint8_t a1,
                                    std::uint8_t b0, std::uint8_t b1,
                                    std::uint8_t c0, std::uint8_t c1,
                                    bool include_tail)
{
    if (!protocol_.tx92Two(0x55,0xAA,"probe 55AA") ||
        !protocol_.tx92Two(a0,a1,"probe word 1") ||
        !protocol_.tx92Two(b0,b1,"probe word 2") ||
        !protocol_.tx92Two(c0,c1,"probe word 3")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return !include_tail || probeUnlockTail();
}

bool ReadOnlyCartridge::probeReset(bool use_f0)
{
    return protocol_.tx92Two(use_f0 ? 0xF0 : 0xFF,
                             use_f0 ? 0x00 : 0xFF,"probe ID reset") &&
           protocol_.tx92One(1,4,"probe status 1/04") &&
           protocol_.tx92One(0,0,"probe status 0/00 #1") &&
           protocol_.tx92One(0,0,"probe status 0/00 #2");
}

bool ReadOnlyCartridge::tx92TwoAt(std::uint32_t address,
                                  std::uint8_t first, std::uint8_t second,
                                  const char* label)
{
    auto command = Protocol::command92Two();
    writeLe32(command, 4, address);
    return protocol_.commandDataEcho(command, {first, second}, label);
}

bool ReadOnlyCartridge::read91Sub2Four()
{
    const std::vector<std::uint8_t> command = {
        0x5A,0xA5,0x91,0x02, 0,0,0,0, 4,0,0,0,0};
    std::vector<std::uint8_t> response;
    return transport_.out(command, timeout_ms) &&
           transport_.inExact(response, 4, timeout_ms);
}

bool ReadOnlyCartridge::flashIdProbe(std::uint8_t a0, std::uint8_t a1,
                                     std::uint8_t b0, std::uint8_t b1,
                                     std::uint8_t c0, std::uint8_t c1)
{
    return probePrefix(a0,a1,b0,b1,c0,c1) &&
           protocol_.tx92Two(0x90,0,"probe 90/00") &&
           read91Sub2Four() && probeReset(false);
}

bool ReadOnlyCartridge::initialize()
{
    std::cout << "Initializing EZF Advance III/card using the capture-proven probe path...\n";
    if (!startup() || !probePrefix(0,0,0,0,0,0) ||
        !tx92TwoAt(0x555,0xAA,0,"probe @555 AA00") ||
        !tx92TwoAt(0x2AA,0x55,0,"probe @2AA 5500") ||
        !tx92TwoAt(0x555,0x90,0,"probe @555 9000") ||
        !read91Sub2Four() ||
        !protocol_.tx92Two(0x90,0,"probe post-initial 90/00") ||
        !probeReset(true) ||
        !flashIdProbe(0,0,0,0,0,0) ||
        !flashIdProbe(2,0,0,0x40,0,0) ||
        !flashIdProbe(2,0,0,0x80,0,0) ||
        !flashIdProbe(2,0,0,0xC0,0,0) ||
        !flashIdProbe(0,0,0,0,2,0) ||
        !probePrefix(0,0,0,0,0,0,false)) return false;

    const std::vector<std::uint8_t> c95 = {
        0x5A,0xA5,0x95,0, 0x80,0,0,0, 0,0,0,0,0};
    std::vector<std::uint8_t> echo;
    if (!transport_.out(c95,timeout_ms) ||
        !transport_.inExact(echo,c95.size(),timeout_ms) || echo != c95)
        return false;
    std::vector<std::uint8_t> header;
    if (!read(0,header,0xAC)) return false;
    std::cout << "Read prime complete. First card bytes: ";
    printHex(header.data(),std::min<std::size_t>(4,header.size()));
    return true;
}

bool ReadOnlyCartridge::flashStatus()
{
    return protocol_.tx92Two(0xFF,0xFF,"status FFFF") &&
           protocol_.tx92One(1,4,"status 04") &&
           protocol_.tx92One(0,0,"status 00 A") &&
           protocol_.tx92One(0,0,"status 00 B");
}

bool ReadOnlyCartridge::finishReadMapping(const char* prefix)
{
    return flashStatus() &&
           protocol_.tx92Two(0x55,0xAA,std::string(prefix)+" 55AA") &&
           protocol_.tx92Two(0,0,std::string(prefix)+" 0000 A") &&
           protocol_.tx92Two(0,0,std::string(prefix)+" 0000 B") &&
           protocol_.tx92Two(0,0,std::string(prefix)+" 0000 C");
}

bool ReadOnlyCartridge::prepareLinear16MiB()
{
    std::cout << "Loader/ROM lies above 8 MiB; selecting proven 16-MiB linear read mapping...\n";
    if (!flashStatus() ||
        !protocol_.tx92Two(0x55,0xAA,"readmap 55AA") ||
        !protocol_.tx92Two(2,0,"readmap 0200") ||
        !protocol_.tx92Two(0,0x80,"readmap 0080") ||
        !protocol_.tx92Two(0,0,"readmap 0000 A")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return protocol_.tx92Two(0xAA,0x55,"readmap AA55") &&
           protocol_.tx92Two(0,0,"readmap 0000 B") &&
           protocol_.tx92Two(0,0,"readmap 0000 C") &&
           protocol_.tx92Two(0,0,"readmap 0000 D") &&
           protocol_.tx92One(0,0xAA,"readmap selector0 AA") &&
           protocol_.tx92One(0,0x55,"readmap selector0 55") &&
           protocol_.tx92One(1,6,"readmap selector1 06") &&
           finishReadMapping("read prefix");
}

bool ReadOnlyCartridge::prepareLinear24MiB()
{
    std::cout << "Loader/ROM lies in the 16..24-MiB range; selecting proven 24-MiB linear read mapping...\n";
    if (!flashStatus() ||
        !protocol_.tx92Two(0x55,0xAA,"readmap24 55AA") ||
        !protocol_.tx92Two(2,0,"readmap24 0200") ||
        !protocol_.tx92Two(0,0xC0,"readmap24 00C0") ||
        !protocol_.tx92Two(0,0,"readmap24 0000 A")) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(125000));
    return protocol_.tx92Two(0xAA,0x55,"readmap24 AA55") &&
           protocol_.tx92Two(0,0,"readmap24 0000 B") &&
           protocol_.tx92Two(0,0,"readmap24 0000 C") &&
           protocol_.tx92Two(0,0,"readmap24 0000 D") &&
           protocol_.tx92One(0,0xAA,"readmap24 selector0 AA") &&
           protocol_.tx92One(0,0x55,"readmap24 selector0 55") &&
           protocol_.tx92One(1,6,"readmap24 selector1 06") &&
           finishReadMapping("read24 prefix");
}

bool ReadOnlyCartridge::prepareLinear32MiB()
{
    std::cout << "Loader/ROM lies in the 24..32-MiB range; selecting proven 32-MiB linear read mapping...\n";
    if (!flashStatus() ||
        !protocol_.tx92Two(0x55,0xAA,"readmap32 55AA") ||
        !protocol_.tx92Two(0,0,"readmap32 0000 A") ||
        !protocol_.tx92Two(0,0,"readmap32 0000 B") ||
        !protocol_.tx92Two(2,0,"readmap32 0200")) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(125000));
    return protocol_.tx92Two(0xAA,0x55,"readmap32 AA55") &&
           protocol_.tx92Two(0,0,"readmap32 0000 C") &&
           protocol_.tx92Two(0,0,"readmap32 0000 D") &&
           protocol_.tx92Two(0,0,"readmap32 0000 E") &&
           protocol_.tx92One(0,0xAA,"readmap32 selector0 AA") &&
           protocol_.tx92One(0,0x55,"readmap32 selector0 55") &&
           protocol_.tx92One(1,6,"readmap32 selector1 06") &&
           finishReadMapping("read32 prefix");
}

} // namespace ezfadvance
