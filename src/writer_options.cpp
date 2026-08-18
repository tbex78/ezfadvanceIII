#include "ezfadvance/writer_options.hpp"

#include <limits>
#include <ostream>
#include <string_view>

namespace ezfadvance {

namespace {

bool parseDecimal(std::string_view text, std::size_t maximum,
                  std::size_t& value)
{
    if (text.empty()) return false;

    std::size_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

} // namespace

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
            std::size_t slot = 0;
            if (!parseDecimal(
                    std::string_view(argument).substr(6, equals - 6),
                    catalog_slots, slot) ||
                slot == 0) {
                errors << "Bad type override slot: " << argument
                       << " (valid structural catalog slots are 1.."
                       << catalog_slots << ")\n";
                return {false, false};
            }
            std::size_t value = 0;
            if (!parseDecimal(
                    std::string_view(argument).substr(equals + 1),
                    std::numeric_limits<std::uint8_t>::max(), value)) {
                errors << "Bad type override value: " << argument << '\n';
                return {false, false};
            }
            options.type_overrides[slot - 1] = static_cast<std::uint8_t>(value);
        } else if (argument.rfind("--map", 0) == 0) {
            const auto equals = argument.find('=');
            if (equals == std::string::npos || equals <= 5 ||
                equals + 1 >= argument.size()) return {false, true};
            std::size_t slot = 0;
            if (!parseDecimal(
                    std::string_view(argument).substr(5, equals - 5),
                    catalog_slots, slot) ||
                slot == 0) {
                errors << "Bad mapping override slot: " << argument
                       << " (valid structural catalog slots are 1.."
                       << catalog_slots << ")\n";
                return {false, false};
            }
            std::size_t value = 0;
            if (!parseDecimal(
                    std::string_view(argument).substr(equals + 1),
                    std::numeric_limits<std::uint8_t>::max(), value)) {
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
