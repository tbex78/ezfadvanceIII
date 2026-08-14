#if defined(_WIN32)
#error "Native Windows builds are intentionally unsupported. Use macOS, Linux, BSD, or a Linux VM on Windows."
#endif

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// EZF Advance III card reader 0.6.2, read-only inspector.
// 0.6.2 removes hard-coded project-version text from runtime output.
// Card-read protocol behavior remains unchanged from 0.5.13.
//
// Ported from the historical card-reader v1 implementation. The USB/card
// read/probe behavior is intentionally unchanged. This program never sends
// erase (0x96) commands and never sends ROM-program payloads. Its 0x92 traffic
// is limited to the original EZ3Manager flash-probe / mapping sequences needed
// for capture-derived card inspection.
//
// Supported native targets:
//   macOS
//   Linux
//   FreeBSD
//   OpenBSD
//   NetBSD
//   DragonFly BSD
//
// Native Windows support is intentionally out of scope. Windows users should
// run the Linux build in a VM with direct USB passthrough for VID 0x0E6A /
// PID 0x5088.
//
// Recognized catalog layouts:
//   single ROM: loader-relative header 0x4E8, entry 0x4F8
//   multi ROM : loader-relative header 0x475E, entries 0x476E (28 bytes each)
//
// 0.5.13 improves blank-card diagnostics only. If the first four card bytes
// are FF FF FF FF, the reader now explicitly reports that the cartridge
// appears empty/erased and that no EZ3 loader/catalog is present at byte 0.
// USB probing, mapping, and read behavior are unchanged.
//
// 0.5.12 aligns the diagnostic reader with the writer's current catalog/layout
// knowledge and the newer 5-, 6-, 7-, and 8-ROM captures:
//   * 1..8 active catalog entries are capture-proven; the loader contains 120
//     structurally available 28-byte catalog slots, used here only as a safe
//     parser bound rather than a claim that 120-ROM menu operation is proven;
//   * full 32-MiB / 256-Mbit card geometry;
//   * exact 16-MiB, 24-MiB, and 32-MiB linear read mappings;
//   * the 32-MiB sequence is corrected to the exact 256-Mbit transition used by
//     the current writer and independently reconfirmed by the 6/7/8-ROM captures.
// The utility remains strictly read-only: no erase commands and no ROM
// programming are performed.

static constexpr uint16_t VID = 0x0E6A;
static constexpr uint16_t PID = 0x5088;
static constexpr unsigned char EP_OUT = 0x02;
static constexpr unsigned char EP_IN  = 0x81;
static constexpr int INTERFACE_NUMBER = 0;
static constexpr unsigned USB_TIMEOUT_MS = 15000;
static constexpr unsigned COMMAND_DATA_SETTLE_US = 750;

static constexpr const char* host_platform_name()
{
#if defined(__APPLE__) && defined(__MACH__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(__OpenBSD__)
    return "OpenBSD";
#elif defined(__NetBSD__)
    return "NetBSD";
#elif defined(__DragonFly__)
    return "DragonFly BSD";
#else
    return "Unix-like OS";
#endif
}

static constexpr uint32_t FLASH_WINDOW_SIZE = 0x00800000u; // 8 MiB
static constexpr uint32_t LINEAR16_LIMIT     = 0x01000000u; // 16 MiB
static constexpr uint32_t LINEAR24_LIMIT     = 0x01800000u; // 24 MiB / 192 Mbit
static constexpr uint32_t CARD_IMAGE_LIMIT   = 0x02000000u; // 32 MiB / 256 Mbit

static constexpr size_t SINGLE_HEADER_OFF = 0x4E8;
static constexpr size_t SINGLE_HEADER_COUNT2_OFF = 0x4F6;
static constexpr size_t SINGLE_ENTRY_OFF = 0x4F8;

static constexpr size_t MULTI_HEADER_OFF = 0x475E;
static constexpr size_t MULTI_ENTRY_OFF = 0x476E;
static constexpr size_t CATALOG_ENTRY_SIZE = 28;
static constexpr size_t MULTI_CATALOG_END_OFF = 0x548E;
static constexpr size_t MULTI_CATALOG_MAX_ENTRIES =
    (MULTI_CATALOG_END_OFF - MULTI_ENTRY_OFF) / CATALOG_ENTRY_SIZE;
static constexpr size_t CAPTURE_PROVEN_MAX_ROMS = 8;
static constexpr size_t LOADER_READ_SIZE = 0x7080;

static_assert(MULTI_ENTRY_OFF +
              MULTI_CATALOG_MAX_ENTRIES * CATALOG_ENTRY_SIZE ==
              MULTI_CATALOG_END_OFF,
              "multi-ROM catalog region must contain whole 28-byte entries");
static_assert(MULTI_CATALOG_MAX_ENTRIES == 120,
              "unexpected multi-ROM catalog structural slot count");

static uint16_t read_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static void write_le32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v.at(off + 0) = static_cast<uint8_t>(x >> 0);
    v.at(off + 1) = static_cast<uint8_t>(x >> 8);
    v.at(off + 2) = static_cast<uint8_t>(x >> 16);
    v.at(off + 3) = static_cast<uint8_t>(x >> 24);
}

static void print_hex(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (i && (i % 16) == 0) std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]) << ' ';
    }
    std::cout << std::dec << '\n';
}

static bool bulk_out(libusb_device_handle* h,
                     const uint8_t* p,
                     size_t n,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        h, EP_OUT, const_cast<unsigned char*>(p), static_cast<int>(n),
        &transferred, timeout_ms);

    if (rc != 0) {
        std::cerr << "BULK OUT failed: " << libusb_error_name(rc)
                  << " (" << rc << "), transferred=" << transferred << '\n';
        return false;
    }
    if (transferred != static_cast<int>(n)) {
        std::cerr << "BULK OUT short transfer: " << transferred
                  << "/" << n << '\n';
        return false;
    }
    return true;
}

static bool bulk_out(libusb_device_handle* h,
                     const std::vector<uint8_t>& data,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return bulk_out(h, data.data(), data.size(), timeout_ms);
}

static bool bulk_in_exact(libusb_device_handle* h,
                          std::vector<uint8_t>& data,
                          size_t wanted,
                          unsigned timeout_ms = USB_TIMEOUT_MS)
{
    data.assign(wanted, 0);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        h, EP_IN, data.data(), static_cast<int>(wanted),
        &transferred, timeout_ms);

    if (rc != 0) {
        std::cerr << "BULK IN failed: " << libusb_error_name(rc)
                  << " (" << rc << "), transferred=" << transferred << '\n';
        data.clear();
        return false;
    }

    data.resize(static_cast<size_t>(transferred));
    if (data.size() != wanted) {
        std::cerr << "BULK IN short transfer: " << data.size()
                  << "/" << wanted << '\n';
        return false;
    }
    return true;
}

static bool bulk_in_max(libusb_device_handle* h,
                        std::vector<uint8_t>& data,
                        size_t max_len,
                        unsigned timeout_ms = USB_TIMEOUT_MS)
{
    data.assign(max_len, 0);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        h, EP_IN, data.data(), static_cast<int>(max_len),
        &transferred, timeout_ms);

    if (rc != 0) {
        std::cerr << "BULK IN failed: " << libusb_error_name(rc)
                  << " (" << rc << "), transferred=" << transferred << '\n';
        data.clear();
        return false;
    }
    data.resize(static_cast<size_t>(transferred));
    return true;
}

static void legacy_command_data_settle()
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(COMMAND_DATA_SETTLE_US);
    while (std::chrono::steady_clock::now() < deadline) {
        // Intentional busy-wait: mirrors the original EZ3Manager transport timing.
    }
}

static std::vector<uint8_t> cmd92_2()
{
    return {0x5A,0xA5,0x92,0x02,0x00,0x00,0x00,0x00,
            0x02,0x00,0x00,0x00,0x00};
}

static std::vector<uint8_t> cmd92_1(uint8_t selector)
{
    return {0x5A,0xA5,0x92,0x01,selector,0x00,0x00,0x00,
            0x01,0x00,0x00,0x00,0x00};
}

static bool command_data_echo(libusb_device_handle* h,
                              const std::vector<uint8_t>& command,
                              const std::vector<uint8_t>& data,
                              const std::string& label)
{
    if (!bulk_out(h, command)) {
        std::cerr << label << ": command OUT failed\n";
        return false;
    }

    legacy_command_data_settle();

    if (!bulk_out(h, data)) {
        std::cerr << label << ": data OUT failed\n";
        return false;
    }

    std::vector<uint8_t> response;
    if (!bulk_in_max(h, response, 64)) {
        std::cerr << label << ": echo IN failed\n";
        return false;
    }
    if (response != command) {
        std::cerr << label << ": command echo mismatch\n";
        return false;
    }
    return true;
}

static bool tx92_2(libusb_device_handle* h,
                   uint8_t a, uint8_t b,
                   const std::string& label)
{
    return command_data_echo(h, cmd92_2(), {a,b}, label);
}

static bool tx92_1(libusb_device_handle* h,
                   uint8_t selector, uint8_t value,
                   const std::string& label)
{
    return command_data_echo(h, cmd92_1(selector), {value}, label);
}

static bool manager_tx92_2_at(libusb_device_handle* h,
                              uint32_t word_address,
                              uint8_t a, uint8_t b,
                              const std::string& label)
{
    std::vector<uint8_t> command = cmd92_2();
    write_le32(command, 4, word_address);
    return command_data_echo(h, command, {a,b}, label);
}

static bool read91_sub2_4(libusb_device_handle* h,
                          std::array<uint8_t,4>& response)
{
    const std::vector<uint8_t> command = {
        0x5A,0xA5,0x91,0x02, 0,0,0,0, 0x04,0,0,0,0
    };
    if (!bulk_out(h, command)) return false;
    std::vector<uint8_t> in;
    if (!bulk_in_exact(h, in, 4)) return false;
    std::copy(in.begin(), in.end(), response.begin());
    return true;
}

static std::vector<uint8_t> make_read_command(uint32_t byte_address,
                                              uint32_t length)
{
    if (byte_address & 1u)
        throw std::runtime_error("read address must be word-aligned");

    const uint32_t word_address = byte_address / 2u;
    std::vector<uint8_t> c = {
        0x5A,0xA5,0x91,0x00, 0,0,0,0, 0,0,0,0, 0x00
    };
    write_le32(c, 4, word_address);
    write_le32(c, 8, length);
    return c;
}

static bool read_card(libusb_device_handle* h,
                      uint32_t byte_address,
                      uint8_t* destination,
                      size_t length)
{
    size_t done = 0;
    while (done < length) {
        const size_t piece = std::min<size_t>(0x10000, length - done);
        const uint32_t address = byte_address + static_cast<uint32_t>(done);
        const auto cmd = make_read_command(address, static_cast<uint32_t>(piece));
        if (!bulk_out(h, cmd)) return false;
        std::vector<uint8_t> in;
        if (!bulk_in_exact(h, in, piece)) return false;
        std::copy(in.begin(), in.end(), destination + done);
        done += piece;
    }
    return true;
}

static bool read_card(libusb_device_handle* h,
                      uint32_t byte_address,
                      std::vector<uint8_t>& out,
                      size_t length)
{
    out.assign(length, 0);
    return read_card(h, byte_address, out.data(), out.size());
}

static bool original_bridge_startup_979899(libusb_device_handle* h)
{
    const std::vector<uint8_t> c97 = {
        0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0
    };
    const std::vector<uint8_t> c98 = {
        0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0
    };
    const std::vector<uint8_t> c99 = {
        0x5A,0xA5,0x99,0,0x01,0,0,0,0,0,0,0,0
    };

    std::vector<uint8_t> response;
    if (!bulk_out(h, c97) || !bulk_in_exact(h, response, 1) || response[0] != 0x00)
        return false;
    if (!bulk_out(h, c98) || !bulk_in_exact(h, response, 1) || response[0] != 0x01)
        return false;
    if (!bulk_out(h, c99) || !bulk_in_exact(h, response, c99.size()) || response != c99)
        return false;
    return true;
}

static bool probe_unlock_tail(libusb_device_handle* h)
{
    return tx92_2(h,0xAA,0x55,"probe AA55") &&
           tx92_2(h,0x00,0x00,"probe zero 1") &&
           tx92_2(h,0x00,0x00,"probe zero 2") &&
           tx92_2(h,0x00,0x00,"probe zero 3") &&
           tx92_1(h,0x00,0xAA,"probe selector0 AA") &&
           tx92_1(h,0x00,0x55,"probe selector0 55") &&
           tx92_1(h,0x01,0x06,"probe selector1 06");
}

static bool probe_prefix(libusb_device_handle* h,
                         uint8_t a0, uint8_t a1,
                         uint8_t b0, uint8_t b1,
                         uint8_t c0, uint8_t c1,
                         bool include_tail = true)
{
    if (!tx92_2(h,0x55,0xAA,"probe 55AA")) return false;
    if (!tx92_2(h,a0,a1,"probe word 1")) return false;
    if (!tx92_2(h,b0,b1,"probe word 2")) return false;
    if (!tx92_2(h,c0,c1,"probe word 3")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    return !include_tail || probe_unlock_tail(h);
}

static bool probe_reset_after_id(libusb_device_handle* h, bool use_f0)
{
    return tx92_2(h, use_f0 ? 0xF0 : 0xFF,
                  use_f0 ? 0x00 : 0xFF, "probe ID reset") &&
           tx92_1(h,0x01,0x04,"probe status 1/04") &&
           tx92_1(h,0x00,0x00,"probe status 0/00 #1") &&
           tx92_1(h,0x00,0x00,"probe status 0/00 #2");
}

static bool flash_id_probe(libusb_device_handle* h,
                           uint8_t a0, uint8_t a1,
                           uint8_t b0, uint8_t b1,
                           uint8_t c0, uint8_t c1)
{
    if (!probe_prefix(h,a0,a1,b0,b1,c0,c1)) return false;
    if (!tx92_2(h,0x90,0x00,"probe 90/00")) return false;
    std::array<uint8_t,4> id{};
    if (!read91_sub2_4(h,id)) return false;
    return probe_reset_after_id(h,false);
}

static bool original_manager_read_prime(libusb_device_handle* h)
{
    std::cout << "Initializing EZF Advance III/card using the capture-proven probe path...\n";

    if (!original_bridge_startup_979899(h)) return false;
    if (!probe_prefix(h,0,0,0,0,0,0)) return false;

    if (!manager_tx92_2_at(h,0x555,0xAA,0x00,"probe @555 AA00")) return false;
    if (!manager_tx92_2_at(h,0x2AA,0x55,0x00,"probe @2AA 5500")) return false;
    if (!manager_tx92_2_at(h,0x555,0x90,0x00,"probe @555 9000")) return false;

    std::array<uint8_t,4> first_probe{};
    if (!read91_sub2_4(h,first_probe)) return false;
    if (!tx92_2(h,0x90,0x00,"probe post-initial 90/00")) return false;
    if (!probe_reset_after_id(h,true)) return false;

    if (!flash_id_probe(h,0,0,0,0,0,0)) return false;
    if (!flash_id_probe(h,0x02,0x00,0x00,0x40,0x00,0x00)) return false;
    if (!flash_id_probe(h,0x02,0x00,0x00,0x80,0x00,0x00)) return false;
    if (!flash_id_probe(h,0x02,0x00,0x00,0xC0,0x00,0x00)) return false;
    if (!flash_id_probe(h,0,0,0,0,0x02,0x00)) return false;
    if (!probe_prefix(h,0,0,0,0,0,0,false)) return false;

    const std::vector<uint8_t> c95 = {
        0x5A,0xA5,0x95,0x00, 0x80,0x00,0x00,0x00, 0,0,0,0,0
    };
    std::vector<uint8_t> echo;
    if (!bulk_out(h,c95) || !bulk_in_exact(h,echo,c95.size()) || echo != c95)
        return false;

    std::vector<uint8_t> header;
    if (!read_card(h,0,header,0xAC)) return false;

    std::cout << "Read prime complete. First card bytes: ";
    print_hex(header.data(), std::min<size_t>(4,header.size()));
    return true;
}

static bool flash_status_sequence(libusb_device_handle* h)
{
    return tx92_2(h,0xFF,0xFF,"status FFFF") &&
           tx92_1(h,0x01,0x04,"status 04") &&
           tx92_1(h,0x00,0x00,"status 00 A") &&
           tx92_1(h,0x00,0x00,"status 00 B");
}

// Capture-derived 16-MiB linear read mapping.  This only changes
// flash mapping/status state; it does not erase or program card contents.
static bool prepare_linear_16m_read(libusb_device_handle* h)
{
    std::cout << "Loader/ROM lies above 8 MiB; selecting proven 16-MiB linear read mapping...\n";

    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"readmap 55AA")) return false;
    if (!tx92_2(h,0x02,0x00,"readmap 0200")) return false;
    if (!tx92_2(h,0x00,0x80,"readmap 0080")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap 0000 A")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    if (!tx92_2(h,0xAA,0x55,"readmap AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap 0000 D")) return false;
    if (!tx92_1(h,0x00,0xAA,"readmap selector0 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"readmap selector0 55")) return false;
    if (!tx92_1(h,0x01,0x06,"readmap selector1 06")) return false;
    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"read prefix 55AA")) return false;
    if (!tx92_2(h,0x00,0x00,"read prefix 0000 A")) return false;
    if (!tx92_2(h,0x00,0x00,"read prefix 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"read prefix 0000 C")) return false;
    return true;
}


// Exact 24-MiB / 192-Mbit linear read mapping from 4_4_4_4_8MB.pcap.
// This is the same transition used by the writer for exact-24-MiB readback:
// status, 55AA,0200,00C0,0000,125 ms, AA55,0000x3,AA,55,06,
// status, then 55AA,0000,0000,0000.
static bool prepare_linear_24m_read(libusb_device_handle* h)
{
    std::cout << "Loader/ROM lies in the 16..24-MiB range; selecting proven "
                 "24-MiB linear read mapping...\n";

    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"readmap24 55AA")) return false;
    if (!tx92_2(h,0x02,0x00,"readmap24 0200")) return false;
    if (!tx92_2(h,0x00,0xC0,"readmap24 00C0")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap24 0000 A")) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(125000));
    if (!tx92_2(h,0xAA,0x55,"readmap24 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap24 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap24 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap24 0000 D")) return false;
    if (!tx92_1(h,0x00,0xAA,"readmap24 selector0 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"readmap24 selector0 55")) return false;
    if (!tx92_1(h,0x01,0x06,"readmap24 selector1 06")) return false;
    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"read24 prefix 55AA")) return false;
    if (!tx92_2(h,0x00,0x00,"read24 prefix 0000 A")) return false;
    if (!tx92_2(h,0x00,0x00,"read24 prefix 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"read24 prefix 0000 C")) return false;
    return true;
}


// Capture-derived 32-MiB / 256-Mbit linear read mapping. This is the exact
// full-card transition from 256MBits-rom.pcap, also reconfirmed by the
// 6-, 7-, and 8-ROM full-card captures.
static bool prepare_linear_32m_read(libusb_device_handle* h)
{
    std::cout << "Loader/ROM lies in the 24..32-MiB range; selecting proven "
                 "32-MiB linear read mapping...\n";

    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"readmap32 55AA")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap32 0000 A")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap32 0000 B")) return false;
    if (!tx92_2(h,0x02,0x00,"readmap32 0200")) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(125000));
    if (!tx92_2(h,0xAA,0x55,"readmap32 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap32 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap32 0000 D")) return false;
    if (!tx92_2(h,0x00,0x00,"readmap32 0000 E")) return false;
    if (!tx92_1(h,0x00,0xAA,"readmap32 selector0 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"readmap32 selector0 55")) return false;
    if (!tx92_1(h,0x01,0x06,"readmap32 selector1 06")) return false;
    if (!flash_status_sequence(h)) return false;
    if (!tx92_2(h,0x55,0xAA,"read32 prefix 55AA")) return false;
    if (!tx92_2(h,0x00,0x00,"read32 prefix 0000 A")) return false;
    if (!tx92_2(h,0x00,0x00,"read32 prefix 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"read32 prefix 0000 C")) return false;
    return true;
}

static std::optional<uint32_t> arm_branch_target(uint32_t ins)
{
    if ((ins & 0xFF000000u) != 0xEA000000u)
        return std::nullopt;

    int32_t imm = static_cast<int32_t>(ins & 0x00FFFFFFu);
    if (imm & 0x00800000)
        imm |= static_cast<int32_t>(0xFF000000u);

    const int64_t target = 8ll + static_cast<int64_t>(imm) * 4ll;
    if (target < 0 || target > 0xFFFFFFFFll)
        return std::nullopt;
    return static_cast<uint32_t>(target);
}

static std::string clean_ascii(const uint8_t* p, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = p[i];
        if (b == 0x00 || b == 0xFF) break;
        out.push_back((b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '.');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static bool gba_header_checksum_ok(const std::vector<uint8_t>& h)
{
    if (h.size() < 0xC0) return false;
    uint8_t x = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i)
        x = static_cast<uint8_t>(x - h[i]);
    x = static_cast<uint8_t>(x - 0x19);
    return x == h[0xBD];
}

struct GbaHeader {
    bool readable = false;
    bool checksum_ok = false;
    std::string title;
    std::string game_code;
    std::string maker_code;
    uint8_t version = 0;
};

static GbaHeader read_gba_header(libusb_device_handle* h, uint32_t start)
{
    GbaHeader g;
    std::vector<uint8_t> b;
    if (!read_card(h,start,b,0xC0)) return g;
    g.readable = true;
    g.title = clean_ascii(b.data()+0xA0,12);
    g.game_code = clean_ascii(b.data()+0xAC,4);
    g.maker_code = clean_ascii(b.data()+0xB0,2);
    g.version = b[0xBC];
    g.checksum_ok = gba_header_checksum_ok(b);
    return g;
}

struct CatalogEntry {
    std::string name;
    uint8_t type = 0;
    uint8_t mapping = 0;
    uint32_t packed_start = 0;
    uint32_t start = 0;
    uint32_t target_or_start = 0;
};

enum class ReadMappingMode {
    Lower8MiB,
    Linear16MiB,
    Linear24MiB,
    Linear32MiB
};

static const char* mapping_mode_name(ReadMappingMode mode)
{
    switch (mode) {
        case ReadMappingMode::Lower8MiB:   return "lower 8-MiB";
        case ReadMappingMode::Linear16MiB: return "16-MiB linear";
        case ReadMappingMode::Linear24MiB: return "24-MiB linear";
        case ReadMappingMode::Linear32MiB: return "32-MiB linear";
    }
    return "unknown";
}

static CatalogEntry parse_catalog_entry(const std::vector<uint8_t>& loader,
                                        size_t off,
                                        bool first)
{
    if (off + CATALOG_ENTRY_SIZE > loader.size())
        throw std::runtime_error("catalog entry outside read loader data");

    CatalogEntry e;
    e.name = clean_ascii(loader.data()+off,16);
    const uint32_t type_field = read_le32(loader.data()+off+16);
    e.type = static_cast<uint8_t>(type_field >> 24);
    e.packed_start = read_le32(loader.data()+off+20);
    e.mapping = static_cast<uint8_t>(e.packed_start & 0xFFu);
    e.start = first ? 0u : static_cast<uint32_t>((e.packed_start >> 8) * 2u);
    e.target_or_start = read_le32(loader.data()+off+24);
    return e;
}

static bool plausible_entry(const CatalogEntry& e, bool first)
{
    if (e.name.empty()) return false;
    if (e.start >= CARD_IMAGE_LIMIT) return false;
    if (!first && (e.start & 0xFFFFu) != 0) return false;
    return true;
}

static std::string hex32(uint32_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

static void print_rom(size_t index,
                      const CatalogEntry& e,
                      const GbaHeader& g,
                      std::optional<uint32_t> allocated_end)
{
    std::cout << "\nROM " << index << "\n"
              << "  Catalog name : " << e.name << "\n"
              << "  Start        : " << hex32(e.start) << "\n";

    if (allocated_end && *allocated_end >= e.start) {
        const uint32_t span = *allocated_end - e.start;
        std::cout << "  Alloc. span  : " << span << " bytes (0x"
                  << std::hex << span << std::dec << ")\n";
    }

    std::cout << "  Catalog type : " << static_cast<unsigned>(e.type) << " (size class)\n"
              << "  Catalog map  : " << static_cast<unsigned>(e.mapping) << "\n";

    if (index == 1)
        std::cout << "  Orig. entry  : " << hex32(e.target_or_start) << "\n";
    else
        std::cout << "  Stored start : " << hex32(e.target_or_start) << "\n";

    if (g.readable) {
        std::cout << "  GBA title    : " << (g.title.empty() ? "(blank)" : g.title) << "\n"
                  << "  Game code    : " << (g.game_code.empty() ? "(blank)" : g.game_code) << "\n"
                  << "  Maker code   : " << (g.maker_code.empty() ? "(blank)" : g.maker_code) << "\n"
                  << "  ROM version  : " << static_cast<unsigned>(g.version) << "\n"
                  << "  Header check : " << (g.checksum_ok ? "OK" : "FAILED") << "\n";
    }
}

static int inspect_card(libusb_device_handle* h)
{
    std::vector<uint8_t> first;
    if (!read_card(h,0,first,4)) return 2;

    const uint32_t first_word = read_le32(first.data());
    const auto loader_target = arm_branch_target(first_word);
    if (!loader_target) {
        const bool first_word_erased =
            std::all_of(first.begin(), first.end(),
                        [](uint8_t b) { return b == 0xFF; });

        if (first_word_erased) {
            std::cout
                << "Card appears empty / erased.\n"
                << "The first four card bytes are FF FF FF FF, which is the "
                   "erased flash state.\n"
                << "No EZ3 loader branch or ROM catalog is present at card "
                   "byte 0.\n"
                << "There is no programmed EZ3 ROM image to inspect.\n";
        } else {
            std::cerr
                << "Card byte 0 is not an ARM unconditional branch.\n"
                << "The card is not blank at byte 0, but it does not look like "
                   "a recognized capture-derived EZ3 single/multi-ROM image.\n";
        }
        return 3;
    }

    const uint32_t loader_start = *loader_target;
    if (loader_start < 0xC0 || loader_start >= CARD_IMAGE_LIMIT) {
        std::cerr << "Patched branch points to " << hex32(loader_start)
                  << ", outside the currently understood 32-MiB / 256-Mbit layout.\n";
        return 3;
    }

    ReadMappingMode mapping_mode = ReadMappingMode::Lower8MiB;
    if (loader_start >= LINEAR24_LIMIT) {
        if (!prepare_linear_32m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear32MiB;
    } else if (loader_start >= LINEAR16_LIMIT) {
        if (!prepare_linear_24m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear24MiB;
    } else if (loader_start >= FLASH_WINDOW_SIZE) {
        if (!prepare_linear_16m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear16MiB;
    }

    const size_t loader_read_len = std::min<size_t>(
        LOADER_READ_SIZE, static_cast<size_t>(CARD_IMAGE_LIMIT - loader_start));
    std::vector<uint8_t> loader;
    if (!read_card(h,loader_start,loader,loader_read_len)) return 2;

    bool is_single = false;
    size_t rom_count = 0;
    size_t entry_off = 0;

    if (loader.size() >= SINGLE_ENTRY_OFF + CATALOG_ENTRY_SIZE) {
        const uint16_t a = read_le16(loader.data()+SINGLE_HEADER_OFF);
        const uint16_t b = read_le16(loader.data()+SINGLE_HEADER_COUNT2_OFF);
        if (a == 1 && b == 1) {
            const CatalogEntry e = parse_catalog_entry(loader,SINGLE_ENTRY_OFF,true);
            if (plausible_entry(e,true)) {
                is_single = true;
                rom_count = 1;
                entry_off = SINGLE_ENTRY_OFF;
            }
        }
    }

    if (loader.size() >= MULTI_HEADER_OFF + 16) {
        const uint16_t a = read_le16(loader.data()+MULTI_HEADER_OFF);
        const uint16_t b = read_le16(loader.data()+MULTI_HEADER_OFF+14);
        if (a >= 2 && a <= MULTI_CATALOG_MAX_ENTRIES && a == b) {
            const size_t required =
                MULTI_ENTRY_OFF + static_cast<size_t>(a) * CATALOG_ENTRY_SIZE;
            if (required <= loader.size()) {
                bool all_ok = true;
                for (size_t i=0;i<a;++i) {
                    const CatalogEntry e = parse_catalog_entry(
                        loader,MULTI_ENTRY_OFF+i*CATALOG_ENTRY_SIZE,i==0);
                    if (!plausible_entry(e,i==0)) all_ok = false;
                }
                if (all_ok) {
                    is_single = false;
                    rom_count = a;
                    entry_off = MULTI_ENTRY_OFF;
                }
            }
        }
    }

    if (rom_count == 0) {
        std::cerr << "Found loader branch at " << hex32(loader_start)
                  << " but no recognized capture-derived single/multi-ROM catalog.\n";
        return 3;
    }

    std::vector<CatalogEntry> entries;
    for (size_t i=0;i<rom_count;++i)
        entries.push_back(parse_catalog_entry(
            loader,entry_off+i*CATALOG_ENTRY_SIZE,i==0));

    // If any cataloged ROM header lies beyond the currently selected read
    // window, upgrade to the smallest capture-proven linear mapping that covers
    // that address. The 24-MiB mapping is now independently capture-proven.
    uint32_t highest_start = 0;
    for (const auto& e : entries)
        highest_start = std::max(highest_start, e.start);

    if (highest_start >= LINEAR24_LIMIT &&
        mapping_mode != ReadMappingMode::Linear32MiB) {
        if (!prepare_linear_32m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear32MiB;
    } else if (highest_start >= LINEAR16_LIMIT &&
               mapping_mode != ReadMappingMode::Linear24MiB &&
               mapping_mode != ReadMappingMode::Linear32MiB) {
        if (!prepare_linear_24m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear24MiB;
    } else if (highest_start >= FLASH_WINDOW_SIZE &&
               mapping_mode == ReadMappingMode::Lower8MiB) {
        if (!prepare_linear_16m_read(h)) return 2;
        mapping_mode = ReadMappingMode::Linear16MiB;
    }

    std::cout << "\n========================================\n"
              << "EZF ADVANCE III CARD CONTENTS\n"
              << "========================================\n"
              << "Layout       : " << (is_single ? "single ROM" : "multi ROM") << "\n"
              << "ROM count    : " << rom_count << "\n"
              << "Loader/menu  : " << hex32(loader_start) << "\n"
              << "Read mapping : " << mapping_mode_name(mapping_mode) << "\n";

    if (!is_single) {
        if (rom_count <= CAPTURE_PROVEN_MAX_ROMS) {
            std::cout << "Catalog proof : count " << rom_count
                      << " is within the capture-proven 2.."
                      << CAPTURE_PROVEN_MAX_ROMS << " active-entry range\n";
        } else {
            std::cout << "Catalog proof : count " << rom_count
                      << " is structurally parseable but exceeds the currently "
                         "capture-proven 8-entry range\n";
        }
    }

    for (size_t i=0;i<entries.size();++i) {
        std::optional<uint32_t> span_end;
        if (!is_single)
            span_end = (i+1 < entries.size()) ? entries[i+1].start : loader_start;
        const GbaHeader g = read_gba_header(h,entries[i].start);
        print_rom(i+1,entries[i],g,span_end);
    }

    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

static void usage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << "\n\n"
              << "Read-only EZF Advance III card inspector (" << host_platform_name() << ").\n"
              << "Connect the EZF Advance III with the GBA card inserted, then run with no arguments.\n";
}

int main(int argc, char** argv)
{
    if (argc != 1) {
        usage(argv[0]);
        return 1;
    }

    libusb_context* ctx = nullptr;
    libusb_device_handle* h = nullptr;

    int rc = libusb_init(&ctx);
    if (rc != 0) {
        std::cerr << "libusb_init failed: " << libusb_error_name(rc) << '\n';
        return 1;
    }

    h = libusb_open_device_with_vid_pid(ctx,VID,PID);
    if (!h) {
        std::cerr << "EZF Advance III USB device VID=0x0E6A PID=0x5088 not found.\n";
        libusb_exit(ctx);
        return 1;
    }

#if defined(__linux__)
    // Linux may bind a kernel driver to the interface. Ask libusb to
    // detach/reattach it automatically. macOS/BSD do not use this path.
    const int detach_rc = libusb_set_auto_detach_kernel_driver(h,1);
    if (detach_rc != 0) {
        std::cerr << "Warning: libusb auto-detach setup failed: "
                  << libusb_error_name(detach_rc)
                  << " (continuing to interface claim)\n";
    }
#endif
    rc = libusb_claim_interface(h,INTERFACE_NUMBER);
    if (rc != 0) {
        std::cerr << "Could not claim interface 0: " << libusb_error_name(rc) << '\n';
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    std::cout << "EZF Advance III opened on " << host_platform_name()
              << "; interface 0 claimed.\n";

    int result = 2;
    if (original_manager_read_prime(h))
        result = inspect_card(h);
    else
        std::cerr << "Card initialization/read-prime failed.\n";

    libusb_release_interface(h,INTERFACE_NUMBER);
    libusb_close(h);
    libusb_exit(ctx);
    return result;
}
