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

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/read_only_cartridge.hpp"
#include "ezfadvance/save_memory_reader.hpp"

// EZF Advance III save reader 0.7.22, read-only dumper.
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

static bool read_card(libusb_device_handle* h,
                      uint32_t byte_address,
                      std::vector<uint8_t>& out,
                      size_t length)
{
    return ezfadvance::ReadOnlyCartridge(h).read(byte_address, out, length);
}

static bool initialize_ez3_read_session(libusb_device_handle* h)
{
    ezfadvance::ReadOnlyCartridge cartridge(h);
    if (!cartridge.initialize()) return false;
    if (ezfadvance::hasEz3Catalog(cartridge.kind())) return true;

    std::cerr << "Save extraction requires an EZ3 flash cartridge; refusing "
                 "EZ3 catalog/save processing for an official or unknown "
                 "cartridge.\n";
    if (!cartridge.finishSession())
        std::cerr << "Read-session cleanup after cartridge rejection failed.\n";
    return false;
}

static bool prepare_linear_16m_read(libusb_device_handle* h)
{
    return ezfadvance::ReadOnlyCartridge(h).prepareLinear16MiB();
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

static CatalogEntry parse_catalog_entry(const std::vector<uint8_t>& loader,
                                        size_t off,
                                        bool first)
{
    return CatalogEntry::parse(loader, off, first);
}

static bool plausible_entry(const CatalogEntry& e, bool first)
{
    return e.plausible(CAPTURE_LINEAR_LIMIT, first);
}

static std::string hex32(uint32_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

static bool read_save_capture(libusb_device_handle* h,
                              size_t save_size,
                              std::vector<uint8_t>& save)
{
    return ezfadvance::SaveMemoryReader(h).read(save_size, save);
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

class SaveExtractor final {
public:
    SaveExtractor(libusb_device_handle* handle,
                  std::optional<std::string> output,
                  std::optional<size_t> requested_rom)
        : handle_(handle),
          output_(std::move(output)),
          requested_rom_(requested_rom)
    {
    }

    int run()
    {
        return inspect_and_dump_save(handle_, output_, requested_rom_);
    }

private:
    libusb_device_handle* handle_;
    std::optional<std::string> output_;
    std::optional<size_t> requested_rom_;
};

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
    if (initialize_ez3_read_session(h)) {
        SaveExtractor extractor(h, output, requested_rom);
        result = extractor.run();
        if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) {
            std::cerr << "Save-reader operation finished, but the "
                         "capture-derived read-session close transition "
                         "failed.\n";
            if (result == 0) result = 2;
        }
    }
    else
        std::cerr << "EZ3 card initialization/read-prime failed.\n";

    return result;
}
