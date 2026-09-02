#include "ezfadvance/card_reader_options.hpp"

#include <stdexcept>

namespace ezfadvance {

CardReaderParseStatus parseCardReaderOptions(
    const std::vector<std::string>& arguments,
    CardReaderOptions& options,
    std::ostream& errors)
{
    options = {};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--extract" || argument == "--dump") {
            if (options.extract || i + 1 >= arguments.size()) {
                errors << "Invalid or incomplete " << argument << " option.\n";
                return CardReaderParseStatus::error;
            }
            options.extract = true;
            const std::string first_value = arguments[++i];
            if (i + 1 < arguments.size() &&
                arguments[i + 1].rfind("--", 0) != 0) {
                try {
                    std::size_t consumed = 0;
                    const unsigned long parsed =
                        std::stoul(first_value, &consumed, 10);
                    if (consumed != first_value.size() || parsed == 0)
                        throw std::invalid_argument("bad ROM number");
                    options.rom_number = static_cast<std::size_t>(parsed);
                    options.output_path = arguments[++i];
                } catch (const std::exception&) {
                    errors << "Bad ROM number for EZ3 " << argument << ".\n";
                    return CardReaderParseStatus::error;
                }
            } else {
                options.output_path = first_value;
            }
            if (options.output_path.empty()) {
                errors << "Extraction output path is empty.\n";
                return CardReaderParseStatus::error;
            }
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--help" || argument == "-h") {
            return CardReaderParseStatus::help;
        } else {
            errors << "Unknown option: " << argument << '\n';
            return CardReaderParseStatus::error;
        }
    }
    if (options.verbose && !options.extract) {
        errors << "--verbose is currently available with extraction only.\n";
        return CardReaderParseStatus::error;
    }
    return CardReaderParseStatus::run;
}

CardReaderDecision decideCardReaderAction(
    CartridgeKind kind,
    const CardReaderOptions& options)
{
    if (kind == CartridgeKind::official_gba_rom) {
        if (options.rom_number) {
            return {CardReaderAction::reject, 0,
                    "Numbered extraction requires an EZ3 flash cartridge."};
        }
        return {options.extract ? CardReaderAction::extract_official
                                : CardReaderAction::inspect_official,
                0, {}};
    }
    if (kind == CartridgeKind::ez3_flash) {
        if (!options.extract)
            return {CardReaderAction::inspect_ez3, 0, {}};
        return {CardReaderAction::extract_ez3,
                options.rom_number.value_or(1), {}};
    }
    return {CardReaderAction::reject, 0,
            "Unknown cartridge kind cannot be inspected or extracted."};
}

} // namespace ezfadvance
