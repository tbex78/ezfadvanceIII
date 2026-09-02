#pragma once

#include "ezfadvance/save_bank_selector.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ezfadvance {

struct SaveReaderOptions {
    std::optional<std::string> output_path;
    std::optional<std::size_t> rom_number;
    std::optional<SaveBankSelector> save_bank;
    std::optional<std::size_t> consecutive_bank_count;
    std::optional<std::string> write_path;
    std::optional<std::string> backup_path;
    bool authorize_write = false;
    bool erase = false;
    bool inspection_only = true;
};

enum class SaveReaderParseStatus { run, help, error };

SaveReaderParseStatus parseSaveReaderOptions(
    const std::vector<std::string>& arguments,
    SaveReaderOptions& options,
    std::ostream& errors);

} // namespace ezfadvance
