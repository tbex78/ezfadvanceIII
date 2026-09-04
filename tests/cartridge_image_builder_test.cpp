#include "ezfadvance/cartridge_image_builder.hpp"
#include "ezfadvance/cartridge_format.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

ezfadvance::RomInfo rom(std::string name, std::size_t size,
                        std::uint8_t fill, std::uint8_t type)
{
    ezfadvance::RomInfo value;
    value.path = name + ".gba";
    value.name = std::move(name);
    value.data.assign(size, fill);
    value.original_entry_target = 0xD4;
    value.entry_type = type;
    value.mapping_flag = 3;
    return value;
}

std::uint64_t hash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value;
}

void verify(std::vector<ezfadvance::RomInfo> roms,
            std::size_t expected_size,
            std::uint64_t expected_hash,
            const std::vector<std::uint32_t>& expected_starts,
            const ezfadvance::CartridgeImageBuildOptions& options = {})
{
    ezfadvance::BuiltCartridgeImage image;
    std::string error;
    const bool ok = ezfadvance::CartridgeImageBuilder{}.build(
        roms, image, error, options);
    if (!ok) {
        std::cerr << error << '\n';
        std::abort();
    }
    assert(image.programmed_size == expected_size);
    assert(image.bytes.size() == expected_size);
    assert(hash(image.bytes) == expected_hash);
    assert(roms.size() == expected_starts.size());
    for (std::size_t index = 0; index < roms.size(); ++index)
        assert(roms[index].start == expected_starts[index]);
}

} // namespace

int main()
{
    auto single = rom("Single", 0x10000, 0xFF, 9);
    for (std::size_t i = 0; i < 0x200; ++i) single.data[i] = 0x11;
    verify({single}, 65536, 0xfb860f668ace9a0bull, {0});

    ezfadvance::BuiltCartridgeImage experimental_image;
    std::string experimental_error;
    std::vector experimental_roms{single};
    ezfadvance::CartridgeImageBuildOptions experimental_options;
    experimental_options.direct_boot_single_rom = true;
    const bool experimental_ok = ezfadvance::CartridgeImageBuilder{}.build(
        experimental_roms, experimental_image, experimental_error,
        experimental_options);
    assert(experimental_ok);
    assert(experimental_error.empty());
    assert(experimental_roms[0].start == 0);
    assert(experimental_image.report.find(
               "Experimental direct-boot image") != std::string::npos);
    assert(experimental_image.programmed_size == 0x10000);
    assert(experimental_image.bytes.size() == 0x10000);
    assert(experimental_image.bytes == single.data);

    verify({
        rom("Large", 0x100000, 0x21, 5),
        rom("Small", 0x10000, 0x42, 9)},
        1142784, 0x02a43fb2ee37f13dull, {0, 1048576});

    verify({
        rom("TwoMiB", 0x200000, 0x31, 4),
        rom("OneMiB", 0x100000, 0x52, 5),
        rom("Tiny", 0x10000, 0x73, 9)},
        3240192, 0xfaea9cb9bdb454faull, {0, 2097152, 3145728});

    std::vector<ezfadvance::RomInfo> eight;
    for (unsigned index = 0; index < 8; ++index)
        eight.push_back(rom("R" + std::to_string(index + 1),
                            0x10000, static_cast<std::uint8_t>(0x80 + index), 9));
    verify(std::move(eight), 553216, 0xda12fcefa1437053ull,
           {0, 65536, 131072, 196608, 262144, 327680, 393216, 458752});

    auto internal = rom("Internal", 0x1000000, 0x5A, 1);
    std::fill(internal.data.begin() + 0x200000,
              internal.data.begin() + 0x208000, 0xFF);
    verify({std::move(internal)}, 16777216, 0x600b5a10da4331c7ull, {0});
}
