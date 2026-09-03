#include "ezfadvance/save_output_path.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace ezfadvance {

std::string SaveOutputPath::resolve(
    const std::optional<std::string>& requested_path,
    const std::optional<std::size_t>& selected_rom,
    std::uint16_t selector,
    const std::vector<SaveCatalogRom>& roms)
{
    if (requested_path) return *requested_path;
    if (!selected_rom) {
        std::ostringstream path;
        path << "save-bank-" << std::hex << std::setw(4)
             << std::setfill('0') << selector << ".sav";
        return path.str();
    }

    const auto& rom = roms.at(*selected_rom);
    const auto& identity = rom.header.game_code.empty()
        ? rom.catalog_entry.name
        : rom.header.game_code;
    return safeComponent(identity) + ".sav";
}

std::string SaveOutputPath::safeComponent(std::string value)
{
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!(std::isalnum(byte) || character == '-' || character == '_' ||
              character == '.')) {
            character = '_';
        }
    }
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "gba_save" : value;
}

} // namespace ezfadvance
