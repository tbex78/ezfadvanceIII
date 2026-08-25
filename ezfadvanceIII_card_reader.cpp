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
#include <fstream>
#include <iomanip>
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

// EZF Advance III card reader 0.7.23, read-only inspector.
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

static std::string progress_duration(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const uint64_t total_seconds = static_cast<uint64_t>(seconds + 0.5);
    const uint64_t hours = total_seconds / 3600;
    const uint64_t minutes = (total_seconds % 3600) / 60;
    const uint64_t secs = total_seconds % 60;
    std::ostringstream output;
    if (hours != 0) {
        output << hours << ':' << std::setw(2) << std::setfill('0') << minutes
               << ':' << std::setw(2) << secs;
    } else {
        output << minutes << ':' << std::setw(2) << std::setfill('0') << secs;
    }
    return output.str();
}

class ProgressBar final {
public:
    ProgressBar(std::string label, std::size_t total)
        : label_(std::move(label)), total_(total),
          started_(std::chrono::steady_clock::now())
    {
    }

    ~ProgressBar()
    {
        if (drew_ && !finished_) std::cout << '\n';
    }

    void update(std::size_t completed)
    {
        completed = std::min(completed, total_);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_).count();
        const double ratio = total_ == 0 ? 1.0 :
            static_cast<double>(completed) / static_cast<double>(total_);
        static constexpr std::size_t width = 32;
        const std::size_t filled = std::min<std::size_t>(
            static_cast<std::size_t>(ratio * width), width);

        std::ostringstream output;
        output << label_ << " [";
        for (std::size_t i = 0; i < width; ++i)
            output << (i < filled ? '=' :
                       (i == filled && completed < total_ ? '>' : ' '));
        output << "] " << std::fixed << std::setprecision(1)
               << ratio * 100.0 << "%  " << std::setprecision(2)
               << static_cast<double>(completed) / (1024.0 * 1024.0) << '/'
               << static_cast<double>(total_) / (1024.0 * 1024.0) << " MiB";

        if (elapsed > 0.0 && completed != 0) {
            const double rate = static_cast<double>(completed) / elapsed;
            const double remaining = rate > 0.0 ?
                static_cast<double>(total_ - completed) / rate : 0.0;
            output << "  " << std::setprecision(1) << rate / 1024.0
                   << " KiB/s  elapsed " << progress_duration(elapsed)
                   << "  ETA " << progress_duration(remaining);
        }

        const std::string line = output.str();
        std::cout << '\r' << line;
        if (last_width_ > line.size())
            std::cout << std::string(last_width_ - line.size(), ' ');
        std::cout << std::flush;
        last_width_ = line.size();
        drew_ = true;
        if (completed == total_) {
            std::cout << '\n';
            finished_ = true;
        }
    }

private:
    std::string label_;
    std::size_t total_;
    bool drew_ = false;
    bool finished_ = false;
    std::size_t last_width_ = 0;
    std::chrono::steady_clock::time_point started_;
};

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

static bool initialize_read_session(libusb_device_handle* h,
                                    ezfadvance::CartridgeKind& kind)
{
    ezfadvance::ReadOnlyCartridge cartridge(h);
    const bool initialized = cartridge.initialize();
    kind = cartridge.kind();
    return initialized;
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

static int inspect_official_cartridge(libusb_device_handle* h)
{
    std::vector<std::uint8_t> header_bytes;
    const bool read_ok = read_card(h, 0, header_bytes, 0xC0);
    const bool header_ok = read_ok &&
        ezfadvance::CartridgeFormat::validGbaRomHeader(header_bytes);
    const GbaHeader header = read_ok ? GbaHeader::parse(header_bytes)
                                     : GbaHeader{};
    if (!header_ok) {
        std::cerr << "Official-ROM probe behavior was detected, but the GBA "
                     "fixed header byte/checksum validation failed; the "
                     "cartridge is not confirmed as an official GBA ROM.\n";
        if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) {
            std::cerr << "Read-session cleanup after header rejection failed.\n";
            return 2;
        }
        return 3;
    }
    std::cout << "\n========================================\n"
              << "OFFICIAL GBA CARTRIDGE\n"
              << "========================================\n"
              << "GBA title    : " << (header.title.empty() ? "(blank)" : header.title) << '\n'
              << "Game code    : " << (header.game_code.empty() ? "(blank)" : header.game_code) << '\n'
              << "Maker code   : " << (header.maker_code.empty() ? "(blank)" : header.maker_code) << '\n'
              << "ROM version  : " << static_cast<unsigned>(header.version) << '\n'
              << "Header check : " << (header.checksum_ok ? "OK" : "FAILED") << '\n';
    if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) return 2;
    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

static int extract_official_cartridge(libusb_device_handle* h,
                                      const std::string& output_path,
                                      bool verbose)
{
    constexpr std::size_t dump_size = CARD_IMAGE_LIMIT;
    constexpr std::size_t block_size = 0x10000;
    std::vector<std::uint8_t> image(dump_size);
    std::cout << "Reading the full 32-MiB GBA address space as "
              << (dump_size / block_size) << " x 64-KiB blocks...\n";

    bool read_ok = true;
    bool header_checked = false;
    bool header_ok = false;
    ezfadvance::ReadOnlyCartridge cartridge(h);
    const auto extraction_started = std::chrono::steady_clock::now();
    {
        ProgressBar progress("Extract", dump_size);
        if (!verbose) progress.update(0);
        for (std::size_t offset = 0; offset < dump_size && read_ok;
             offset += block_size) {
            const auto block_started = std::chrono::steady_clock::now();
            read_ok = cartridge.read(static_cast<std::uint32_t>(offset),
                                     image.data() + offset, block_size);
            if (!read_ok) continue;
            if (offset == 0) {
                header_checked = true;
                std::vector<std::uint8_t> first_block(
                    image.begin(), image.begin() + block_size);
                header_ok = ezfadvance::CartridgeFormat::validGbaRomHeader(
                    first_block);
                if (!header_ok) {
                    read_ok = false;
                    continue;
                }
                if (verbose)
                    std::cout << "Validated GBA header checksum and fixed "
                                 "value 0x96 before full extraction.\n";
            }
            if (verbose) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - block_started).count();
                const double kib_per_second = seconds > 0.0 ?
                    (static_cast<double>(block_size) / 1024.0) / seconds : 0.0;
                std::cout << "read card byte 0x" << std::hex
                          << std::setw(8) << std::setfill('0') << offset
                          << " length 0x" << block_size << std::dec
                          << std::setfill(' ') << ": " << std::fixed
                          << std::setprecision(3) << seconds << " s, "
                          << std::setprecision(1) << kib_per_second
                          << " KiB/s\n";
            } else {
                progress.update(offset + block_size);
            }
        }
    }
    if (verbose && read_ok) {
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - extraction_started).count();
        const double mib_per_second = seconds > 0.0 ?
            (static_cast<double>(dump_size) / (1024.0 * 1024.0)) / seconds : 0.0;
        std::cout << "ROM read complete: " << dump_size << " bytes in "
                  << std::fixed << std::setprecision(3) << seconds << " s ("
                  << std::setprecision(2) << mib_per_second << " MiB/s).\n";
    }
    const bool finish_ok = ezfadvance::ReadOnlyCartridge(h).finishSession();
    if (header_checked && !header_ok) {
        std::cerr << "Official cartridge classification was not sufficient: "
                     "the GBA header checksum/fixed value is invalid. "
                     "Extraction was refused.\n";
        if (!finish_ok)
            std::cerr << "Read-session cleanup also failed.\n";
        return 3;
    }
    if (!read_ok) {
        std::cerr << "Official cartridge extraction failed during ROM read.\n";
        if (!finish_ok)
            std::cerr << "Read-session cleanup also failed.\n";
        return 2;
    }
    if (!finish_ok) {
        std::cerr << "ROM data was read, but read-session cleanup failed; "
                     "no output file was written.\n";
        return 2;
    }

    const auto output_size =
        ezfadvance::CartridgeFormat::trimmedGbaRomSize(image);
    if (!output_size) {
        std::cerr << "Could not derive a supported 1/2/4/8/16/32-MiB ROM size "
                     "from the trailing 0xFF region; no output was written.\n";
        return 3;
    }
    const std::size_t removed = image.size() - *output_size;
    image.resize(*output_size);
    std::cout << "Detected ROM extent: " << (image.size() / (1024 * 1024))
              << " MiB; removed " << (removed / (1024 * 1024))
              << " MiB of trailing 0xFF address-space padding.\n";

    std::ofstream output(output_path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "Could not create output file: " << output_path << '\n';
        return 2;
    }
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    output.close();
    if (!output) {
        std::cerr << "Failed while writing output file: " << output_path << '\n';
        return 2;
    }

    std::cout << "Wrote " << image.size() << " bytes to " << output_path
              << "\nThe output was rounded up to the smallest supported "
                 "1/2/4/8/16/32-MiB size containing all non-0xFF data.\n"
              << "No erase or ROM programming operation was performed.\n";
    return 0;
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

static std::optional<uint32_t> stored_end(const CatalogEntry& entry)
{
    if (entry.type > 9)
        return std::nullopt;
    const uint64_t size_class =
        static_cast<uint64_t>(CARD_IMAGE_LIMIT) >> entry.type;
    const uint64_t end =
        static_cast<uint64_t>(entry.start) + size_class - 1;
    if (end >= CARD_IMAGE_LIMIT)
        return std::nullopt;
    return static_cast<uint32_t>(end);
}

struct Ez3ExtractionRequest {
    std::size_t rom_number = 0;
    std::string output_path;
};

static bool write_binary_file(const std::string& path,
                              const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "Could not create output file: " << path << '\n';
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "Failed while writing output file: " << path << '\n';
        return false;
    }
    return true;
}

static int extract_ez3_rom(libusb_device_handle* h,
                           const std::vector<CatalogEntry>& entries,
                           uint32_t loader_start,
                           bool is_single,
                           const Ez3ExtractionRequest& request,
                           bool verbose)
{
    const auto fail_after_cleanup = [h](int status) {
        if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) {
            std::cerr << "Read-session cleanup also failed.\n";
            return 2;
        }
        return status;
    };
    if (request.rom_number == 0 || request.rom_number > entries.size()) {
        std::cerr << "ROM number " << request.rom_number
                  << " is outside the catalog range 1.." << entries.size()
                  << ".\n";
        return fail_after_cleanup(1);
    }

    const std::size_t index = request.rom_number - 1;
    const CatalogEntry& entry = entries[index];
    const auto inclusive_end = stored_end(entry);
    if (!inclusive_end || *inclusive_end < entry.start) {
        std::cerr << "ROM " << request.rom_number
                  << " has an invalid catalog size class or extent.\n";
        return fail_after_cleanup(3);
    }
    const std::size_t size =
        static_cast<std::size_t>(*inclusive_end - entry.start) + 1;

    // Inspection selects a mapping from loader/header addresses. Extraction
    // must additionally cover the selected ROM's complete stored extent.
    if (*inclusive_end >= LINEAR24_LIMIT) {
        if (!prepare_linear_32m_read(h)) return fail_after_cleanup(2);
    } else if (*inclusive_end >= LINEAR16_LIMIT) {
        if (!prepare_linear_24m_read(h)) return fail_after_cleanup(2);
    } else if (*inclusive_end >= FLASH_WINDOW_SIZE) {
        if (!prepare_linear_16m_read(h)) return fail_after_cleanup(2);
    }

    std::vector<std::uint8_t> image;
    std::cout << "Extracting ROM " << request.rom_number << " from "
              << hex32(entry.start) << " through " << hex32(*inclusive_end)
              << " (" << size << " bytes)...\n";
    image.resize(size);
    constexpr std::size_t block_size = 0x10000;
    bool read_ok = true;
    const auto extraction_started = std::chrono::steady_clock::now();
    {
        ProgressBar progress("Extract", size);
        if (!verbose) progress.update(0);
        for (std::size_t offset = 0; offset < size && read_ok;
             offset += block_size) {
            const std::size_t length = std::min(block_size, size - offset);
            const auto block_started = std::chrono::steady_clock::now();
            read_ok = ezfadvance::ReadOnlyCartridge(h).read(
                entry.start + static_cast<std::uint32_t>(offset),
                image.data() + offset, length);
            if (!read_ok) continue;
            if (verbose) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - block_started).count();
                const double kib_per_second = seconds > 0.0 ?
                    (static_cast<double>(length) / 1024.0) / seconds : 0.0;
                std::cout << "read card byte "
                          << hex32(entry.start + static_cast<std::uint32_t>(offset))
                          << " length 0x" << std::hex << length << std::dec
                          << ": " << std::fixed << std::setprecision(3)
                          << seconds << " s, " << std::setprecision(1)
                          << kib_per_second << " KiB/s\n";
            } else {
                progress.update(offset + length);
            }
        }
    }
    if (!read_ok) {
        std::cerr << "ROM extraction failed during card read.\n";
        return fail_after_cleanup(2);
    }
    if (verbose) {
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - extraction_started).count();
        const double mib_per_second = seconds > 0.0 ?
            (static_cast<double>(size) / (1024.0 * 1024.0)) / seconds : 0.0;
        std::cout << "ROM read complete: " << size << " bytes in "
                  << std::fixed << std::setprecision(3) << seconds << " s ("
                  << std::setprecision(2) << mib_per_second << " MiB/s).\n";
    }

    // Original EZ3Manager places the loader in erased ROM space when it can.
    // Reconstruct that overlap as erased bytes rather than exporting loader
    // code as part of the game. The known single loader is 0x660 bytes; the
    // maximum capture-derived multi loader extent is 0x7080 bytes.
    const std::size_t loader_length = is_single ? 0x660u : LOADER_READ_SIZE;
    const std::uint64_t rom_begin = entry.start;
    const std::uint64_t rom_end = rom_begin + image.size();
    const std::uint64_t loader_begin = loader_start;
    const std::uint64_t loader_end = loader_begin + loader_length;
    const std::uint64_t overlap_begin = std::max(rom_begin, loader_begin);
    const std::uint64_t overlap_end = std::min(rom_end, loader_end);
    if (overlap_begin < overlap_end) {
        std::fill(image.begin() + static_cast<std::ptrdiff_t>(overlap_begin - rom_begin),
                  image.begin() + static_cast<std::ptrdiff_t>(overlap_end - rom_begin),
                  0xFF);
        std::cout << "Restored overlapping loader area "
                  << hex32(static_cast<uint32_t>(overlap_begin)) << ".."
                  << hex32(static_cast<uint32_t>(overlap_end - 1))
                  << " to 0xFF.\n";
    }

    if (!ezfadvance::CartridgeFormat::restoreEz3Entry(
            image, entry, index == 0)) {
        std::cerr << "Could not reconstruct ROM 1's original entry branch.\n";
        return fail_after_cleanup(3);
    }
    if (index == 0)
        std::cout << "Restored ROM 1 entry branch to catalog target "
                  << hex32(entry.target_or_start) << ".\n";

    if (!ezfadvance::CartridgeFormat::validGbaRomHeader(image)) {
        std::cerr << "Reconstructed ROM header validation failed; no output "
                     "file was written.\n";
        return fail_after_cleanup(3);
    }
    if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) {
        std::cerr << "ROM data was read, but read-session cleanup failed; "
                     "no output file was written.\n";
        return 2;
    }
    if (!write_binary_file(request.output_path, image))
        return 2;

    std::cout << "Wrote " << image.size() << " bytes to "
              << request.output_path
              << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
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

    const auto end = stored_end(e);
    std::cout << "  Stored end   : "
              << (end ? hex32(*end) : "(invalid size class/extent)") << "\n";

    if (g.readable) {
        std::cout << "  GBA title    : " << (g.title.empty() ? "(blank)" : g.title) << "\n"
                  << "  Game code    : " << (g.game_code.empty() ? "(blank)" : g.game_code) << "\n"
                  << "  Maker code   : " << (g.maker_code.empty() ? "(blank)" : g.maker_code) << "\n"
                  << "  ROM version  : " << static_cast<unsigned>(g.version) << "\n"
                  << "  Header check : " << (g.checksum_ok ? "OK" : "FAILED") << "\n";
    }
}

static int inspect_card(libusb_device_handle* h,
                        const std::optional<Ez3ExtractionRequest>& extraction,
                        bool verbose)
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

    if (extraction)
        return extract_ez3_rom(h, entries, loader_start, is_single,
                               *extraction, verbose);

    if (!ezfadvance::ReadOnlyCartridge(h).finishSession()) {
        std::cerr << "Card inspection succeeded, but the capture-derived "
                     "read-session close transition failed.\n";
        return 2;
    }

    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

class CardInspector final {
public:
    CardInspector(libusb_device_handle* handle,
                  std::optional<Ez3ExtractionRequest> extraction,
                  bool verbose) noexcept
        : handle_(handle), extraction_(std::move(extraction)), verbose_(verbose)
    {
    }

    int run() { return inspect_card(handle_, extraction_, verbose_); }

private:
    libusb_device_handle* handle_;
    std::optional<Ez3ExtractionRequest> extraction_;
    bool verbose_ = false;
};

static void usage(const char* argv0)
{
    std::cout << "Usage:\n"
              << "  " << argv0 << "\n"
              << "  " << argv0 << " --extract OUTPUT.gba [--verbose]\n"
              << "  " << argv0 << " --extract N OUTPUT.gba [--verbose]\n\n"
              << "Read-only EZF Advance III card inspector (" << host_platform_name() << ").\n"
              << "--extract reads 32 MiB and writes a trimmed .gba dump of a detected "
                 "official GBA cartridge.\n"
              << "--verbose replaces the extraction progress bar with per-block "
                 "address and timing diagnostics.\n";
    std::cout << "For EZ3 flash, --extract N OUTPUT.gba reads the displayed "
                 "catalog ROM number and reconstructs ROM 1's original entry.\n";
}

int main(int argc, char** argv)
{
    bool extract = false;
    bool verbose = false;
    std::optional<Ez3ExtractionRequest> ez3_extraction;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--extract") {
            if (extract || ez3_extraction || i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            extract = true;
            const std::string first_value = argv[++i];
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                try {
                    std::size_t consumed = 0;
                    const unsigned long parsed =
                        std::stoul(first_value, &consumed, 10);
                    if (consumed != first_value.size() || parsed == 0)
                        throw std::invalid_argument("bad ROM number");
                    output_path = argv[++i];
                    ez3_extraction = Ez3ExtractionRequest{
                        static_cast<std::size_t>(parsed), output_path};
                } catch (const std::exception&) {
                    std::cerr << "Bad ROM number for EZ3 --extract.\n";
                    usage(argv[0]);
                    return 1;
                }
            } else {
                output_path = first_value;
            }
            if (output_path.empty()) {
                usage(argv[0]);
                return 1;
            }
        } else if (argument == "--verbose") {
            verbose = true;
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            usage(argv[0]);
            return 1;
        }
    }
    if (verbose && !extract) {
        std::cerr << "--verbose is currently available with --extract only.\n";
        usage(argv[0]);
        return 1;
    }

    if (extract || ez3_extraction) {
        const std::string& destination = extract ? output_path
                                                  : ez3_extraction->output_path;
        std::ifstream existing(destination, std::ios::binary);
        if (existing.good()) {
            std::cerr << "Refusing to overwrite existing output file: "
                      << destination << '\n';
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
    ezfadvance::CartridgeKind kind = ezfadvance::CartridgeKind::unknown;
    if (initialize_read_session(h, kind)) {
        if (kind == ezfadvance::CartridgeKind::official_gba_rom) {
            if (ez3_extraction) {
                std::cerr << "Numbered --extract requires an EZ3 flash cartridge.\n";
                result = ezfadvance::ReadOnlyCartridge(h).finishSession() ? 1 : 2;
            } else
            result = extract ? extract_official_cartridge(h, output_path,
                                                          verbose)
                             : inspect_official_cartridge(h);
        } else {
            if (extract && !ez3_extraction) {
                std::cerr << "EZ3 extraction requires a catalog ROM number: "
                             "--extract N OUTPUT.gba\n";
                result = ezfadvance::ReadOnlyCartridge(h).finishSession() ? 1 : 2;
            } else {
                CardInspector inspector(h, ez3_extraction, verbose);
                result = inspector.run();
            }
        }
    }
    else
        std::cerr << "Card initialization/classification failed.\n";

    return result;
}
