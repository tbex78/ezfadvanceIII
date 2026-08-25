#pragma once

#include "ezfadvance/read_only_cartridge.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ezfadvance {

struct CardReaderOptions {
    bool extract = false;
    bool verbose = false;
    std::optional<std::size_t> rom_number;
    std::string output_path;
};

enum class CardReaderParseStatus { run, help, error };

CardReaderParseStatus parseCardReaderOptions(
    const std::vector<std::string>& arguments,
    CardReaderOptions& options,
    std::ostream& errors);

enum class CardReaderAction {
    inspect_official,
    extract_official,
    inspect_ez3,
    extract_ez3,
    reject
};

struct CardReaderDecision {
    CardReaderAction action = CardReaderAction::reject;
    std::size_t rom_number = 0;
    std::string error;
};

CardReaderDecision decideCardReaderAction(
    CartridgeKind kind,
    const CardReaderOptions& options);

} // namespace ezfadvance
