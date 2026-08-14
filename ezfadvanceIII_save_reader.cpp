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
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// EZF Advance III save reader 0.6.2, read-only dumper.
// 0.6.2 removes hard-coded project-version text from runtime output.
// Save-read protocol behavior remains unchanged from 0.5.10.
//
// Ported from the historical save-reader v2 implementation. The USB/save
// protocol and conservative safety behavior are intentionally unchanged.
// This utility never sends erase (0x96) commands and never sends ROM-program
// payloads. Its 0x92 traffic is limited to the capture-derived EZ3Manager
// probe/mapping command sequences needed to read the cartridge.
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
// Current catalog support is deliberately conservative: 1..3 ROMs in the
// first 16 MiB, using only the capture-proven linear-read mapping.
//
// Save protocol evidence:
//   readsav.pcap: 0x0900 -> 32 KiB, then 0x0910 -> 32 KiB.
//   writesav.pcap: writes exactly the first 32 KiB payload from readsav.pcap.
//   readmultiromonesav.pcap: Piano + Bios_Dumper card; Bios_Dumper save export
//     performs only 0x0900 -> 32 KiB and produces a 32-KiB .sav file.
// Therefore this reader treats SRAM_V111 / Bios_Dumper as a capture-proven
// 32-KiB save and refuses ambiguous multi-save configurations.

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
static constexpr uint32_t CAPTURE_LINEAR_LIMIT  = 0x01000000u; // 16 MiB

static constexpr size_t SINGLE_HEADER_OFF = 0x4E8;
static constexpr size_t SINGLE_HEADER_COUNT2_OFF = 0x4F6;
static constexpr size_t SINGLE_ENTRY_OFF = 0x4F8;

static constexpr size_t MULTI_HEADER_OFF = 0x475E;
static constexpr size_t MULTI_ENTRY_OFF = 0x476E;
static constexpr size_t CATALOG_ENTRY_SIZE = 28;
static constexpr size_t MAX_CAPTURED_ROMS = 3;
static constexpr size_t LOADER_READ_SIZE = 0x7080;

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
    if (e.start >= CAPTURE_LINEAR_LIMIT) return false;
    if (!first && (e.start & 0xFFFFu) != 0) return false;
    return true;
}

static std::string hex32(uint32_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

static bool select_save_bank_capture(libusb_device_handle* h,
                                     uint16_t bank_value)
{
    // readsav.pcap / writesav.pcap use the same eight 0x92/02 register writes
    // before accessing one 32-KiB save window.  The bank selector is the fourth
    // two-byte value: 0x0900 for save bytes 0x0000..0x7FFF, 0x0910 for
    // 0x8000..0xFFFF.
    const uint8_t lo = static_cast<uint8_t>(bank_value & 0xFFu);
    const uint8_t hi = static_cast<uint8_t>(bank_value >> 8);

    return tx92_2(h,0x55,0xAA,"savebank 55AA") &&
           tx92_2(h,0x00,0x00,"savebank 0000 A") &&
           tx92_2(h,0x00,0x00,"savebank 0000 B") &&
           tx92_2(h,lo,hi,"savebank selector") &&
           tx92_2(h,0x00,0x00,"savebank 0000 C") &&
           tx92_2(h,0x00,0x00,"savebank 0000 D") &&
           tx92_2(h,0x00,0x00,"savebank 0000 E") &&
           tx92_2(h,0x00,0x00,"savebank 0000 F");
}

static bool read_save_bank_32k(libusb_device_handle* h,
                               uint16_t bank_value,
                               std::vector<uint8_t>& out)
{
    if (!select_save_bank_capture(h,bank_value))
        return false;

    // Exact readsav.pcap save-read command:
    //   5A A5 91 01  00 00 00 00  00 80 00 00  00
    // It returns exactly 0x8000 bytes on EP 0x81.
    const std::vector<uint8_t> command = {
        0x5A,0xA5,0x91,0x01,
        0x00,0x00,0x00,0x00,
        0x00,0x80,0x00,0x00,
        0x00
    };

    if (!bulk_out(h,command)) {
        std::cerr << "save read command OUT failed\n";
        return false;
    }
    return bulk_in_exact(h,out,0x8000);
}

static bool read_save_capture(libusb_device_handle* h,
                              size_t save_size,
                              std::vector<uint8_t>& save)
{
    if (save_size != 0x8000 && save_size != 0x10000)
        throw std::runtime_error("capture reader supports only 32-KiB or 64-KiB reads");

    std::vector<uint8_t> bank0;
    std::cout << "Reading save bank 1"
              << (save_size == 0x10000 ? "/2" : "")
              << " (selector 0x0900, 32 KiB)...\n";
    if (!read_save_bank_32k(h,0x0900,bank0)) return false;

    save = bank0;
    if (save_size == 0x10000) {
        std::vector<uint8_t> bank1;
        std::cout << "Reading save bank 2/2 (selector 0x0910, 32 KiB)...\n";
        if (!read_save_bank_32k(h,0x0910,bank1)) return false;
        save.insert(save.end(),bank1.begin(),bank1.end());
    }

    return save.size() == save_size;
}

static bool contains_pattern(const std::vector<uint8_t>& data,
                             const std::string& pattern)
{
    if (pattern.empty() || data.size() < pattern.size()) return false;
    return std::search(data.begin(),data.end(),
                       pattern.begin(),pattern.end()) != data.end();
}

struct SaveSignature {
    std::string text;
    bool capture_proven_32k = false;
};

static SaveSignature detect_save_signature(libusb_device_handle* h,
                                           uint32_t start,
                                           uint32_t span)
{
    // Scan the ROM allocation in 64-KiB chunks.  Keep a small overlap so a
    // marker split at a chunk boundary is still found.
    static const std::vector<std::string> patterns = {
        "SRAM_V111",
        "SRAM_V112",
        "SRAM_V",
        std::string("SRAM\0",5),
        "FLASH1M",
        "FLASH512",
        "FLASH_V",
        "EEPROM_V"
    };

    constexpr size_t CHUNK = 0x10000;
    constexpr size_t OVERLAP = 32;
    std::vector<uint8_t> carry;

    uint32_t pos = 0;
    while (pos < span) {
        const size_t n = std::min<size_t>(CHUNK,static_cast<size_t>(span-pos));
        std::vector<uint8_t> b;
        if (!read_card(h,start+pos,b,n))
            return {};

        std::vector<uint8_t> joined;
        joined.reserve(carry.size()+b.size());
        joined.insert(joined.end(),carry.begin(),carry.end());
        joined.insert(joined.end(),b.begin(),b.end());

        for (const auto& pat : patterns) {
            if (contains_pattern(joined,pat)) {
                SaveSignature s;
                s.text = (pat.size() == 5 && pat[4] == '\0') ? "SRAM" : pat;
                // readmultiromonesav.pcap + writesav.pcap prove the exact
                // SRAM_V111 / Bios_Dumper save file is 0x8000 bytes.
                s.capture_proven_32k = (pat == "SRAM_V111");
                return s;
            }
        }

        const size_t keep = std::min(OVERLAP,joined.size());
        carry.assign(joined.end()-static_cast<std::ptrdiff_t>(keep),
                     joined.end());
        pos += static_cast<uint32_t>(n);
    }
    return {};
}

static std::string safe_filename_component(std::string s)
{
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_' || c == '.'))
            c = '_';
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "gba_save" : s;
}

static int inspect_and_dump_save(libusb_device_handle* h,
                                 const std::optional<std::string>& requested_output,
                                 const std::optional<size_t>& requested_rom)
{
    std::vector<uint8_t> first;
    if (!read_card(h,0,first,4)) return 2;

    const uint32_t first_word = read_le32(first.data());
    const auto loader_target = arm_branch_target(first_word);
    if (!loader_target) {
        std::cerr << "Card byte 0 is not an ARM unconditional branch.\n"
                  << "This does not look like a recognized capture-derived EZ3 image.\n";
        return 3;
    }

    const uint32_t loader_start = *loader_target;
    if (loader_start < 0xC0 || loader_start >= CAPTURE_LINEAR_LIMIT) {
        std::cerr << "Patched branch points to " << hex32(loader_start)
                  << ", outside the currently understood first-16-MiB layout.\n";
        return 3;
    }

    if (loader_start >= FLASH_WINDOW_SIZE) {
        if (!prepare_linear_16m_read(h)) return 2;
    }

    const size_t loader_read_len = std::min<size_t>(
        LOADER_READ_SIZE, static_cast<size_t>(CAPTURE_LINEAR_LIMIT-loader_start));
    std::vector<uint8_t> loader;
    if (!read_card(h,loader_start,loader,loader_read_len)) return 2;

    bool is_single = false;
    size_t rom_count = 0;
    size_t entry_off = 0;

    if (loader.size() >= SINGLE_ENTRY_OFF+CATALOG_ENTRY_SIZE) {
        const uint16_t a = read_le16(loader.data()+SINGLE_HEADER_OFF);
        const uint16_t b = read_le16(loader.data()+SINGLE_HEADER_COUNT2_OFF);
        if (a == 1 && b == 1) {
            const CatalogEntry e =
                parse_catalog_entry(loader,SINGLE_ENTRY_OFF,true);
            if (plausible_entry(e,true)) {
                is_single = true;
                rom_count = 1;
                entry_off = SINGLE_ENTRY_OFF;
            }
        }
    }

    if (!is_single && loader.size() >=
        MULTI_ENTRY_OFF+MAX_CAPTURED_ROMS*CATALOG_ENTRY_SIZE) {
        const uint16_t a = read_le16(loader.data()+MULTI_HEADER_OFF);
        const uint16_t b = read_le16(loader.data()+MULTI_HEADER_OFF+14);
        if (a >= 2 && a <= MAX_CAPTURED_ROMS && a == b) {
            bool all_ok = true;
            for (size_t i=0;i<a;++i) {
                const CatalogEntry e = parse_catalog_entry(
                    loader,MULTI_ENTRY_OFF+i*CATALOG_ENTRY_SIZE,i==0);
                if (!plausible_entry(e,i==0)) all_ok = false;
            }
            if (all_ok) {
                rom_count = a;
                entry_off = MULTI_ENTRY_OFF;
            }
        }
    }

    if (rom_count == 0) {
        std::cerr << "Found loader branch at " << hex32(loader_start)
                  << " but no recognized EZ3 catalog.\n";
        return 3;
    }

    struct DetectedRom {
        CatalogEntry e;
        GbaHeader g;
        uint32_t span = 0;
        SaveSignature sig;
    };
    std::vector<DetectedRom> roms;
    roms.reserve(rom_count);

    for (size_t i=0;i<rom_count;++i) {
        DetectedRom r;
        r.e = parse_catalog_entry(loader,entry_off+i*CATALOG_ENTRY_SIZE,i==0);
        r.g = read_gba_header(h,r.e.start);
        const uint32_t end = (i+1 < rom_count)
            ? parse_catalog_entry(loader,entry_off+(i+1)*CATALOG_ENTRY_SIZE,false).start
            : loader_start;
        r.span = (end > r.e.start) ? end-r.e.start : 0;
        if (r.span)
            r.sig = detect_save_signature(h,r.e.start,r.span);
        roms.push_back(std::move(r));
    }

    std::cout << "\n========================================\n"
              << "EZF ADVANCE III SAVE READER - CARD CONTENTS\n"
              << "========================================\n"
              << "Layout       : " << (is_single ? "single ROM" : "multi ROM") << "\n"
              << "ROM count    : " << rom_count << "\n"
              << "Loader/menu  : " << hex32(loader_start) << "\n";

    for (size_t i=0;i<roms.size();++i) {
        const auto& r = roms[i];
        std::cout << "\nROM " << (i+1) << "\n"
                  << "  Catalog name : " << r.e.name << "\n"
                  << "  Start        : " << hex32(r.e.start) << "\n"
                  << "  Alloc. span  : " << r.span << " bytes\n"
                  << "  EZ type      : " << static_cast<unsigned>(r.e.type) << "\n"
                  << "  Mapping flag : " << static_cast<unsigned>(r.e.mapping) << "\n"
                  << "  GBA title    : " << (r.g.title.empty() ? "(blank)" : r.g.title) << "\n"
                  << "  Game code    : " << (r.g.game_code.empty() ? "(blank)" : r.g.game_code) << "\n"
                  << "  Save marker  : " << (r.sig.text.empty() ? "(none found)" : r.sig.text) << "\n";
    }

    size_t selected = 0;
    if (requested_rom && (*requested_rom < 1 || *requested_rom > rom_count)) {
        std::cerr << "--rom must be between 1 and " << rom_count << ".\n";
        return 1;
    }

    if (rom_count == 1) {
        selected = 0;
        if (requested_rom && *requested_rom != 1) {
            std::cerr << "Single-ROM card: only --rom 1 is valid.\n";
            return 1;
        }
    } else {
        // The new multi-ROM capture has exactly one capture-proven save-bearing
        // ROM: Piano has no recognized save marker, Bios_Dumper has SRAM_V111.
        // Crucially, there is NO USB ROM-slot selection command before the
        // 0x0900 save read.  Therefore this reader refuses ambiguous cards rather than
        // pretending --rom can switch the active hardware save window.
        std::vector<size_t> proven;
        for (size_t i=0;i<roms.size();++i)
            if (roms[i].sig.capture_proven_32k)
                proven.push_back(i);

        if (proven.size() != 1) {
            std::cerr << "\nThis multi-ROM card has " << proven.size()
                      << " capture-proven SRAM_V111 save-bearing ROM(s).\n"
                      << "The available PCAP shows no USB command for switching "
                         "between multiple save slots, so this reader will not guess.\n";
            return 4;
        }

        selected = proven[0];
        if (requested_rom && (*requested_rom-1) != selected) {
            std::cerr << "\n--rom " << *requested_rom
                      << " is not the uniquely detected capture-proven save ROM.\n"
                      << "No save-slot switch is known, so no dump is performed.\n";
            return 4;
        }

        std::cout << "\nAuto-selected ROM " << (selected+1)
                  << " because it is the only capture-proven SRAM_V111 save ROM.\n";
    }

    const auto& chosen = roms[selected];

    if (!chosen.sig.capture_proven_32k) {
        std::cerr << "\nSelected ROM " << (selected+1) << " has save marker "
                  << (chosen.sig.text.empty() ? "(unknown)" : chosen.sig.text)
                  << ".\nThis reader only performs the capture-proven 32-KiB SRAM_V111 "
                     "save dump. No read is performed for this ROM.\n";
        return 4;
    }

    if (!is_single) {
        // Critical evidence from readmultiromonesav.pcap: there is no
        // additional ROM-slot selection command before this read.  Therefore
        // this reader only uses the multi-ROM path when one uniquely recognized
        // SRAM_V111 save-bearing ROM is present.
        std::cout << "\nMulti-ROM save path: capture shows no extra ROM-slot "
                     "selection command before selector 0x0900.\n";
    }

    std::cout << "\nDumping save for ROM " << (selected+1) << ": "
              << chosen.e.name << "\n";

    std::vector<uint8_t> save;
    if (!read_save_capture(h,0x8000,save)) {
        std::cerr << "Save read failed.\n";
        return 2;
    }

    std::string output;
    if (requested_output) {
        output = *requested_output;
    } else if (!chosen.g.game_code.empty()) {
        output = safe_filename_component(chosen.g.game_code)+".sav";
    } else {
        output = safe_filename_component(chosen.e.name)+".sav";
    }

    std::ofstream f(output,std::ios::binary);
    if (!f) {
        std::cerr << "Could not create output file: " << output << '\n';
        return 5;
    }
    f.write(reinterpret_cast<const char*>(save.data()),
            static_cast<std::streamsize>(save.size()));
    if (!f) {
        std::cerr << "Failed while writing output file: " << output << '\n';
        return 5;
    }

    const size_t nonzero = static_cast<size_t>(std::count_if(
        save.begin(),save.end(),[](uint8_t b){ return b != 0x00; }));
    const size_t nonff = static_cast<size_t>(std::count_if(
        save.begin(),save.end(),[](uint8_t b){ return b != 0xFF; }));

    std::cout << "\n========================================\n"
              << "SAVE DUMP COMPLETE\n"
              << "========================================\n"
              << "ROM          : " << (selected+1) << " / " << rom_count << "\n"
              << "Output       : " << output << "\n"
              << "Size         : " << save.size() << " bytes (32 KiB)\n"
              << "Non-zero     : " << nonzero << " bytes\n"
              << "Non-FF       : " << nonff << " bytes\n"
              << "Protocol     : 0x91/01, selector 0x0900, one 0x8000-byte IN\n\n"
              << "No save-write payload, ROM-program, or erase operation is "
                 "implemented by this program.\n";
    return 0;
}

static void usage(const char* argv0)
{
    std::cout << "Usage:\n"
              << "  " << argv0 << "\n"
              << "  " << argv0 << " --output file.sav\n"
              << "  " << argv0 << " --rom N [--output file.sav]\n\n"
              << "Read-only EZF Advance III save dumper (" << host_platform_name() << ").\n"
              << "Capture-proven path: SRAM_V111 / Bios_Dumper = 32 KiB, "
                 "selector 0x0900.\n"
              << "On a multi-ROM card, the reader auto-selects only when exactly one "
                 "capture-proven save-bearing ROM is found.\n";
}

int main(int argc, char** argv)
{
    std::optional<std::string> output;
    std::optional<size_t> requested_rom;

    for (int i=1;i<argc;++i) {
        const std::string a = argv[i];
        if (a == "--output") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            output = argv[++i];
        } else if (a == "--rom") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            const int n = std::stoi(argv[++i]);
            if (n < 1) {
                std::cerr << "Bad --rom value.\n";
                return 1;
            }
            requested_rom = static_cast<size_t>(n);
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << a << '\n';
            usage(argv[0]);
            return 1;
        }
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
        std::cerr << "Could not claim interface 0: "
                  << libusb_error_name(rc) << '\n';
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    std::cout << "EZF Advance III opened on " << host_platform_name()
              << "; interface 0 claimed.\n";

    int result = 2;
    if (original_manager_read_prime(h))
        result = inspect_and_dump_save(h,output,requested_rom);
    else
        std::cerr << "Card initialization/read-prime failed.\n";

    libusb_release_interface(h,INTERFACE_NUMBER);
    libusb_close(h);
    libusb_exit(ctx);
    return result;
}
