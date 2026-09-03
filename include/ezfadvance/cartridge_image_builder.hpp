#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ezfadvance {

struct RomInfo {
    std::string path;
    std::string name;
    std::vector<std::uint8_t> data;
    std::uint32_t start = 0;
    std::uint32_t original_entry_target = 0;
    std::uint8_t entry_type = 9;
    bool entry_type_overridden = false;
    std::uint8_t mapping_flag = 3;
    bool mapping_flag_overridden = false;
};

struct BuiltCartridgeImage {
    std::vector<std::uint8_t> bytes;
    std::size_t programmed_size = 0;
    std::string report;
};

struct CartridgeImageBuildOptions {
    bool use_multi_loader_for_single_rom = false;
};

class CartridgeImageBuilder final {
public:
    static constexpr std::size_t catalog_slots = 120;

    bool build(std::vector<RomInfo>& roms,
               BuiltCartridgeImage& result,
               std::string& error,
               const CartridgeImageBuildOptions& options = {}) const;
};

} // namespace ezfadvance
