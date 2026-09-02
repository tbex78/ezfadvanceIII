#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ezfadvance {

class SaveFileStore final {
public:
    bool write(const std::string& path,
               const std::vector<std::uint8_t>& data,
               std::ostream& errors) const;
    bool writeNew(const std::string& path,
                  const std::vector<std::uint8_t>& data,
                  std::ostream& errors) const;
};

} // namespace ezfadvance
