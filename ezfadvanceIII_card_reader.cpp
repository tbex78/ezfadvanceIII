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

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/read_only_cartridge.hpp"

// EZF Advance III card reader 0.7.0, read-only inspector.
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

static bool read_card(libusb_device_handle* h,
                      uint32_t byte_address,
                      std::vector<uint8_t>& out,
                      size_t length)
{
    return ezfadvance::ReadOnlyCartridge(h).read(byte_address, out, length);
}

static bool original_manager_read_prime(libusb_device_handle* h)
{
    return ezfadvance::ReadOnlyCartridge(h).initialize();
}

static bool prepare_linear_16m_read(libusb_device_handle* h)
{
    return ezfadvance::ReadOnlyCartridge(h).prepareLinear16MiB();
}

static bool prepare_linear_24m_read(libusb_device_handle* h)
{
    return ezfadvance::ReadOnlyCartridge(h).prepareLinear24MiB();
}

static bool prepare_linear_32m_read(libusb_device_handle* h)
{
    return ezfadvance::ReadOnlyCartridge(h).prepareLinear32MiB();
}

static std::optional<uint32_t> arm_branch_target(uint32_t ins)
{
    return ezfadvance::CartridgeFormat::armBranchTarget(ins);
}

using GbaHeader = ezfadvance::GbaHeader;

static GbaHeader read_gba_header(libusb_device_handle* h, uint32_t start)
{
    std::vector<uint8_t> b;
    if (!read_card(h,start,b,0xC0)) return {};
    return GbaHeader::parse(b);
}

using CatalogEntry = ezfadvance::CatalogEntry;

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
    return CatalogEntry::parse(loader, off, first);
}

static bool plausible_entry(const CatalogEntry& e, bool first)
{
    return e.plausible(CARD_IMAGE_LIMIT, first);
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

class CardInspector final {
public:
    explicit CardInspector(libusb_device_handle* handle) noexcept
        : handle_(handle)
    {
    }

    int run() { return inspect_card(handle_); }

private:
    libusb_device_handle* handle_;
};

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

    int result = 2;
    if (original_manager_read_prime(h)) {
        CardInspector inspector(h);
        result = inspector.run();
    }
    else
        std::cerr << "Card initialization/read-prime failed.\n";

    return result;
}
