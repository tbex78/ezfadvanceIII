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
#include "ezfadvance/platform.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/ez3_catalog.hpp"
#include "ezfadvance/ez3_catalog_reader.hpp"
#include "ezfadvance/read_only_cartridge.hpp"
#include "ezfadvance/save_memory_reader.hpp"
#include "ezfadvance/save_memory_writer.hpp"
#include "ezfadvance/save_bank_layout.hpp"
#include "ezfadvance/save_bank_selector.hpp"
#include "ezfadvance/save_catalog_analyzer.hpp"
#include "ezfadvance/save_selection.hpp"
#include "ezfadvance/version.hpp"

// EZF Advance III save reader/writer 0.11.1.
// 0.6.2 removes hard-coded project-version text from runtime output.
// Save-read protocol behavior remains unchanged from 0.5.10.
//
// Ported from the historical save-reader v2 implementation. The USB/save
// protocol and conservative safety behavior are intentionally unchanged.
// This utility never sends erase (0x96) commands or ROM-program payloads. Save
// writing is limited to the capture-derived 32-KiB SRAM_V111 transaction and
// requires backup, explicit authorization, and byte-for-byte verification.
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
// Catalog discovery supports the capture-proven 1..8 entry layouts and shared
// 8-/16-/24-/32-MiB linear read mappings.
//
// Save protocol evidence:
//   readsav.pcap: 0x0900 -> 32 KiB, then 0x0910 -> 32 KiB.
//   writesav.pcap: writes exactly the first 32 KiB payload from readsav.pcap.
//   readmultiromonesav.pcap: Piano + Bios_Dumper card; Bios_Dumper save export
//     performs only 0x0900 -> 32 KiB and produces a 32-KiB .sav file.
// The paired FFTA + DumpRom captures additionally expose four shared 32-KiB
// banks. This tool assigns them cumulatively in catalog order so a later ROM
// does not overwrite banks reserved by an earlier ROM.

static constexpr uint32_t CARD_IMAGE_SIZE = 0x02000000u;

static constexpr size_t MAX_CAPTURED_ROMS = 8;

static std::string hex32(uint32_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

static std::string hex16(uint16_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(4) << std::setfill('0') << v;
    return s.str();
}

static bool read_save_capture(ezfadvance::SaveMemoryReader& reader,
                              size_t save_size,
                              uint16_t first_bank,
                              std::vector<uint8_t>& save)
{
    return reader.read(save_size, save, first_bank);
}

static bool write_binary_file_without_overwrite(
    const std::string& path,
    const std::vector<uint8_t>& data)
{
    std::ifstream existing(path, std::ios::binary);
    if (existing.good()) {
        std::cerr << "Refusing to overwrite existing backup file: "
                  << path << '\n';
        return false;
    }
    existing.close();

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "Could not create backup file: " << path << '\n';
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) {
        std::cerr << "Failed while writing backup file: " << path << '\n';
        return false;
    }
    return true;
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

static int inspect_and_dump_save(ezfadvance::ReadOnlyCartridge& cartridge,
                                 ezfadvance::SaveMemoryReader& save_reader,
                                 ezfadvance::SaveMemoryWriter& save_writer,
                                 const std::optional<std::string>& requested_output,
                                 const std::optional<size_t>& requested_rom,
                                 const std::optional<ezfadvance::SaveBankSelector>& requested_bank,
                                 const std::optional<size_t>& requested_bank_count,
                                 const std::optional<std::vector<uint8_t>>& write_save,
                                 const std::optional<std::string>& backup_path)
{
    ezfadvance::Ez3CatalogReader catalog_reader(
        cartridge, CARD_IMAGE_SIZE, MAX_CAPTURED_ROMS);
    const auto discovery = catalog_reader.read();
    if (discovery.status == ezfadvance::Ez3CatalogReadStatus::read_failed)
        return 2;
    if (discovery.status == ezfadvance::Ez3CatalogReadStatus::missing_branch) {
        std::cerr << "Card byte 0 is not an ARM unconditional branch.\n"
                  << "This does not look like a recognized capture-derived EZ3 image.\n";
        return 3;
    }

    if (discovery.status ==
        ezfadvance::Ez3CatalogReadStatus::branch_out_of_range) {
        std::cerr << "Patched branch points to " << hex32(discovery.loader_start)
                  << ", outside the understood 32-MiB cartridge layout.\n";
        return 3;
    }

    const uint32_t loader_start = discovery.loader_start;
    const uint32_t loader_end = discovery.loader_end;
    if (!discovery) {
        std::cerr << "Found loader branch at " << hex32(discovery.loader_start)
                  << " but no recognized EZ3 catalog.\n";
        return 3;
    }
    const auto& catalog = *discovery.catalog;
    const bool is_single = catalog.isSingle();
    const std::size_t rom_count = catalog.entries.size();

    ezfadvance::SaveCatalogAnalyzer analyzer(cartridge, CARD_IMAGE_SIZE);
    const auto analysis = analyzer.analyze(
        catalog, loader_start, loader_end);
    if (!analysis)
        return 2;
    const auto& roms = analysis.roms;

    std::cout << "\n========================================\n"
              << "EZF ADVANCE III SAVE TOOL - CARD CONTENTS\n"
              << "========================================\n"
              << "Layout       : " << (is_single ? "single ROM" : "multi ROM") << "\n"
              << "ROM count    : " << rom_count << "\n"
              << "Loader/menu  : " << hex32(loader_start) << "\n";

    for (size_t i=0;i<roms.size();++i) {
        const auto& r = roms[i];
        std::cout << "\nROM " << (i+1) << "\n"
                  << "  Catalog name : " << r.catalog_entry.name << "\n"
                  << "  Start        : " << hex32(r.catalog_entry.start) << "\n"
                  << "  Alloc. span  : " << r.allocation_span << " bytes\n"
                  << "  EZ type      : " << static_cast<unsigned>(r.catalog_entry.type) << "\n"
                  << "  Mapping flag : " << static_cast<unsigned>(r.catalog_entry.mapping) << "\n"
                  << "  GBA title    : " << (r.header.title.empty() ? "(blank)" : r.header.title) << "\n"
                  << "  Game code    : " << (r.header.game_code.empty() ? "(blank)" : r.header.game_code) << "\n"
                  << "  Save marker  : " << (r.save_marker.empty() ? "(none found)" : r.save_marker) << "\n";
    }

    std::vector<size_t> supported_candidates;
    for (size_t i = 0; i < roms.size(); ++i) {
        if (ezfadvance::supportedSaveSizeForMarker(roms[i].save_marker))
            supported_candidates.push_back(i);
    }

    std::vector<std::string> save_markers;
    save_markers.reserve(roms.size());
    for (const auto& rom : roms)
        save_markers.push_back(rom.save_marker);

    const bool direct_bank_access = ezfadvance::isDirectSaveBankAccess(
        requested_rom, requested_bank.has_value());
    std::optional<size_t> selected;
    size_t selected_save_size = 0;
    uint16_t save_selector = 0;
    if (direct_bank_access) {
        selected_save_size = requested_bank_count
            ? *requested_bank_count * ezfadvance::SaveBankSelector::bank_size
            : ezfadvance::directSaveAccessSize(
                  write_save ? std::optional<size_t>(write_save->size())
                             : std::nullopt);
        if (!requested_bank->accommodates(selected_save_size)) {
            std::cerr << "\nSave bank " << hex16(requested_bank->value())
                      << " cannot contain a " << selected_save_size
                      << "-byte direct save access within the four proven banks. "
                         "No save access was performed.\n";
            return 4;
        }
        save_selector = requested_bank->value();
        std::cout << "\nUsing explicitly requested save bank without ROM "
                     "selection; catalog allocation was bypassed.\n";
    } else {
        const auto selection = ezfadvance::selectSaveRom(
            rom_count, supported_candidates, requested_rom);
        if (selection.status == ezfadvance::SaveSelectionStatus::out_of_range) {
            std::cerr << "--rom must be between 1 and " << rom_count << ".\n";
            return 1;
        }
        if (selection.status == ezfadvance::SaveSelectionStatus::rom_required) {
            std::cerr
                << "\nThis is a multi-ROM card. Specify --rom N, or use "
                   "--save-bank 0x09X0 for direct bank access.\n";
            return 4;
        }
        if (selection.status == ezfadvance::SaveSelectionStatus::no_supported_candidate) {
            std::cerr << "\nThis card has no supported 32-/64-KiB save-bearing ROM.\n"
                      << "No save read was performed.\n";
            return 4;
        }
        if (selection.status == ezfadvance::SaveSelectionStatus::multiple_supported_candidates) {
            std::cerr << "\nThis card has an unsupported ambiguous save selection.\n"
                      << "No save read was performed.\n";
            return 4;
        }
        if (selection.status == ezfadvance::SaveSelectionStatus::requested_rom_mismatch) {
            std::cerr << "\nSelected ROM " << *requested_rom
                      << " does not have a supported save format.\n"
                      << "No save read was performed.\n";
            return 4;
        }
        selected = selection.index;
        const auto selected_size =
            ezfadvance::supportedSaveSizeForMarker(roms[*selected].save_marker);
        if (!selected_size) {
            std::cerr << "\nSelected ROM " << (*selected + 1)
                      << " has no supported save size.\n";
            return 4;
        }
        selected_save_size = *selected_size;
    }

    if (requested_bank && selected) {
        if (!requested_bank->accommodates(selected_save_size)) {
            std::cerr << "\nSave bank " << hex16(requested_bank->value())
                      << " cannot contain this ROM's " << selected_save_size
                      << "-byte save within the four proven banks. "
                         "No save access was performed.\n";
            return 4;
        }
        save_selector = requested_bank->value();
        std::cout << "\nUsing explicitly requested save bank; automatic "
                     "catalog allocation was bypassed.\n";
    } else if (selected) {
        const auto bank_layout =
            ezfadvance::allocateSaveBanks(save_markers, *selected);
        if (bank_layout.status ==
            ezfadvance::SaveBankLayoutStatus::unknown_predecessor_capacity) {
            std::cerr << "\nCannot allocate save banks because ROM "
                      << (bank_layout.first_unknown_rom + 1)
                      << " has an unknown save capacity. No save access was performed.\n";
            return 4;
        }
        if (bank_layout.status == ezfadvance::SaveBankLayoutStatus::capacity_exceeded) {
            std::cerr << "\nThe cumulative save allocation exceeds the four proven "
                         "32-KiB banks. No save access was performed.\n";
            return 4;
        }
        if (bank_layout.status != ezfadvance::SaveBankLayoutStatus::selected)
            return 4;
        save_selector = bank_layout.selector;
    }

    if (!is_single && selected) {
        std::cout << "\nMulti-ROM save allocation: cumulative 32-KiB banks "
                     "in catalog order.\n";
    }
    std::cout << "Selected save bank: " << hex16(save_selector) << "\n";

    if (selected) {
        std::cout << "\nReading save for ROM " << (*selected + 1) << ": "
                  << roms[*selected].catalog_entry.name << "\n";
    } else {
        std::cout << "\nReading explicitly selected save bank "
                  << hex16(save_selector) << "...\n";
    }

    std::vector<uint8_t> save;
    if (!read_save_capture(save_reader,selected_save_size,save_selector,save)) {
        std::cerr << "Save read failed.\n";
        return 2;
    }

    if (write_save) {
        if (!backup_path) {
            std::cerr << "Internal error: save writing requires a backup path.\n";
            return 1;
        }
        if (write_save->size() != selected_save_size) {
            std::cerr << "Save input size " << write_save->size()
                      << " bytes does not match the selected access size of "
                      << selected_save_size
                      << " bytes. No save write was performed.\n";
            return 4;
        }
        if (!write_binary_file_without_overwrite(*backup_path, save))
            return 5;

        std::cout << "Existing save backed up to: " << *backup_path << "\n";
        if (!cartridge.finishSession()) {
            std::cerr << "Pre-write readiness transition failed. No save-write "
                         "payload was sent. Backup: " << *backup_path << '\n';
            return 2;
        }

        const auto bank_count = selected_save_size / 0x8000;
        std::cout << "Writing " << selected_save_size
                  << "-byte save across " << bank_count
                  << " bank" << (bank_count == 1 ? "" : "s")
                  << " beginning at selector " << hex16(save_selector) << "...\n";
        if (!save_writer.write(save_selector, *write_save)) {
            std::cerr << "Save write failed. The original backup is available at "
                      << *backup_path << ".\n";
            return 2;
        }

        if (!cartridge.finishSession()) {
            std::cerr << "Post-write readiness transition failed. The save was "
                         "not verified. Backup: " << *backup_path << '\n';
            return 2;
        }

        std::vector<uint8_t> verification;
        std::cout << "Reading save back for byte-for-byte verification...\n";
        if (!read_save_capture(save_reader, selected_save_size, save_selector, verification)) {
            std::cerr << "Save read-back failed. Backup: " << *backup_path << '\n';
            return 2;
        }
        if (verification != *write_save) {
            const auto mismatch = std::mismatch(
                verification.begin(), verification.end(), write_save->begin());
            const auto offset = static_cast<size_t>(
                std::distance(verification.begin(), mismatch.first));
            std::cerr << "SAVE VERIFICATION FAILED at byte offset 0x"
                      << std::hex << offset << std::dec
                      << ". Backup: " << *backup_path << '\n';
            return 6;
        }

        std::cout << "\n========================================\n"
                  << "SAVE WRITE AND VERIFICATION COMPLETE\n"
                  << "========================================\n"
                  << "ROM          : "
                  << (selected ? std::to_string(*selected + 1) + " / " +
                                     std::to_string(rom_count)
                               : "(direct bank access)") << "\n"
                  << "Backup       : " << *backup_path << "\n"
                  << "Size         : " << selected_save_size << " bytes ("
                  << (selected_save_size / 1024) << " KiB)\n"
                  << "Write        : " << bank_count
                  << " x 0x92/01 from selector " << hex16(save_selector)
                  << ", each with one 0x8000-byte OUT\n"
                  << "Verification : full byte-for-byte 0x91/01 read-back matched\n";
        return 0;
    }

    std::string output;
    if (requested_output) {
        output = *requested_output;
    } else if (!selected) {
        output = "save-bank-" + hex16(save_selector).substr(2) + ".sav";
    } else if (!roms[*selected].header.game_code.empty()) {
        output = safe_filename_component(roms[*selected].header.game_code)+".sav";
    } else {
        output = safe_filename_component(roms[*selected].catalog_entry.name)+".sav";
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
              << "ROM          : "
              << (selected ? std::to_string(*selected + 1) + " / " +
                                 std::to_string(rom_count)
                           : "(direct bank access)") << "\n"
              << "Output       : " << output << "\n"
              << "Size         : " << save.size() << " bytes ("
              << (save.size() / 1024) << " KiB)\n"
              << "Non-zero     : " << nonzero << " bytes\n"
              << "Non-FF       : " << nonff << " bytes\n"
              << "Protocol     : " << (save.size() / 0x8000)
              << " x 0x91/01 from selector " << hex16(save_selector)
              << ", each with one 0x8000-byte IN\n\n"
              << "No save-write payload, ROM-program, or erase operation was "
                 "performed by this extraction.\n";
    return 0;
}

class SaveExtractor final {
public:
    SaveExtractor(ezfadvance::ReadOnlyCartridge& cartridge,
                  ezfadvance::SaveMemoryReader& save_reader,
                  ezfadvance::SaveMemoryWriter& save_writer,
                  std::optional<std::string> output,
                  std::optional<size_t> requested_rom,
                  std::optional<ezfadvance::SaveBankSelector> requested_bank,
                  std::optional<size_t> requested_bank_count,
                  std::optional<std::vector<uint8_t>> write_save,
                  std::optional<std::string> backup_path)
        : cartridge_(cartridge),
          save_reader_(save_reader),
          save_writer_(save_writer),
          output_(std::move(output)),
          requested_rom_(requested_rom),
          requested_bank_(requested_bank),
          requested_bank_count_(requested_bank_count),
          write_save_(std::move(write_save)),
          backup_path_(std::move(backup_path))
    {
    }

    int run()
    {
        return inspect_and_dump_save(cartridge_, save_reader_, save_writer_,
                                     output_, requested_rom_,
                                     requested_bank_,
                                     requested_bank_count_,
                                     write_save_, backup_path_);
    }

private:
    ezfadvance::ReadOnlyCartridge& cartridge_;
    ezfadvance::SaveMemoryReader& save_reader_;
    ezfadvance::SaveMemoryWriter& save_writer_;
    std::optional<std::string> output_;
    std::optional<size_t> requested_rom_;
    std::optional<ezfadvance::SaveBankSelector> requested_bank_;
    std::optional<size_t> requested_bank_count_;
    std::optional<std::vector<uint8_t>> write_save_;
    std::optional<std::string> backup_path_;
};

static void usage(const char* argv0)
{
    std::cout << "Usage:\n"
              << "  " << argv0 << "\n"
              << "  " << argv0 << " --version\n"
              << "  " << argv0 << " --output file.sav\n"
              << "  " << argv0 << " --rom N [--output file.sav]\n\n"
              << "  " << argv0 << " [--rom N] --save-bank 0x09X0 "
                 "[--consecutive-bank N] [--output file.sav]\n\n"
              << "  " << argv0 << " --write file.sav --backup original.sav "
                 "--yes-really-write [--rom N]\n"
              << "  " << argv0 << " --write file.sav --backup original.sav "
                 "--yes-really-write --save-bank 0x09X0 "
                 "[--consecutive-bank N]\n\n"
              << "EZF Advance III save reader/writer (" << ezfadvance::hostPlatformName() << ").\n"
              << "Supported paths: 32-KiB SRAM_V111 and 64-KiB FLASH512 with cumulative four-bank "
                 "allocation beginning at selector 0x0900.\n"
              << "On a multi-ROM card, --rom N is required unless --save-bank "
                 "selects a physical bank directly. --save-bank overrides "
                 "automatic allocation and accepts 0x0900, 0x0910, 0x0920, "
                 "or 0x0930. --consecutive-bank reads or writes 1 to 4 "
                 "consecutive 32-KiB banks in direct-bank mode. For writes, "
                 "the input size must match the requested bank count.\n";
}

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_save_reader");
        return 0;
    }

    std::optional<std::string> output;
    std::optional<size_t> requested_rom;
    std::optional<ezfadvance::SaveBankSelector> requested_bank;
    std::optional<size_t> requested_bank_count;
    std::optional<std::string> write_path;
    std::optional<std::string> backup_path;
    bool yes_really_write = false;

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
        } else if (a == "--write") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            write_path = argv[++i];
        } else if (a == "--save-bank") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            requested_bank = ezfadvance::SaveBankSelector::parse(argv[++i]);
            if (!requested_bank) {
                std::cerr << "Bad --save-bank value; expected 0x0900, "
                             "0x0910, 0x0920, or 0x0930.\n";
                return 1;
            }
        } else if (a == "--consecutive-bank") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            requested_bank_count =
                ezfadvance::parseConsecutiveBankCount(argv[++i]);
            if (!requested_bank_count) {
                std::cerr << "Bad --consecutive-bank value; expected 1 to 4.\n";
                return 1;
            }
        } else if (a == "--backup") {
            if (i+1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            backup_path = argv[++i];
        } else if (a == "--yes-really-write") {
            yes_really_write = true;
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << a << '\n';
            usage(argv[0]);
            return 1;
        }
    }

    if (write_path && output) {
        std::cerr << "--output is for extraction and cannot be combined with --write.\n";
        return 1;
    }
    if (requested_bank_count && !requested_bank) {
        std::cerr << "--consecutive-bank requires --save-bank.\n";
        return 1;
    }
    if (requested_bank_count && requested_rom) {
        std::cerr << "--consecutive-bank is for direct bank access and cannot "
                     "be combined with --rom.\n";
        return 1;
    }
    if (requested_bank_count &&
        !requested_bank->accommodates(
            *requested_bank_count * ezfadvance::SaveBankSelector::bank_size)) {
        std::cerr << "The requested consecutive-bank range extends beyond "
                     "save bank 0x0930.\n";
        return 1;
    }
    if (!write_path && (backup_path || yes_really_write)) {
        std::cerr << "--backup and --yes-really-write require --write FILE.\n";
        return 1;
    }
    if (write_path && (!backup_path || !yes_really_write)) {
        std::cerr << "Save writing requires both --backup FILE and "
                     "--yes-really-write.\n";
        return 1;
    }
    if (ezfadvance::saveWritePathsConflict(write_path, backup_path)) {
        std::cerr << "The input save and backup paths must be different.\n";
        return 1;
    }

    std::optional<std::vector<uint8_t>> write_save;
    if (write_path) {
        std::ifstream input(*write_path, std::ios::binary);
        if (!input) {
            std::cerr << "Could not open save input: " << *write_path << '\n';
            return 1;
        }
        write_save.emplace(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
        if (write_save->empty() || write_save->size() % 0x8000 != 0 ||
            write_save->size() > 0x20000) {
            std::cerr << "Save input must contain 1 to 4 complete 32768-byte "
                         "banks; got "
                      << write_save->size() << ".\n";
            return 1;
        }
        if (requested_bank_count &&
            write_save->size() != *requested_bank_count * 0x8000) {
            std::cerr << "Save input size " << write_save->size()
                      << " does not match --consecutive-bank "
                      << *requested_bank_count << " ("
                      << (*requested_bank_count * 0x8000) << " bytes).\n";
            return 1;
        }
    }

    ezfadvance::UsbDevice device;
    const auto open_result = device.open(std::cerr);
    if (!open_result) {
        ezfadvance::reportUsbOpenFailure(open_result, std::cerr);
        return 1;
    }
    ezfadvance::BulkTransport transport(device.handle());
    ezfadvance::ReadOnlyCartridge cartridge(transport);
    ezfadvance::SaveMemoryReader save_reader(transport);
    ezfadvance::SaveMemoryWriter save_writer(transport);

    std::cout << "EZF Advance III opened on " << ezfadvance::hostPlatformName()
              << "; interface 0 claimed.\n";

    int result = 2;
    if (cartridge.initialize() && ezfadvance::hasEz3Catalog(cartridge.kind())) {
        SaveExtractor extractor(cartridge, save_reader, save_writer,
                                output, requested_rom, requested_bank,
                                requested_bank_count,
                                write_save, backup_path);
        result = extractor.run();
        if (!cartridge.finishSession()) {
            std::cerr << "Save-reader operation finished, but the "
                         "capture-derived read-session close transition "
                         "failed.\n";
            if (result == 0) result = 2;
        }
    } else if (cartridge.kind() != ezfadvance::CartridgeKind::unknown) {
        std::cerr << "Save extraction requires an EZ3 flash cartridge; refusing "
                     "EZ3 catalog/save processing for an official or unknown "
                     "cartridge.\n";
        if (!cartridge.finishSession())
            std::cerr << "Read-session cleanup after cartridge rejection failed.\n";
    } else {
        std::cerr << "EZ3 card initialization/read-prime failed.\n";
    }

    return result;
}
