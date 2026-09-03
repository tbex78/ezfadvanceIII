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

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/cartridge_image_builder.hpp"
#include "ezfadvance/card_writer.hpp"
#include "ezfadvance/card_write_presenter.hpp"
#include "ezfadvance/cartridge_layout_presenter.hpp"
#include "ezfadvance/libusb_writer_backend.hpp"
#include "ezfadvance/platform.hpp"
#include "ezfadvance/rom_input_loader.hpp"
#include "ezfadvance/verification_session.hpp"
#include "ezfadvance/writer_options.hpp"
#include "ezfadvance/verification_policy.hpp"
#include "ezfadvance/version.hpp"

static constexpr size_t MAX_CARD_IMAGE = 0x2000000;   // 256 Mbit / 32 MiB card

using RomInfo = ezfadvance::RomInfo;
using BuiltCartridgeImage = ezfadvance::BuiltCartridgeImage;
using CartridgeImageBuilder = ezfadvance::CartridgeImageBuilder;

static void usage(const char* argv0)
{
    std::cerr
        << "ezfadvanceIII manager-primed ROM writer (" << ezfadvance::hostPlatformName() << ")\n\n"
        << "Version:\n"
        << "  " << argv0 << " --version\n\n"
        << "Dry run / inspect layout only:\n"
        << "  " << argv0 << " rom1.gba [rom2.gba ...]\n\n"
        << "Build in memory + erase/program/verify:\n"
        << "  " << argv0 << " --yes-really-write "
           "rom1.gba [rom2.gba ...]\n\n"
        << "Build in memory + erase/program without read-back verify:\n"
        << "  " << argv0 << " --yes-really-write --skip-verify "
           "rom1.gba [rom2.gba ...]\n\n"
        << "Output options:\n"
        << "  --verbose   Show per-sector/per-block erase, program, timing, and verify diagnostics.\n"
        << "              Without it, erase/program/verify use live progress bars.\n\n"
        << "Optional menu-title override:\n"
        << "  --title1=Name   --title6=Another\n\n"
        << "Optional capture-metadata overrides:\n"
        << "  --type1=2   --type6=3   --type10=4\n"
        << "  --map1=6    --map6=6    --map10=3\n\n"
        << "Experimental image construction:\n"
        << "  --experimental-single-multi-loader\n"
        << "      Build a one-ROM image with the multi-ROM loader/catalog.\n";
}

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_multirom_writer");
        return 0;
    }

    try {
        ezfadvance::WriterOptions options;
        const auto parse_result = ezfadvance::WriterOptions::parse(
            argc, argv, options, std::cerr);
        if (!parse_result.ok) {
            if (parse_result.show_usage) usage(argv[0]);
            return 1;
        }
        const bool do_write = options.write;
        const bool skip_verify = options.skip_verify;
        const bool verbose = options.verbose;
        const bool experimental_single_multi_loader =
            options.experimental_single_multi_loader;
        const auto& type_override = options.type_overrides;
        const auto& mapping_override = options.mapping_overrides;
        const auto& title_override = options.title_overrides;
        const auto& rom_paths = options.rom_paths;

        if (rom_paths.empty()) {
            usage(argv[0]);
            return 1;
        }
        if (experimental_single_multi_loader && rom_paths.size() != 1) {
            std::cerr << "--experimental-single-multi-loader requires exactly one ROM.\n";
            return 1;
        }
        if (rom_paths.size() > CartridgeImageBuilder::catalog_slots) {
            std::cerr << "Too many ROMs for the embedded catalog structure: "
                      << rom_paths.size() << " supplied, "
                      << CartridgeImageBuilder::catalog_slots
                      << " slots available.\n";
            return 1;
        }

        const ezfadvance::RomInputLoader rom_loader(
            std::cin, std::cout, std::cerr);
        std::vector<RomInfo> roms;
        for (size_t i = 0; i < rom_paths.size(); ++i) {
            RomInfo r;
            r.path = rom_paths[i];
            r.name = title_override[i].value_or(
                ezfadvance::RomInputLoader::deriveCatalogName(r.path));
            if (type_override[i]) {
                r.entry_type = *type_override[i];
                r.entry_type_overridden = true;
            }
            if (mapping_override[i]) {
                r.mapping_flag = *mapping_override[i];
                r.mapping_flag_overridden = true;
            }

            if (!rom_loader.load(r))
                return 1;

            roms.push_back(std::move(r));
        }

        uint64_t total_rom_file_bytes = 0;
        for (const auto& r : roms)
            total_rom_file_bytes += static_cast<uint64_t>(r.data.size());

        std::cout << "Total input ROM file bytes: " << total_rom_file_bytes
                  << " (0x" << std::hex << total_rom_file_bytes << std::dec
                  << "), cartridge capacity " << MAX_CARD_IMAGE
                  << " bytes / 256 Mbit\n";

        if (total_rom_file_bytes > MAX_CARD_IMAGE) {
            std::cerr << "Total input ROM file size exceeds the 256-Mbit / "
                         "32-MiB cartridge capacity.\n";
            return 1;
        }

        const CartridgeImageBuilder image_builder;
        BuiltCartridgeImage built_image;
        std::string image_build_error;
        const ezfadvance::CartridgeImageBuildOptions image_options{
            experimental_single_multi_loader};
        if (!image_builder.build(
                roms, built_image, image_build_error, image_options)) {
            std::cerr << image_build_error << '\n';
            return 1;
        }
        std::cout << built_image.report;
        if (experimental_single_multi_loader) {
            std::cout
                << "WARNING: experimental one-entry multi-loader image.\n"
                << "This layout is not derived from an original-manager capture "
                   "and requires real-hardware qualification.\n";
        }
        std::vector<uint8_t>& image = built_image.bytes;
        const size_t programmed_size = built_image.programmed_size;

        ezfadvance::CartridgeLayoutPresenter{std::cout}.print(
            roms, image, programmed_size);

        if (!do_write) {
            std::cout
                << "\nDRY RUN: no USB device was touched and the cartridge was not modified.\n"
                << "Review the layout above, then rerun with --yes-really-write "
                   "to program it.\n";
            return 0;
        }

        std::cout
            << "\nWARNING: --yes-really-write supplied.\n"
            << "This will erase sectors at the beginning of the cartridge "
               "and program the constructed image.\n";

        ezfadvance::UsbDevice device;
        const auto open_result = device.open(std::cerr);
        if (!open_result) {
            ezfadvance::reportUsbOpenFailure(
                open_result, std::cerr, "ezfadvanceIII");
            return 1;
        }
        libusb_device_handle* h = device.handle();
        auto writer_backend = ezfadvance::makeLibusbWriterBackend(h, verbose);
        const ezfadvance::CardWriter card_writer(*writer_backend);

        std::cout << "ezfadvanceIII opened; interface 0 claimed.\n";

        const auto write_result = card_writer.write(image, skip_verify, std::cout);

        if (write_result.status == ezfadvance::CardWriteStatus::preflight_failed) {
            std::cerr
                << "\n========================================\n"
                << "WRITE PREFLIGHT ABORTED\n"
                << "========================================\n"
                << "Initialization/card check did not complete.\n"
                << "No erase or program operation was attempted.\n";

            return 2;
        }

        ezfadvance::printCardWriteSummary(write_result, std::cout);

        return write_result ? 0 : 2;
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
