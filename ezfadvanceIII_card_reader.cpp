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

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/platform.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/card_reader_options.hpp"
#include "ezfadvance/ez3_card_workflow.hpp"
#include "ezfadvance/read_only_cartridge.hpp"
#include "ezfadvance/progress_bar.hpp"
#include "ezfadvance/version.hpp"

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
//   Windows 10/11
//
// Windows 10/11 is supported through libusb with a compatible WinUSB/libusbK
// device driver. Unix-like targets retain their existing native paths.
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

static constexpr uint32_t CARD_IMAGE_LIMIT   = 0x02000000u; // 32 MiB / 256 Mbit

using GbaHeader = ezfadvance::GbaHeader;

static int inspect_official_cartridge(ezfadvance::ReadOnlyCartridge& cartridge)
{
    std::vector<std::uint8_t> header_bytes;
    const bool read_ok = cartridge.read(0, header_bytes, 0xC0);
    const bool header_ok = read_ok &&
        ezfadvance::CartridgeFormat::validGbaRomHeader(header_bytes);
    const GbaHeader header = read_ok ? GbaHeader::parse(header_bytes)
                                     : GbaHeader{};
    if (!header_ok) {
        std::cerr << "Official-ROM probe behavior was detected, but the GBA "
                     "fixed header byte/checksum validation failed; the "
                     "cartridge is not confirmed as an official GBA ROM.\n";
        if (!cartridge.finishSession()) {
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
    if (!cartridge.finishSession()) return 2;
    std::cout << "\nNo erase or ROM programming operation was performed.\n";
    return 0;
}

static int extract_official_cartridge(ezfadvance::ReadOnlyCartridge& cartridge,
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
    const auto extraction_started = std::chrono::steady_clock::now();
    {
        ezfadvance::ProgressBar progress("Extract", dump_size);
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
    const bool finish_ok = cartridge.finishSession();
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

static void usage(const char* argv0)
{
    std::cout << "Usage:\n"
              << "  " << argv0 << "\n"
              << "  " << argv0 << " --version\n"
              << "  " << argv0 << " --extract OUTPUT.gba [--verbose]\n"
              << "  " << argv0 << " --extract N OUTPUT.gba [--verbose]\n"
              << "  " << argv0 << " --dump OUTPUT.gba [--verbose]\n"
              << "  " << argv0 << " --dump N OUTPUT.gba [--verbose]\n\n"
              << "Read-only EZF Advance III card inspector (" << ezfadvance::hostPlatformName() << ").\n"
              << "--extract and --dump are equivalent. They read 32 MiB and write a trimmed .gba dump of a detected "
                 "official GBA cartridge.\n"
              << "--verbose replaces the extraction progress bar with per-block "
                 "address and timing diagnostics.\n";
    std::cout << "For EZ3 flash, extraction defaults to catalog ROM 1; adding "
                 "N selects another displayed ROM number. ROM 1's original "
                 "entry is reconstructed.\n";
}

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_card_reader");
        return 0;
    }

    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    ezfadvance::CardReaderOptions options;
    const auto parse_status = ezfadvance::parseCardReaderOptions(
        arguments, options, std::cerr);
    if (parse_status == ezfadvance::CardReaderParseStatus::help) {
        usage(argv[0]);
        return 0;
    }
    if (parse_status == ezfadvance::CardReaderParseStatus::error) {
        usage(argv[0]);
        return 1;
    }

    if (options.extract) {
        std::ifstream existing(options.output_path, std::ios::binary);
        if (existing.good()) {
            std::cerr << "Refusing to overwrite existing output file: "
                      << options.output_path << '\n';
            return 1;
        }
    }

    ezfadvance::UsbDevice device;
    const auto open_result = device.open(std::cerr);
    if (!open_result) {
        ezfadvance::reportUsbOpenFailure(open_result, std::cerr);
        return 1;
    }
    ezfadvance::ReadOnlyCartridge cartridge(device.handle());

    std::cout << "EZF Advance III opened on " << ezfadvance::hostPlatformName()
              << "; interface 0 claimed.\n";

    int result = 2;
    if (cartridge.initialize()) {
        const auto decision = ezfadvance::decideCardReaderAction(
            cartridge.kind(), options);
        switch (decision.action) {
        case ezfadvance::CardReaderAction::inspect_official:
            result = inspect_official_cartridge(cartridge);
            break;
        case ezfadvance::CardReaderAction::extract_official:
            result = extract_official_cartridge(
                cartridge, options.output_path, options.verbose);
            break;
        case ezfadvance::CardReaderAction::inspect_ez3: {
            ezfadvance::Ez3CardWorkflow inspector(
                cartridge, std::nullopt, options.verbose);
            result = inspector.run();
            break;
        }
        case ezfadvance::CardReaderAction::extract_ez3: {
            if (!options.rom_number) {
                std::cout << "No EZ3 ROM number specified; defaulting to "
                             "catalog ROM 1.\n";
            }
            ezfadvance::Ez3CardWorkflow inspector(
                cartridge,
                ezfadvance::Ez3ExtractionRequest{decision.rom_number,
                                                 options.output_path},
                options.verbose);
            result = inspector.run();
            break;
        }
        case ezfadvance::CardReaderAction::reject:
            std::cerr << decision.error << '\n';
            result = cartridge.finishSession() ? 1 : 2;
            break;
        }
    }
    else
        std::cerr << "Card initialization/classification failed.\n";

    return result;
}
