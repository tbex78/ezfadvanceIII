#pragma once

#include "ezfadvance/protocol.hpp"

#include <cstdint>
#include <string>

namespace ezfadvance {

// Capture-derived selector transition shared by save reads and writes.
class SaveBankAccess final {
public:
    explicit SaveBankAccess(Transport& transport) noexcept
        : protocol_(transport) {}

    bool select(std::uint16_t bank_value,
                const std::string& label_prefix = "savebank") const;

private:
    Protocol protocol_;
};

} // namespace ezfadvance
