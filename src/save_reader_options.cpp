#include "ezfadvance/save_reader_options.hpp"

#include "ezfadvance/save_selection.hpp"

#include <charconv>
#include <string_view>

namespace ezfadvance {
namespace {

std::optional<std::size_t> parsePositiveNumber(std::string_view text) noexcept
{
    std::size_t value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || value == 0)
        return std::nullopt;
    return value;
}

} // namespace

SaveReaderParseStatus parseSaveReaderOptions(
    const std::vector<std::string>& arguments,
    SaveReaderOptions& options,
    std::ostream& errors)
{
    options = {};
    options.inspection_only = arguments.empty();

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto value = [&]() -> const std::string* {
            if (i + 1 >= arguments.size()) return nullptr;
            return &arguments[++i];
        };

        if (argument == "--output") {
            const auto* path = value();
            if (!path) return SaveReaderParseStatus::error;
            options.output_path = *path;
        } else if (argument == "--rom") {
            const auto* number = value();
            const auto parsed = number
                ? parsePositiveNumber(*number)
                : std::optional<std::size_t>{};
            if (!parsed) {
                errors << "Bad --rom value.\n";
                return SaveReaderParseStatus::error;
            }
            options.rom_number = *parsed;
        } else if (argument == "--write") {
            const auto* path = value();
            if (!path) return SaveReaderParseStatus::error;
            options.write_path = *path;
        } else if (argument == "--save-bank") {
            const auto* selector = value();
            options.save_bank = selector
                ? SaveBankSelector::parse(*selector)
                : std::optional<SaveBankSelector>{};
            if (!options.save_bank) {
                errors << "Bad --save-bank value; expected 0x0900, 0x0910, "
                          "0x0920, or 0x0930.\n";
                return SaveReaderParseStatus::error;
            }
        } else if (argument == "--consecutive-bank") {
            const auto* count = value();
            options.consecutive_bank_count = count
                ? parseConsecutiveBankCount(*count)
                : std::optional<std::size_t>{};
            if (!options.consecutive_bank_count) {
                errors << "Bad --consecutive-bank value; expected 1 to 4.\n";
                return SaveReaderParseStatus::error;
            }
        } else if (argument == "--backup") {
            const auto* path = value();
            if (!path) return SaveReaderParseStatus::error;
            options.backup_path = *path;
        } else if (argument == "--yes-really-write") {
            options.authorize_write = true;
        } else if (argument == "--erase") {
            options.erase = true;
        } else if (argument == "--help" || argument == "-h") {
            return SaveReaderParseStatus::help;
        } else {
            errors << "Unknown option: " << argument << '\n';
            return SaveReaderParseStatus::error;
        }
    }

    if (options.write_path && options.output_path) {
        errors << "--output is for extraction and cannot be combined with "
                  "--write.\n";
        return SaveReaderParseStatus::error;
    }
    if (options.erase &&
        (options.write_path || options.output_path || options.rom_number ||
         options.authorize_write)) {
        errors << "--erase cannot be combined with --write, --output, --rom, "
                  "or --yes-really-write.\n";
        return SaveReaderParseStatus::error;
    }
    if (options.consecutive_bank_count && !options.save_bank) {
        errors << "--consecutive-bank requires --save-bank.\n";
        return SaveReaderParseStatus::error;
    }
    if (options.consecutive_bank_count && options.rom_number) {
        errors << "--consecutive-bank is for direct bank access and cannot be "
                  "combined with --rom.\n";
        return SaveReaderParseStatus::error;
    }
    if (options.consecutive_bank_count &&
        !options.save_bank->accommodates(
            *options.consecutive_bank_count * SaveBankSelector::bank_size)) {
        errors << "The requested consecutive-bank range extends beyond save "
                  "bank 0x0930.\n";
        return SaveReaderParseStatus::error;
    }
    if (!options.write_path && !options.erase &&
        (options.backup_path || options.authorize_write)) {
        errors << "--backup and --yes-really-write require --write FILE.\n";
        return SaveReaderParseStatus::error;
    }
    if (options.write_path && !options.authorize_write) {
        errors << "Save writing requires --yes-really-write.\n";
        return SaveReaderParseStatus::error;
    }
    if (saveWritePathsConflict(options.write_path, options.backup_path)) {
        errors << "The input save and backup paths must be different.\n";
        return SaveReaderParseStatus::error;
    }

    return SaveReaderParseStatus::run;
}

} // namespace ezfadvance
