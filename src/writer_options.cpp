#include "ezfadvance/writer_options.hpp"

#include <ostream>

namespace ezfadvance {

WriterParseResult WriterOptions::parse(int argc, char** argv,
                                       WriterOptions& options,
                                       std::ostream& errors)
{
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--yes-really-write") {
            options.write = true;
        } else if (argument == "--skip-verify") {
            options.skip_verify = true;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument.rfind("--type", 0) == 0) {
            const auto equals = argument.find('=');
            if (equals == std::string::npos || equals <= 6 ||
                equals + 1 >= argument.size()) return {false, true};
            const auto slot = static_cast<std::size_t>(
                std::stoul(argument.substr(6, equals - 6)));
            if (slot == 0 || slot > catalog_slots) {
                errors << "Bad type override slot: " << argument
                       << " (valid structural catalog slots are 1.."
                       << catalog_slots << ")\n";
                return {false, false};
            }
            const int value = std::stoi(argument.substr(equals + 1));
            if (value < 0 || value > 255) {
                errors << "Bad type override value: " << argument << '\n';
                return {false, false};
            }
            options.type_overrides[slot - 1] = static_cast<std::uint8_t>(value);
        } else if (argument.rfind("--map", 0) == 0) {
            const auto equals = argument.find('=');
            if (equals == std::string::npos || equals <= 5 ||
                equals + 1 >= argument.size()) return {false, true};
            const auto slot = static_cast<std::size_t>(
                std::stoul(argument.substr(5, equals - 5)));
            if (slot == 0 || slot > catalog_slots) {
                errors << "Bad mapping override slot: " << argument
                       << " (valid structural catalog slots are 1.."
                       << catalog_slots << ")\n";
                return {false, false};
            }
            const int value = std::stoi(argument.substr(equals + 1));
            if (value < 0 || value > 255) {
                errors << "Bad mapping override value: " << argument << '\n';
                return {false, false};
            }
            options.mapping_overrides[slot - 1] =
                static_cast<std::uint8_t>(value);
        } else if (!argument.empty() && argument[0] == '-') {
            errors << "Unknown option: " << argument << '\n';
            return {false, true};
        } else {
            options.rom_paths.push_back(argument);
        }
    }
    return {true, false};
}

} // namespace ezfadvance
