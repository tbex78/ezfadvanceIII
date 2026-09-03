#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

#include "ezfadvance/cartridge_image_builder.hpp"

namespace ezfadvance {

class CartridgeLayoutPresenter final {
public:
    explicit CartridgeLayoutPresenter(std::ostream& output);

    void print(const std::vector<RomInfo>& roms,
               const std::vector<std::uint8_t>& image,
               std::size_t programmed_size) const;

private:
    std::ostream& output_;
};

} // namespace ezfadvance
