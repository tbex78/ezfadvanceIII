#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

struct WriterParseResult {
    bool ok = false;
    bool show_usage = false;
};

class WriterOptions final {
public:
    static constexpr std::size_t catalog_slots = 120;

    static WriterParseResult parse(int argc, char** argv,
                                   WriterOptions& options,
                                   std::ostream& errors);

    bool write = false;
    bool skip_verify = false;
    bool verbose = false;
    bool experimental_clean_start = false;
    std::array<std::optional<std::uint8_t>, catalog_slots> type_overrides{};
    std::array<std::optional<std::uint8_t>, catalog_slots> mapping_overrides{};
    std::array<std::optional<std::string>, catalog_slots> title_overrides{};
    std::vector<std::string> rom_paths;
};

} // namespace ezfadvance
