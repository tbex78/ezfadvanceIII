#pragma once

#include <iosfwd>
#include <string>

#include "ezfadvance/cartridge_image_builder.hpp"

namespace ezfadvance {

class RomInputLoader final {
public:
    RomInputLoader(std::istream& input, std::ostream& output, std::ostream& error);

    bool load(RomInfo& rom) const;

    static std::string deriveCatalogName(const std::string& path);

private:
    bool confirmNonSramSave(const RomInfo& rom,
                            const std::string& signature,
                            const std::string& family) const;

    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
};

} // namespace ezfadvance
