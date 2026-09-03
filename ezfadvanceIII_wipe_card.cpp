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

#include <iostream>
#include <string>

#include "ezfadvance/card_wipe_workflow.hpp"
#include "ezfadvance/flash_window_selector.hpp"
#include "ezfadvance/platform.hpp"
#include "ezfadvance/save_bank_cleaner.hpp"
#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/version.hpp"

namespace {

void printUsage(const char* program)
{
    std::cerr
        << "EZF Advance III card wipe utility ("
        << ezfadvance::hostPlatformName() << ")\n\n"
        << "WARNING: This operation is destructive and erases cartridge "
           "flash and all four save banks.\n\n"
        << "RECOMMENDED BEFORE USE:\n"
        << "  Unplug the EZF Advance III USB device, then plug it back in "
           "before running\n"
        << "  this wipe utility. This gives the wipe a fresh USB/bridge "
           "session.\n\n"
        << "Usage: " << program << " --yes-really-wipe\n"
        << "       " << program << " --version\n";
}

void printSummary(bool succeeded)
{
    std::cout << "\n========================================\n";
    if (succeeded) {
        std::cout
            << "CARD WIPE COMPLETED.\n"
            << "The two blank-check reads captured by Windows both returned "
               "all FF.\n"
            << "All four 32-KiB save banks were explicitly written with "
               "zeros.\n";
    } else {
        std::cout
            << "CARD WIPE FAILED OR COULD NOT BE VERIFIED.\n"
            << "Please unplug and reconnect the EZF Advance III, then try "
               "again.\n";
    }
    std::cout << "========================================\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_wipe_card");
        return 0;
    }

    if (argc != 2 || std::string(argv[1]) != "--yes-really-wipe") {
        printUsage(argv[0]);
        return 1;
    }

    std::cout
        << "========================================\n"
        << "BEFORE WIPING\n"
        << "========================================\n"
        << "Recommended: unplug the EZF Advance III USB device and plug it "
           "back in\n"
        << "before using this wipe utility, so the wipe starts from a fresh "
           "USB/bridge session.\n\n"
        << "WARNING: ERASE REQUEST CONFIRMED.\n"
        << "This erases ROM flash and explicitly zeroes all four 32-KiB save "
           "banks.\n";

    ezfadvance::UsbDevice device;
    const auto open_result = device.open(std::cerr);
    if (!open_result) {
        ezfadvance::reportUsbOpenFailure(open_result, std::cerr);
        return 1;
    }

    std::cout << "EZF Advance III opened on "
              << ezfadvance::hostPlatformName()
              << "; interface 0 claimed.\n";

    ezfadvance::BulkTransport transport(device.handle());
    ezfadvance::FlashWindowSelector flash_windows(
        transport, ezfadvance::FlashWindowSelector::wipeTiming());
    ezfadvance::SaveBankCleaner save_bank_cleaner(transport);
    ezfadvance::CardWipeWorkflow workflow(
        transport, flash_windows, save_bank_cleaner, std::cout, std::cerr);
    const bool succeeded = workflow.execute();

    printSummary(succeeded);
    return succeeded ? 0 : 2;
}
