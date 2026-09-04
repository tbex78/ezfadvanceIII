#include "ezfadvance/cartridge_layout_presenter.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace ezfadvance {
namespace {

void printHex(std::ostream& output,
              const std::uint8_t* data,
              std::size_t size,
              std::size_t maximum)
{
    const std::size_t shown = std::min(size, maximum);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i && (i % 16) == 0) output << '\n';
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(data[i]) << ' ';
    }
    if (shown < size) output << "...";
    output << std::dec << '\n';
}

} // namespace

CartridgeLayoutPresenter::CartridgeLayoutPresenter(std::ostream& output)
    : output_(output)
{
}

void CartridgeLayoutPresenter::print(const std::vector<RomInfo>& roms,
                                     const std::vector<std::uint8_t>& image,
                                     std::size_t programmed_size) const
{
    output_ << "\n========================================\n"
            << "IMAGE LAYOUT\n"
            << "========================================\n";

    for (std::size_t i = 0; i < roms.size(); ++i) {
        const auto& rom = roms[i];
        output_ << "ROM " << (i + 1)
                << ": " << rom.name
                << "\n  file: " << rom.path
                << "\n  size: " << rom.data.size()
                << " (0x" << std::hex << rom.data.size() << std::dec << ")"
                << "\n  byte start: 0x" << std::hex << rom.start << std::dec
                << "\n  original entry target: 0x"
                << std::hex << rom.original_entry_target << std::dec
                << "\n  catalog type: " << static_cast<unsigned>(rom.entry_type)
                << (rom.entry_type_overridden ? " (override)" : " (size-class rule)")
                << "\n  mapping flag: " << static_cast<unsigned>(rom.mapping_flag)
                << (rom.mapping_flag_overridden ? " (override)" : " (signature/map rule)")
                << "\n";
    }

    output_ << "Constructed image bytes: " << image.size()
            << " (0x" << std::hex << image.size() << std::dec << ")\n"
            << "Programmed bytes: " << programmed_size
            << " (0x" << std::hex << programmed_size << std::dec << ")\n"
            << "Image first 4 bytes: ";
    printHex(output_, image.data(), std::min<std::size_t>(4, image.size()), 4);
}

} // namespace ezfadvance
