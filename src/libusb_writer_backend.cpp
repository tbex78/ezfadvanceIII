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

#include "ezfadvance/libusb_writer_backend.hpp"
#include "ezfadvance/flash_window_selector.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/platform.hpp"
#include "ezfadvance/progress_bar.hpp"
#include "ezfadvance/save_bank_cleaner.hpp"
#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/verification_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t PROGRAM_BLOCK = 0x10000;
static constexpr size_t FLASH_WINDOW_SIZE = 0x800000;
static constexpr size_t CARD_HALF_SIZE = 0x1000000;
static constexpr unsigned USB_TIMEOUT_MS = 15000;


static constexpr unsigned COMMAND_DATA_SETTLE_US = 750; // exact legacy DLL busy-wait
static constexpr unsigned READINESS_ATTEMPTS = 5;
static constexpr auto READINESS_RETRY_DELAY = std::chrono::milliseconds(100);

static void print_hex(const uint8_t* p, size_t n, size_t max = 64)
{
    const size_t shown = std::min(n, max);
    for (size_t i = 0; i < shown; ++i) {
        if (i && (i % 16) == 0) std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]) << ' ';
    }
    if (shown < n) std::cout << "...";
    std::cout << std::dec << '\n';
}

static void write_le32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v.at(off + 0) = static_cast<uint8_t>(x >> 0);
    v.at(off + 1) = static_cast<uint8_t>(x >> 8);
    v.at(off + 2) = static_cast<uint8_t>(x >> 16);
    v.at(off + 3) = static_cast<uint8_t>(x >> 24);
}

static bool bulk_out(libusb_device_handle* h,
                     const uint8_t* p,
                     size_t n,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).out(p, n, timeout_ms);
}

static bool bulk_out(libusb_device_handle* h,
                     const std::vector<uint8_t>& data,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return bulk_out(h, data.data(), data.size(), timeout_ms);
}

// Legacy Windows EZF Advance III DLL WriteBulkNonDMAData behavior:
// after sending the 13-byte command, it busy-waits 0x2EE microseconds
// (750 us) before sending the associated data payload. The DLL constant at
// 0x10011458 is exactly 1e-6 and the delay routine uses QueryPerformanceCounter.
// Reproduce that timing rather than the earlier 16 ms / transport experiments.
static void legacy_command_data_settle()
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(COMMAND_DATA_SETTLE_US);
    while (std::chrono::steady_clock::now() < deadline) {
        // Intentional busy-wait: mirrors the original DLL.
    }
}

static bool bulk_in_exact(libusb_device_handle* h,
                          std::vector<uint8_t>& data,
                          size_t wanted,
                          unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).inExact(data, wanted, timeout_ms);
}

static bool bulk_in_max(libusb_device_handle* h,
                        std::vector<uint8_t>& data,
                        size_t max_len,
                        unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).inMax(data, max_len, timeout_ms);
}

static std::vector<uint8_t> cmd92_2()
{
    return ezfadvance::Protocol::command92Two();
}

static std::vector<uint8_t> cmd92_1(uint8_t selector)
{
    return ezfadvance::Protocol::command92One(selector);
}

static bool command_data_echo(libusb_device_handle* h,
                              const std::vector<uint8_t>& command,
                              const std::vector<uint8_t>& data,
                              const std::string& label,
                              unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::Protocol(h).commandDataEcho(
        command, data, label, {timeout_ms, COMMAND_DATA_SETTLE_US, true});
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

// Full bridge initialization captured in initialize.pcap and independently
// observed on the real device:
//
//   0x97 -> one byte 00
//   0x98 -> one byte 01
//   0x99 with parameter 01 -> 13-byte echo
//
// v4/v5 only sent 0x98.  That was sufficient for simple reads and erase
// traffic, but both versions programmed only the first ~0xF2 bytes of a
// 64 KiB block.  The legacy DLL exposes separate DMA/non-DMA bulk paths, and
// the 0x99 parameter is therefore a strong candidate for enabling/configuring
// the bridge's large-transfer path.  v6 restores the complete known-good
// initialization instead of relying on the bridge's previous session state.

// ---------------------------------------------------------------------------
// Original EZ3Manager card-initialization / flash-probe phase.
//
// This sequence is copied from the behavior proven by v22 on Apple Silicon.
// It is deliberately kept intact in v23.  Once ordinary writing is stable,
// individual probes can be removed experimentally to find the minimum prime.
// ---------------------------------------------------------------------------

static bool manager_tx92_2(libusb_device_handle* h,
                           uint8_t a, uint8_t b,
                           const std::string& label)
{
    return tx92_2(h, a, b, label);
}

static bool manager_tx92_1(libusb_device_handle* h,
                           uint8_t selector, uint8_t value,
                           const std::string& label)
{
    return tx92_1(h, selector, value, label);
}

static bool manager_tx92_2_at(libusb_device_handle* h,
                              uint32_t word_address,
                              uint8_t a,
                              uint8_t b,
                              const std::string& label)
{
    std::vector<uint8_t> command = cmd92_2();
    write_le32(command, 4, word_address);
    return command_data_echo(h, command, {a, b}, label);
}

static bool manager_read91_sub2_4(libusb_device_handle* h,
                                  std::array<uint8_t, 4>& response)
{
    const std::vector<uint8_t> command = {
        0x5A,0xA5,0x91,0x02,
        0,0,0,0,
        0x04,0,0,0,0
    };

    if (!bulk_out(h, command))
        return false;

    std::vector<uint8_t> in;
    if (!bulk_in_exact(h, in, 4))
        return false;

    std::copy(in.begin(), in.end(), response.begin());
    return true;
}

static bool manager_read91_normal(libusb_device_handle* h,
                                  uint32_t word_address,
                                  uint8_t* data,
                                  size_t length)
{
    std::vector<uint8_t> command(13, 0);
    command[0] = 0x5A;
    command[1] = 0xA5;
    command[2] = 0x91;
    command[3] = 0x00;
    write_le32(command, 4, word_address);
    write_le32(command, 8, static_cast<uint32_t>(length));

    if (!bulk_out(h, command))
        return false;

    std::vector<uint8_t> in;
    if (!bulk_in_exact(h, in, length))
        return false;

    std::copy(in.begin(), in.end(), data);
    return true;
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

    if (!bulk_out(h, c97) ||
        !bulk_in_exact(h, response, 1) ||
        response[0] != 0x00) {
        std::cerr << "Original-manager 0x97 startup failed.\n";
        return false;
    }
    std::cout << "0x97 -> 00\n";

    // Cartridge-presence/readiness gate.  On the hardware-proven startup path
    // an inserted, ready EZ-Flash Advance III returns one byte 0x01 to 0x98.
    // A run with no cartridge inserted fails at this exact step.  Keep the
    // protocol unchanged, but turn that condition into an explicit preflight
    // diagnostic so the program stops before any erase/program operation.
    bool ready = false;
    for (unsigned attempt = 1; attempt <= READINESS_ATTEMPTS; ++attempt) {
        if (!bulk_out(h, c98)) {
            std::cerr
                << "\nCARTRIDGE PREFLIGHT FAILED: could not send the 0x98 "
                   "readiness command.\n"
                << "Check the ezfadvanceIII USB connection and try again.\n"
                << "No erase or program operation was attempted.\n";
            return false;
        }
        if (!bulk_in_exact(h, response, 1)) {
            std::cerr
                << "\nGBA CARTRIDGE NOT DETECTED / NOT READY.\n"
                << "The ezfadvanceIII device did not return the required 0x98 "
                   "readiness byte (expected 01).\n"
                << "No erase or program operation was attempted.\n";
            return false;
        }
        if (response[0] == 0x01) {
            ready = true;
            break;
        }
        if (response[0] != 0x00) {
            std::cerr << "\nUnexpected 0x98 readiness value 0x"
                      << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(response[0])
                      << std::dec << std::setfill(' ') << ".\n"
                      << "No erase or program operation was attempted.\n";
            return false;
        }
        if (attempt != READINESS_ATTEMPTS) {
            std::cout << "0x98 readiness returned 00; retrying ("
                      << attempt << '/' << READINESS_ATTEMPTS << ")...\n";
            std::this_thread::sleep_for(READINESS_RETRY_DELAY);
        }
    }
    if (!ready) {
        std::cerr
            << "\nGBA CARTRIDGE NOT DETECTED / NOT READY.\n"
            << "0x98 readiness remained 0x00 after " << READINESS_ATTEMPTS
            << " checks.\n"
            << "Make sure an EZ-Flash Advance III cartridge is fully inserted, "
               "then retry.\n"
            << "No erase or program operation was attempted.\n";
        return false;
    }
    std::cout << "0x98 -> 01 (cartridge ready)\n";

    if (!bulk_out(h, c99) ||
        !bulk_in_exact(h, response, c99.size()) ||
        response != c99) {
        std::cerr << "Original-manager 0x99 echo failed.\n";
        return false;
    }
    std::cout << "0x99 echo OK\n";
    return true;
}

static bool manager_probe_unlock_tail(libusb_device_handle* h)
{
    if (!manager_tx92_2(h, 0xAA, 0x55, "manager AA55")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 1")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 2")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 3")) return false;
    if (!manager_tx92_1(h, 0x00, 0xAA, "manager selector0 AA")) return false;
    if (!manager_tx92_1(h, 0x00, 0x55, "manager selector0 55")) return false;
    if (!manager_tx92_1(h, 0x01, 0x06, "manager selector1 06")) return false;
    return true;
}

static bool manager_probe_prefix(libusb_device_handle* h,
                                 uint8_t a0, uint8_t a1,
                                 uint8_t b0, uint8_t b1,
                                 uint8_t c0, uint8_t c1,
                                 bool include_tail = true)
{
    if (!manager_tx92_2(h, 0x55, 0xAA, "manager 55AA")) return false;
    if (!manager_tx92_2(h, a0, a1, "manager probe word 1")) return false;
    if (!manager_tx92_2(h, b0, b1, "manager probe word 2")) return false;
    if (!manager_tx92_2(h, c0, c1, "manager probe word 3")) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(125));

    if (include_tail)
        return manager_probe_unlock_tail(h);
    return true;
}

static bool manager_probe_reset_after_id(libusb_device_handle* h,
                                         bool use_f0)
{
    if (!manager_tx92_2(h,
                        use_f0 ? 0xF0 : 0xFF,
                        use_f0 ? 0x00 : 0xFF,
                        "manager ID reset")) return false;
    if (!manager_tx92_1(h, 0x01, 0x04, "manager status 1/04")) return false;
    if (!manager_tx92_1(h, 0x00, 0x00, "manager status 0/00 #1")) return false;
    if (!manager_tx92_1(h, 0x00, 0x00, "manager status 0/00 #2")) return false;
    return true;
}

static bool manager_flash_id_probe(libusb_device_handle* h,
                                   const char* label,
                                   uint8_t a0, uint8_t a1,
                                   uint8_t b0, uint8_t b1,
                                   uint8_t c0, uint8_t c1)
{
    if (!manager_probe_prefix(h, a0,a1,b0,b1,c0,c1))
        return false;

    if (!manager_tx92_2(h, 0x90, 0x00, "manager 90/00"))
        return false;

    std::array<uint8_t, 4> id{};
    if (!manager_read91_sub2_4(h, id))
        return false;

    std::cout << label << " ID/readback = ";
    print_hex(id.data(), id.size(), id.size());

    if (!manager_probe_reset_after_id(h, false))
        return false;

    return true;
}

static bool original_manager_initialize_and_check(libusb_device_handle* h)
{
    std::cout << "\n========================================\n"
              << "ORIGINAL MANAGER CARD INITIALIZATION / PROBE\n"
              << "========================================\n";

    if (!original_bridge_startup_979899(h))
        return false;

    if (!manager_probe_prefix(h, 0,0, 0,0, 0,0))
        return false;

    if (!manager_tx92_2_at(h, 0x555, 0xAA, 0x00, "manager @555 AA00"))
        return false;
    if (!manager_tx92_2_at(h, 0x2AA, 0x55, 0x00, "manager @2AA 5500"))
        return false;
    if (!manager_tx92_2_at(h, 0x555, 0x90, 0x00, "manager @555 9000"))
        return false;

    std::array<uint8_t, 4> first_probe{};
    if (!manager_read91_sub2_4(h, first_probe))
        return false;
    std::cout << "initial 0x555/0x2AA probe readback = ";
    print_hex(first_probe.data(), first_probe.size(), first_probe.size());

    if (!manager_tx92_2(h, 0x90, 0x00, "manager post-initial 90/00"))
        return false;
    if (!manager_probe_reset_after_id(h, true))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x0000",
            0x00,0x00, 0x00,0x00, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x4000",
            0x02,0x00, 0x00,0x40, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x8000",
            0x02,0x00, 0x00,0x80, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0xC000",
            0x02,0x00, 0x00,0xC0, 0x00,0x00))
        return false;

    // The original-manager probe above has now exercised all four EZ3 ROM
    // windows.  Our capture-derived programming code proves that each window
    // corresponds to 8 MiB.  Therefore the observed address geometry is
    // 4 * 8 MiB = 32 MiB = 256 Mbit.
    //
    // Important: this is deliberately described as a geometry inference, not
    // a JEDEC density decode.  We have not yet proven how to distinguish a
    // smaller device that mirrors upper window selects, so do not use this
    // number to expand write limits automatically.
    std::cout << "\n========================================\n"
              << "CARTRIDGE INFORMATION\n"
              << "========================================\n"
              << "Status              : inserted / ready\n"
              << "EZ3 probe windows   : 4 x 8 MiB\n"
              << "Capacity (inferred) : 32 MiB / 256 Mbit\n"
              << "Detection basis     : responding EZ3 window geometry\n"
              << "Writer support      : full 32 MiB / 256 Mbit (0.5.6; capture-derived geometry)\n"
              << "Note                : capacity is not yet JEDEC-density decoded\n";

    if (!manager_flash_id_probe(
            h, "final probe window",
            0x00,0x00, 0x00,0x00, 0x02,0x00))
        return false;

    if (!manager_probe_prefix(h, 0,0, 0,0, 0,0, false))
        return false;

    const std::vector<uint8_t> c95 = {
        0x5A,0xA5,0x95,0x00,
        0x80,0x00,0x00,0x00,
        0,0,0,0,0
    };
    std::vector<uint8_t> c95_echo;

    if (!bulk_out(h, c95) ||
        !bulk_in_exact(h, c95_echo, c95.size()) ||
        c95_echo != c95) {
        std::cerr << "0x95/0x80 manager command failed.\n";
        return false;
    }
    std::cout << "0x95 parameter 0x80 echo OK\n";

    std::array<uint8_t, 0xAC> header_read{};
    if (!manager_read91_normal(
            h, 0, header_read.data(), header_read.size()))
        return false;

    std::cout << "manager-style card content check: first 0xAC bytes read OK\n"
              << "first 4 bytes = ";
    print_hex(header_read.data(), 4, 4);

    std::cout << "Original-manager initialization/probe phase complete.\n";
    return true;
}

static bool initialize_bridge(libusb_device_handle* h)
{
    std::cout << "\n========================================\n"
              << "CAPTURE-FAITHFUL STARTUP\n"
              << "========================================\n"
              << "No 0x97 and no 0x99 are sent.\n"
              << "Using the XP-proven successful startup: 3 x 0x98 polls.\n";

    const std::vector<uint8_t> c98 = {
        0x5A,0xA5,0x98,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };

    std::vector<uint8_t> r;

    for (unsigned i = 0; i < 3; ++i) {
        std::cout << "0x98 poll " << (i + 1) << "/3 OUT\n";
        if (!bulk_out(h, c98, 5000)) return false;
        if (!bulk_in_exact(h, r, 1, 5000)) return false;
        if (r[0] != 0x01) {
            std::cerr << "0x98 returned 0x" << std::hex
                      << static_cast<unsigned>(r[0]) << std::dec
                      << ", expected 01\n";
            return false;
        }
        std::cout << "0x98 response = 01\n";

        if (i + 1 < 3)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "post-poll quiet interval: 1000 ms\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    return true;
}

// fireemblem.pcap ends programming in window 0x0080 with only a short
// loader tail beyond 16 MiB. The original manager then performs status/reset
// plus the four-write linear-read prefix before 0x91 verification. Keep this
// sequence restricted to that tiny-tail geometry; a larger partial BANK2
// multi-ROM image previously produced a boundary false negative.

static bool erase_sector(libusb_device_handle* h,
                         uint32_t word_address,
                         size_t index,
                         size_t total,
                         bool verbose)
{
    std::vector<uint8_t> cmd = {
        0x5A,0xA5,0x96,0x00,
        static_cast<uint8_t>(word_address >> 0),
        static_cast<uint8_t>(word_address >> 8),
        static_cast<uint8_t>(word_address >> 16),
        static_cast<uint8_t>(word_address >> 24),
        0x00,0x00,0x00,0x00,0x00
    };

    if (!bulk_out(h, cmd, 5000)) return false;

    std::vector<uint8_t> response;
    if (!bulk_in_max(h, response, 64, 5000)) return false;

    if (response.size() != 13 ||
        !std::equal(cmd.begin(), cmd.begin() + 12, response.begin())) {
        std::cerr << "Unexpected 0x96 response at word address 0x"
                  << std::hex << word_address << std::dec << '\n';
        return false;
    }

    if (response[12] != 0x00) {
        std::cerr << "0x96 erase returned nonzero status at word address 0x"
                  << std::hex << word_address << ": 0x"
                  << static_cast<unsigned>(response[12]) << std::dec << '\n';
        return false;
    }

    if (verbose) {
        std::cout << "  erase " << (index + 1) << "/" << total
                  << " @ word 0x" << std::hex << std::setw(6)
                  << std::setfill('0') << word_address
                  << " (byte 0x" << std::setw(6) << (word_address * 2u)
                  << ")" << std::dec << '\n';
    }
    return true;
}

static std::vector<uint32_t> selective_bank0_erase_addresses(size_t programmed_size)
{
    std::vector<uint32_t> out;

    // The capture always erases the boot-sector group first.
    for (uint32_t a = 0x0000; a <= 0x8000; a += 0x1000)
        out.push_back(a);

    // In add3rom, subsequent 64 KiB byte regions map to 0x96 word addresses
    // 0x10000, 0x18000, 0x20000, ...
    // This reproduces the exact add3rom address list for its 0x51900-byte image.
    for (size_t byte_start = 0x20000;
         byte_start < programmed_size;
         byte_start += 0x10000) {
        out.push_back(static_cast<uint32_t>(byte_start / 2));
    }

    return out;
}

static std::vector<uint32_t> full_128mb_bank0_erase_addresses()
{
    std::vector<uint32_t> out;

    // Exact first group from writerom128Mb.pcap: 135 erases.
    // Boot sectors: 0x0000..0x8000 in 0x1000-word steps.
    for (uint32_t a = 0x0000; a <= 0x8000; a += 0x1000)
        out.push_back(a);

    // Remaining bank-0 sectors: 0x10000..0x3F8000 in 0x8000-word steps.
    for (uint32_t a = 0x10000; a <= 0x3F8000; a += 0x8000)
        out.push_back(a);

    return out;
}

static std::vector<uint32_t> full_128mb_bank1_erase_addresses()
{
    std::vector<uint32_t> out;

    // Exact second group from writerom128Mb.pcap: 135 erases.
    for (uint32_t a = 0x0000; a <= 0x3F8000; a += 0x8000)
        out.push_back(a);

    // The last 64-KiB-byte region is split into seven additional 0x1000-word
    // erase sectors in the capture.
    for (uint32_t a = 0x3F9000; a <= 0x3FF000; a += 0x1000)
        out.push_back(a);

    return out;
}

static bool erase_image_capture_faithful(libusb_device_handle* h,
                                         ezfadvance::FlashWindowSelector& windows,
                                         size_t programmed_size,
                                         bool verbose)
{
    if (programmed_size <= FLASH_WINDOW_SIZE) {
        const auto addresses = selective_bank0_erase_addresses(programmed_size);
        std::cout << "\n========================================\n"
                  << "SELECTIVE SECTOR ERASE\n"
                  << "========================================\n";
        ezfadvance::ProgressBar progress("Erase", addresses.size(), false, !verbose);
        for (size_t i = 0; i < addresses.size(); ++i) {
            if (!erase_sector(h, addresses[i], i, addresses.size(), verbose))
                return false;
            progress.update(i + 1);
        }
        return true;
    }

    const auto even_window = full_128mb_bank0_erase_addresses();
    const auto odd_window  = full_128mb_bank1_erase_addresses();

    // fireemblem.pcap erases just 0x96 address 0 in window 2 for its 0x700-byte
    // loader tail. For larger window-2 payloads, v35 conservatively uses the
    // full even-window erase geometry proven by 256MBits-rom.pcap.
    const bool tiny_window2_tail =
        programmed_size > CARD_HALF_SIZE &&
        programmed_size <= CARD_HALF_SIZE + 0x2000;

    std::vector<std::pair<unsigned, std::vector<uint32_t>>> groups;
    groups.push_back({0, even_window});
    groups.push_back({1, odd_window});
    if (programmed_size > CARD_HALF_SIZE) {
        if (tiny_window2_tail)
            groups.push_back({2, std::vector<uint32_t>{0x0000}});
        else
            groups.push_back({2, even_window});
    }
    if (programmed_size > CARD_HALF_SIZE + FLASH_WINDOW_SIZE)
        groups.push_back({3, odd_window});

    size_t total = 0;
    for (const auto& g : groups) total += g.second.size();

    std::cout << "\n========================================\n"
              << "MULTI-WINDOW ERASE (capture-derived)\n"
              << "========================================\n";
    if (verbose) {
        for (const auto& g : groups)
            std::cout << "window " << g.first << ": " << g.second.size()
                      << " erase commands\n";
    }

    ezfadvance::ProgressBar progress("Erase", total, false, !verbose);
    size_t index = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const unsigned window = groups[gi].first;
        if (gi != 0) {
            if (!windows.finishOperation()) return false;
            if (verbose) {
                std::cout << "switching erase window to " << window
                          << " (card byte 0x" << std::hex
                          << (static_cast<size_t>(window) * FLASH_WINDOW_SIZE)
                          << std::dec << ")...\n";
            }
            if (!windows.select(window, verbose ? &std::cout : nullptr))
                return false;
        }
        for (uint32_t a : groups[gi].second) {
            if (!erase_sector(h, a, index, total, verbose)) return false;
            ++index;
            progress.update(index);
        }
    }
    return true;
}

static std::vector<uint8_t> make_program_command(uint32_t byte_address,
                                                 uint32_t length,
                                                 uint8_t final_byte)
{
    if (byte_address & 1u)
        throw std::runtime_error("program address must be word-aligned");

    const uint32_t word_address = byte_address / 2u;
    std::vector<uint8_t> c = {
        0x5A,0xA5,0x92,0x00,
        0,0,0,0,
        0,0,0,0,
        final_byte
    };
    write_le32(c, 4, word_address);
    write_le32(c, 8, length);
    return c;
}

static bool program_transaction_single(libusb_device_handle* h,
                                       uint32_t byte_address,
                                       const uint8_t* data,
                                       size_t n,
                                       bool verbose)
{
    const auto cmd = make_program_command(
        byte_address, static_cast<uint32_t>(n), 0x41);

    if (!bulk_out(h, cmd)) return false;
    legacy_command_data_settle();

    const auto started = std::chrono::steady_clock::now();
    if (!bulk_out(h, data, n, USB_TIMEOUT_MS)) return false;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::vector<uint8_t> completion;
    if (!bulk_in_max(h, completion, 64)) return false;

    auto expected = cmd;
    expected[12] = 0x00;
    if (completion != expected) {
        std::cerr << "Program completion response mismatch at byte 0x"
                  << std::hex << byte_address << std::dec << '\n';
        std::cerr << "Expected:\n";
        print_hex(expected.data(), expected.size(), expected.size());
        std::cerr << "Received:\n";
        if (!completion.empty())
            print_hex(completion.data(), completion.size(), completion.size());
        return false;
    }

    if (verbose) {
        const double kib_s = elapsed > 0.0
            ? (static_cast<double>(n) / 1024.0) / elapsed : 0.0;
        std::cout << "    single data request: " << std::fixed
                  << std::setprecision(3) << elapsed << " s, "
                  << std::setprecision(1) << kib_s << " KiB/s"
                  << std::defaultfloat << '\n';
    }
    return true;
}

static bool program_image(libusb_device_handle* h,
                          ezfadvance::FlashWindowSelector& windows,
                          const std::vector<uint8_t>& image,
                          bool verbose)
{
    std::cout << "\n========================================\n"
              << "PROGRAMMING IMAGE (MANAGER-PRIMED STATE)\n"
              << "========================================\n"
              << "One 0x92 transaction + one BULK OUT per Windows-sized block\n";

    ezfadvance::ProgressBar progress("Program", image.size(), true, !verbose);

    for (size_t off = 0; off < image.size(); ) {
        if (off != 0 && (off % FLASH_WINDOW_SIZE) == 0) {
            const unsigned window = static_cast<unsigned>(off / FLASH_WINDOW_SIZE);
            if (window > 3) {
                std::cerr << "Program window index exceeds captured 256-Mbit geometry.\n";
                return false;
            }
            if (!windows.finishOperation()) return false;
            if (verbose) {
                std::cout << "\nSwitching program window to " << window
                          << " (card byte 0x" << std::hex << off << std::dec
                          << ")...\n";
            }
            if (!windows.select(window, verbose ? &std::cout : nullptr))
                return false;
        }

        const size_t n = std::min(PROGRAM_BLOCK, image.size() - off);
        const size_t local_off = off % FLASH_WINDOW_SIZE;
        if (verbose) {
            std::cout << "program card byte 0x" << std::hex << std::setw(8)
                      << std::setfill('0') << off
                      << " local byte 0x" << std::setw(6) << local_off
                      << " length 0x" << n << std::dec << '\n';
        }

        if (!program_transaction_single(
                h, static_cast<uint32_t>(local_off),
                image.data() + off, n, verbose))
            return false;
        off += n;
        progress.update(off);
    }
    return true;
}

class LibusbWriterBackend final : public ezfadvance::WriterBackend {
public:
    LibusbWriterBackend(libusb_device_handle* handle, bool verbose)
        : handle_(handle), verbose_(verbose), transport_(handle),
          save_bank_cleaner_(transport_),
          flash_windows_(transport_, ezfadvance::FlashWindowSelector::writerTiming()),
          verification_(transport_)
    {
    }

    bool preflight() override { return original_manager_initialize_and_check(handle_); }
    bool initializeBridge() override { return initialize_bridge(handle_); }
    bool prepareGlobalWrite() override {
        if (verbose_) {
            std::cout << "\n========================================\n"
                      << "GLOBAL WRITE SETUP (capture-derived)\n"
                      << "========================================\n"
                      << "32 KiB setup transport: one libusb bulk transfer (baseline)\n";
        }
        return save_bank_cleaner_.clearAll(std::cout, verbose_);
    }
    bool clearSaveBanks() override {
        std::cout << "\nClearing all four save banks after ROM workflow...\n";
        return save_bank_cleaner_.clearAll(std::cout, verbose_);
    }
    bool selectWindowZeroForErase() override {
        return flash_windows_.select(0);
    }
    bool erase(std::size_t image_size) override {
        return erase_image_capture_faithful(
            handle_, flash_windows_, image_size, verbose_);
    }
    bool finalizeFlashState() override { return flash_windows_.finishOperation(); }
    bool selectWindowZeroForProgram() override {
        return flash_windows_.select(0, verbose_ ? &std::cout : nullptr);
    }
    bool program(const std::vector<uint8_t>& image) override {
        return program_image(handle_, flash_windows_, image, verbose_);
    }
    bool verify(ezfadvance::VerificationMode mode,
                const std::vector<uint8_t>& image) override {
        switch (mode) {
        case ezfadvance::VerificationMode::partial_first_window:
            return verifyPartialFirstWindow(image);
        case ezfadvance::VerificationMode::exact_8_mib:
            return verifyExact8MiB(image);
        case ezfadvance::VerificationMode::partial_12_mib:
            return verifyPartial12MiB(image);
        case ezfadvance::VerificationMode::exact_16_mib:
            return verifyExact16MiB(image);
        case ezfadvance::VerificationMode::tiny_tail_above_16_mib:
            return verifyTinyTailAbove16MiB(image);
        case ezfadvance::VerificationMode::partial_20_mib:
            return verifyPartial20MiB(image);
        case ezfadvance::VerificationMode::exact_24_mib:
            return verifyExact24MiB(image);
        case ezfadvance::VerificationMode::partial_28_mib:
            return verifyPartial28MiB(image);
        case ezfadvance::VerificationMode::exact_32_mib:
            return verifyExact32MiB(image);
        case ezfadvance::VerificationMode::unsupported_partial_higher_window:
            return false;
        }
        return false;
    }

private:
    bool verifyPartialFirstWindow(const std::vector<uint8_t>& image) {
        std::cout << "\nPreparing capture-proven partial first-window linear "
                     "read/verify state...\n"
                  << "\n========================================\n"
                  << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
                  << "========================================\n";

        const size_t verify_size =
            ezfadvance::VerificationSession::partialFirstWindowExtent(
                image.size());
        if (verify_size != image.size()) {
            std::cout
                << "Partial BANK0 verify extent rounded to 64-KiB block: 0x"
                << std::hex << verify_size << std::dec << " bytes\n";
        }
        ezfadvance::ProgressBar progress("Verify", verify_size, true, !verbose_);
        return verification_.verifyPartialFirstWindow(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact8MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing lower-8-MiB / 64-Mbit linear read/verify mapping...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact8MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact16MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 128-Mbit / 16-MiB linear read/verify mapping...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact16MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyPartial12MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing capture-proven partial 12-MiB linear "
               "read/verify state...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyPartial12MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyTinyTailAbove16MiB(const std::vector<uint8_t>& image) {
        const size_t verify_size =
            ezfadvance::VerificationSession::tinyTailAbove16MiBExtent(
                image.size());
        std::cout
            << "\nPreparing tiny BANK2-tail linear read/verify mapping "
               "(fireemblem.pcap)...\n"
            << "Short BANK2 tail verify extent: 0x" << std::hex
            << verify_size << std::dec << " bytes\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", verify_size, true, !verbose_);
        return verification_.verifyTinyTailAbove16MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact24MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 192-Mbit / 24-MiB linear read/verify mapping...\n"
            << "    capture quiet interval: approximately 109 ms\n"
            << "    preserved pre-AA55 settle: 125000 us\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact24MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyPartial20MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing capture-proven partial 20-MiB linear "
               "read/verify state...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyPartial20MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyPartial28MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing capture-proven partial 28-MiB linear "
               "read/verify state...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyPartial28MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact32MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 256-Mbit / 32-MiB linear read/verify mapping...\n"
            << "    preserved pre-AA55 settle: 125000 us\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ezfadvance::ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact32MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }

    libusb_device_handle* handle_;
    bool verbose_;
    ezfadvance::BulkTransport transport_;
    ezfadvance::SaveBankCleaner save_bank_cleaner_;
    ezfadvance::FlashWindowSelector flash_windows_;
    ezfadvance::VerificationSession verification_;
};


namespace ezfadvance {

std::unique_ptr<WriterBackend> makeLibusbWriterBackend(
    libusb_device_handle* handle, bool verbose)
{
    return std::unique_ptr<WriterBackend>(
        new ::LibusbWriterBackend(handle, verbose));
}

} // namespace ezfadvance
