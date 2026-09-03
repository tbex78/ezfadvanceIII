#include "ezfadvance/rom_input_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <istream>
#include <iterator>
#include <ostream>

#include "ezfadvance/rom_analyzer.hpp"

namespace ezfadvance {

RomInputLoader::RomInputLoader(std::istream& input,
                               std::ostream& output,
                               std::ostream& error)
    : input_(input), output_(output), error_(error)
{
}

std::string RomInputLoader::deriveCatalogName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string stem =
        (slash == std::string::npos) ? path : path.substr(slash + 1);

    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0)
        stem.resize(dot);

    std::string name;
    for (unsigned char character : stem) {
        if (name.size() >= 16) break;
        if (character >= 0x20 && character <= 0x7e)
            name.push_back(static_cast<char>(character));
        else
            name.push_back('_');
    }

    return name.empty() ? "ROM" : name;
}

bool RomInputLoader::confirmNonSramSave(const RomInfo& rom,
                                        const std::string& signature,
                                        const std::string& family) const
{
    error_
        << "\n========================================\n"
        << "WARNING: NON-SRAM SAVE FORMAT\n"
        << "========================================\n"
        << "ROM: " << rom.path << '\n'
        << "Detected save library: " << signature << '\n'
        << "Embedded save-library signature family: " << family << "\n\n"
        << "This marker normally identifies a non-SRAM GBA save library. However,\n"
        << "manual SRAM patching can leave the original FLASH/EEPROM signature in\n"
        << "the ROM, so the marker does NOT prove the active runtime save method.\n\n"
        << "This writer NEVER patches or converts ROM save routines automatically.\n"
        << "If SRAM conversion is needed, do it manually with a separate save-patching\n"
        << "tool BEFORE running this writer. If the ROM is already SRAM-patched, this\n"
        << "signature warning may be safely expected.\n"
        << "========================================\n"
        << "Continue anyway? [y/N]: " << std::flush;

    std::string answer;
    if (!std::getline(input_, answer)) {
        error_ << "\nNo response received; aborting safely.\n";
        return false;
    }

    const auto first = answer.find_first_not_of(" \t\r\n");
    const auto last = answer.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        error_ << "No selected; aborting safely.\n";
        return false;
    }

    std::string normalized = answer.substr(first, last - first + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    if (normalized == "y" || normalized == "yes") {
        error_ << "Yes selected; continuing.\n";
        return true;
    }

    error_ << "No selected; aborting safely.\n";
    return false;
}

bool RomInputLoader::load(RomInfo& rom) const
{
    std::ifstream file(rom.path, std::ios::binary);
    if (!file) {
        error_ << "Cannot open ROM: " << rom.path << '\n';
        return false;
    }

    rom.data.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());
    if (rom.data.empty()) {
        error_ << "ROM is empty: " << rom.path << '\n';
        return false;
    }

    const auto analysis = RomAnalyzer{}.analyze(rom.data);
    if (!analysis.original_entry_target) {
        error_ << "ROM does not begin with the capture-supported "
                  "EAxxxxxx ARM branch: " << rom.path << '\n';
        return false;
    }

    if (analysis.non_sram_save &&
        !confirmNonSramSave(rom,
                            analysis.non_sram_save->signature,
                            analysis.non_sram_save->family)) {
        error_ << "ROM processing cancelled before image construction; "
                  "no USB write was attempted.\n";
        return false;
    }

    rom.original_entry_target = *analysis.original_entry_target;
    if (rom.name.empty()) rom.name = deriveCatalogName(rom.path);
    if (!rom.entry_type_overridden) rom.entry_type = analysis.entry_type;

    if (rom.mapping_flag_overridden)
        return true;

    rom.mapping_flag = analysis.mapping_flag;
    if (!analysis.has_eeprom_library)
        return true;

    if (rom.mapping_flag == 0) {
        error_
            << "EEPROM capacity could not be proven from ROM structure: "
            << rom.path << '\n'
            << "Catalog map 4 requires 4-Kbit/512-byte EEPROM; map 5 "
               "requires 64-Kbit/8-KiB EEPROM.\n"
            << "Specify the evidence-appropriate value explicitly with "
               "--mapN=4 or --mapN=5 for this ROM slot.\n"
            << "No USB write was attempted.\n";
        return false;
    }

    output_ << "EEPROM SDK initialization selects "
            << (rom.mapping_flag == 4 ? "4-Kbit / 512-byte" :
                                        "64-Kbit / 8-KiB")
            << " capacity; using catalog map "
            << static_cast<unsigned>(rom.mapping_flag) << ".\n";
    return true;
}

} // namespace ezfadvance
