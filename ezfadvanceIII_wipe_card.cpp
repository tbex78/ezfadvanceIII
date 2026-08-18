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
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/protocol.hpp"

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

// ezfadvanceIII wipe utility 0.7.10.
// 0.6.2 removes hard-coded project-version text from runtime output.
// Erase protocol/timing behavior remains unchanged from 0.5.10.
//
// Ported from the capture-derived wipe-card v3 utility.
// The erase geometry, per-bank setup, 0x96 erase commands, status sequence,
// cleanup, and two capture-derived blank-verification reads are intentionally
// unchanged. 0.5.10 changes naming and Unix-like portability only.
//
// Supported native targets: macOS, Linux, FreeBSD, OpenBSD, NetBSD,
// DragonFly BSD. Native Windows support is intentionally out of scope;
// Windows users should use a Linux VM with direct USB passthrough for
// VID 0x0E6A / PID 0x5088.

static void print_hex(const uint8_t* p, size_t n, size_t max = 64)
{
    const size_t shown = std::min(n, max);
    for (size_t i = 0; i < shown; ++i) {
        if (i && (i % 16) == 0) std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]) << ' ';
    }
    std::cout << std::dec << '\n';
}

static bool bulk_out(libusb_device_handle* h,
                     const std::vector<uint8_t>& data,
                     unsigned timeout_ms = 5000)
{
    return ezfadvance::BulkTransport(h).out(data, timeout_ms);
}

static bool bulk_in_max(libusb_device_handle* h,
                        std::vector<uint8_t>& data,
                        size_t max_len,
                        unsigned timeout_ms = 5000)
{
    return ezfadvance::BulkTransport(h).inMax(data, max_len, timeout_ms);
}

// Non-destructive cartridge/readiness preflight.  The writer and real-device
// testing established that an inserted, ready EZ-Flash Advance III returns a
// single 0x01 byte to command 0x98.  Keep this check minimal so the captured
// delete.pcap erase sequence below remains otherwise unchanged.
static bool cartridge_ready_preflight(libusb_device_handle* h)
{
    const std::vector<uint8_t> c98 = {
        0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0
    };

    std::vector<uint8_t> response;

    std::cout << "\n========================================\n"
              << "CARTRIDGE PREFLIGHT\n"
              << "========================================\n";

    if (!bulk_out(h, c98, 5000)) {
        std::cerr
            << "CARTRIDGE PREFLIGHT FAILED: could not send the 0x98 readiness command.\n"
            << "Check the EZF Advance III USB connection and try again.\n"
            << "No erase operation was attempted.\n";
        return false;
    }

    if (!bulk_in_max(h, response, 1, 5000) || response.size() != 1) {
        std::cerr
            << "GBA CARTRIDGE NOT DETECTED / NOT READY.\n"
            << "The EZF Advance III did not return the required 0x98 readiness byte "
               "(expected 01).\n"
            << "Make sure an EZ-Flash Advance III cartridge is fully inserted, then retry.\n"
            << "No erase operation was attempted.\n";
        return false;
    }

    if (response[0] != 0x01) {
        std::cerr
            << "GBA CARTRIDGE NOT DETECTED / NOT READY.\n"
            << "0x98 readiness returned 0x"
            << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(response[0])
            << std::dec << std::setfill(' ')
            << "; expected 0x01.\n"
            << "Make sure an EZ-Flash Advance III cartridge is fully inserted, then retry.\n"
            << "No erase operation was attempted.\n";
        return false;
    }

    std::cout << "0x98 -> 01 (cartridge inserted / ready)\n";
    return true;
}

static std::vector<uint8_t> cmd92_2()
{
    // delete.pcap:
    // 5A A5 92 02 00 00 00 00 02 00 00 00 00
    return ezfadvance::Protocol::command92Two();
}

static std::vector<uint8_t> cmd92_1(uint8_t selector)
{
    // selector=0 for AA/55/00 operations
    // selector=1 for 06/04 operations
    return ezfadvance::Protocol::command92One(selector);
}

static bool command_data_echo(libusb_device_handle* h,
                              const std::vector<uint8_t>& command,
                              const std::vector<uint8_t>& data,
                              const char* label,
                              unsigned timeout_ms = 5000)
{
    return ezfadvance::Protocol(h).commandDataEcho(
        command, data, label, {timeout_ms, 0, true});
}

static bool tx92_2(libusb_device_handle* h,
                   uint8_t a, uint8_t b,
                   const char* label)
{
    return command_data_echo(h, cmd92_2(), {a,b}, label);
}

static bool tx92_1(libusb_device_handle* h,
                   uint8_t selector, uint8_t value,
                   const char* label)
{
    return command_data_echo(h, cmd92_1(selector), {value}, label);
}

static bool flash_bank_setup(libusb_device_handle* h, unsigned bank)
{
    // Exact per-bank setup derived from delete.pcap.
    // bank 0: 55AA, 0000, 0000, 0000, AA55, 0000, 0000, 0000, AA,55,06
    // bank 1: 55AA, 0200, 0040, 0000, AA55, ...
    // bank 2: 55AA, 0200, 0080, 0000, AA55, ...
    // bank 3: 55AA, 0200, 00C0, 0000, AA55, ...
    const uint8_t bank_select_hi = static_cast<uint8_t>(bank * 0x40);

    if (!tx92_2(h, 0x55,0xAA, "SETUP 55AA")) return false;
    if (bank == 0) {
        if (!tx92_2(h, 0x00,0x00, "SETUP BANK MODE 0000")) return false;
    } else {
        if (!tx92_2(h, 0x02,0x00, "SETUP BANK MODE 0200")) return false;
    }
    if (!tx92_2(h, 0x00,bank_select_hi, "SETUP BANK SELECT")) return false;
    if (!tx92_2(h, 0x00,0x00, "SETUP 0000 A")) return false;

    if (!tx92_2(h, 0xAA,0x55, "SETUP AA55")) return false;
    if (!tx92_2(h, 0x00,0x00, "SETUP 0000 B")) return false;
    if (!tx92_2(h, 0x00,0x00, "SETUP 0000 C")) return false;
    if (!tx92_2(h, 0x00,0x00, "SETUP 0000 D")) return false;

    if (!tx92_1(h, 0x00,0xAA, "SETUP AA")) return false;
    if (!tx92_1(h, 0x00,0x55, "SETUP 55")) return false;
    if (!tx92_1(h, 0x01,0x06, "SETUP 06")) return false;

    return true;
}

static bool flash_status_sequence(libusb_device_handle* h)
{
    // Exact sequence after each 135-sector sweep in delete.pcap.
    if (!tx92_2(h, 0xFF,0xFF, "STATUS FFFF")) return false;
    if (!tx92_1(h, 0x01,0x04, "STATUS 04")) return false;
    if (!tx92_1(h, 0x00,0x00, "STATUS 00 A")) return false;
    if (!tx92_1(h, 0x00,0x00, "STATUS 00 B")) return false;
    return true;
}

static bool final_cleanup(libusb_device_handle* h)
{
    // Exact cleanup immediately before blank verification in delete.pcap.
    if (!tx92_2(h, 0x55,0xAA, "CLEANUP 55AA")) return false;
    if (!tx92_2(h, 0x00,0x00, "CLEANUP 0000 A")) return false;
    if (!tx92_2(h, 0x00,0x00, "CLEANUP 0000 B")) return false;
    if (!tx92_2(h, 0x00,0x00, "CLEANUP 0000 C")) return false;
    return true;
}

static std::vector<uint32_t> bottom_boot_sector_addresses()
{
    std::vector<uint32_t> v;
    // 9 small sectors: 0x0000 .. 0x8000, step 0x1000.
    for (uint32_t a = 0x000000; a <= 0x008000; a += 0x001000)
        v.push_back(a);
    // 126 large sectors: 0x010000 .. 0x3F8000, step 0x8000.
    for (uint32_t a = 0x010000; a <= 0x3F8000; a += 0x008000)
        v.push_back(a);
    return v;
}

static std::vector<uint32_t> top_boot_sector_addresses()
{
    std::vector<uint32_t> v;
    // 128 large sectors: 0x000000 .. 0x3F8000, step 0x8000.
    for (uint32_t a = 0x000000; a <= 0x3F8000; a += 0x008000)
        v.push_back(a);
    // 7 small sectors: 0x3F9000 .. 0x3FF000, step 0x1000.
    for (uint32_t a = 0x3F9000; a <= 0x3FF000; a += 0x001000)
        v.push_back(a);
    return v;
}

static bool erase_sector(libusb_device_handle* h,
                         uint32_t address,
                         unsigned bank,
                         size_t sector_index,
                         size_t sector_count)
{
    std::vector<uint8_t> cmd = {
        0x5A,0xA5,0x96,0x00,
        static_cast<uint8_t>(address >> 0),
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 24),
        0x00,0x00,0x00,0x00,0x00
    };

    if (!bulk_out(h, cmd, 5000)) {
        std::cerr << "Erase command failed at bank " << bank
                  << " address 0x" << std::hex << address << std::dec << '\n';
        return false;
    }

    std::vector<uint8_t> response;
    if (!bulk_in_max(h, response, 64, 5000)) {
        std::cerr << "Erase response failed at bank " << bank
                  << " address 0x" << std::hex << address << std::dec << '\n';
        return false;
    }

    if (response.size() != 13 ||
        !std::equal(cmd.begin(), cmd.begin() + 12, response.begin())) {
        std::cerr << "Unexpected 0x96 response at bank " << bank
                  << " address 0x" << std::hex << address << std::dec << "\nExpected prefix:\n";
        print_hex(cmd.data(), 12, 12);
        std::cerr << "Received:\n";
        print_hex(response.data(), response.size(), response.size());
        return false;
    }

    // In delete.pcap, byte 12 is 0x00 for every successful erase.
    // During earlier non-erasing live tests it was 0x01, so treat non-zero as failure.
    if (response[12] != 0x00) {
        std::cerr << "0x96 erase status is non-zero at bank " << bank
                  << " address 0x" << std::hex << address
                  << ": status=0x" << static_cast<unsigned>(response[12])
                  << std::dec << '\n';
        return false;
    }

    if (sector_index == 0 || ((sector_index + 1) % 8) == 0 || sector_index + 1 == sector_count) {
        std::cout << "  bank " << (bank + 1) << "/4: sector "
                  << (sector_index + 1) << "/" << sector_count
                  << " @ 0x" << std::hex << std::setw(6) << std::setfill('0')
                  << address << std::dec << '\n';
    }

    return true;
}

static bool erase_bank(libusb_device_handle* h, unsigned bank)
{
    const std::vector<uint32_t> sectors =
        (bank == 0 || bank == 2)
        ? bottom_boot_sector_addresses()
        : top_boot_sector_addresses();

    if (sectors.size() != 135) {
        std::cerr << "Internal error: expected 135 sectors, got "
                  << sectors.size() << '\n';
        return false;
    }

    std::cout << "\n========================================\n";
    std::cout << "ERASING FLASH BANK " << (bank + 1) << "/4\n";
    std::cout << "========================================\n";

    if (!flash_bank_setup(h, bank)) {
        std::cerr << "Bank setup failed for bank " << bank << '\n';
        return false;
    }

    for (size_t i = 0; i < sectors.size(); ++i) {
        if (!erase_sector(h, sectors[i], bank, i, sectors.size()))
            return false;
    }

    if (!flash_status_sequence(h)) {
        std::cerr << "Post-erase status sequence failed for bank " << bank << '\n';
        return false;
    }

    return true;
}

static bool read_region(libusb_device_handle* h,
                        const std::vector<uint8_t>& command,
                        size_t expected_len,
                        std::vector<uint8_t>& result)
{
    if (!bulk_out(h, command, 5000)) return false;
    if (!bulk_in_max(h, result, expected_len, 10000)) return false;
    if (result.size() != expected_len) {
        std::cerr << "Blank verification read returned " << result.size()
                  << " bytes, expected " << expected_len << '\n';
        return false;
    }
    return true;
}

static bool all_ff(const std::vector<uint8_t>& v)
{
    return std::all_of(v.begin(), v.end(), [](uint8_t b) { return b == 0xFF; });
}

static bool verify_blank_like_capture(libusb_device_handle* h)
{
    std::cout << "\n========================================\n";
    std::cout << "CAPTURE-DERIVED BLANK VERIFICATION\n";
    std::cout << "========================================\n";

    // delete.pcap: read 172 bytes from address 0.
    const std::vector<uint8_t> q1 = {
        0x5A,0xA5,0x91,0x00,
        0x00,0x00,0x00,0x00,
        0xAC,0x00,0x00,0x00,
        0x00
    };

    // delete.pcap: read 32 bytes from address/selector 0x02000002.
    const std::vector<uint8_t> q2 = {
        0x5A,0xA5,0x91,0x00,
        0x02,0x00,0x00,0x02,
        0x20,0x00,0x00,0x00,
        0x00
    };

    std::vector<uint8_t> r1, r2;
    if (!read_region(h, q1, 172, r1)) return false;
    if (!all_ff(r1)) {
        auto it = std::find_if(r1.begin(), r1.end(), [](uint8_t b){ return b != 0xFF; });
        size_t off = static_cast<size_t>(std::distance(r1.begin(), it));
        std::cerr << "Blank verification #1 failed at offset 0x"
                  << std::hex << off << std::dec << ": 0x"
                  << std::hex << static_cast<unsigned>(*it) << std::dec << '\n';
        return false;
    }
    std::cout << "Verification #1: 172/172 bytes are FF\n";

    if (!read_region(h, q2, 32, r2)) return false;
    if (!all_ff(r2)) {
        auto it = std::find_if(r2.begin(), r2.end(), [](uint8_t b){ return b != 0xFF; });
        size_t off = static_cast<size_t>(std::distance(r2.begin(), it));
        std::cerr << "Blank verification #2 failed at offset 0x"
                  << std::hex << off << std::dec << ": 0x"
                  << std::hex << static_cast<unsigned>(*it) << std::dec << '\n';
        return false;
    }
    std::cout << "Verification #2: 32/32 bytes are FF\n";

    return true;
}

class CardEraser final {
public:
    explicit CardEraser(libusb_device_handle* handle) noexcept
        : handle_(handle)
    {
    }

    bool execute()
    {
        // Safety gate: no destructive 0x96 erase command is sent until the
        // cartridge has returned the proven 0x98 readiness byte.
        bool ok = cartridge_ready_preflight(handle_);
        for (unsigned bank = 0; bank < 4 && ok; ++bank)
            ok = erase_bank(handle_, bank);

        if (ok) {
            std::cout << "\nRunning final cleanup sequence...\n";
            ok = final_cleanup(handle_);
        }
        if (ok)
            ok = verify_blank_like_capture(handle_);
        return ok;
    }

private:
    libusb_device_handle* handle_;
};

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--yes-really-wipe") {
        std::cerr
            << "EZF Advance III card wipe utility (" << host_platform_name() << ")\n\n"
            << "WARNING: This operation is destructive and erases the cartridge flash.\n\n"
            << "RECOMMENDED BEFORE USE:\n"
            << "  Unplug the EZF Advance III USB device, then plug it back in before running\n"
            << "  this wipe utility. This gives the wipe a fresh USB/bridge session.\n\n"
            << "Usage: " << argv[0] << " --yes-really-wipe\n";
        return 1;
    }

    std::cout
        << "========================================\n"
        << "BEFORE WIPING\n"
        << "========================================\n"
        << "Recommended: unplug the EZF Advance III USB device and plug it back in\n"
        << "before using this wipe utility, so the wipe starts from a fresh USB/bridge session.\n\n"
        << "WARNING: ERASE REQUEST CONFIRMED.\n"
        << "This reproduces the destructive erase sequence observed in delete.pcap.\n";

    ezfadvance::UsbDevice device;
    const auto open_result = device.open(std::cerr);
    if (!open_result) {
        if (open_result.status == ezfadvance::UsbOpenStatus::initialization_failed) {
            std::cerr << "libusb_init failed: "
                      << libusb_error_name(open_result.libusb_error) << '\n';
        } else if (open_result.status == ezfadvance::UsbOpenStatus::device_not_found) {
            std::cerr << "EZF Advance III USB device VID=0x0E6A PID=0x5088 not found.\n";
        } else {
            std::cerr << "Could not claim interface 0: "
                      << libusb_error_name(open_result.libusb_error) << '\n';
        }
        return 1;
    }
    libusb_device_handle* h = device.handle();

    std::cout << "EZF Advance III opened on " << host_platform_name()
              << "; interface 0 claimed.\n";

    CardEraser eraser(h);
    const bool ok = eraser.execute();

    std::cout << "\n========================================\n";
    if (ok) {
        std::cout << "CARD WIPE COMPLETED.\n";
        std::cout << "The two blank-check reads captured by Windows both returned all FF.\n";
    } else {
        std::cout << "CARD WIPE FAILED OR COULD NOT BE VERIFIED.\n";
    }
    std::cout << "========================================\n";

    return ok ? 0 : 2;
}
