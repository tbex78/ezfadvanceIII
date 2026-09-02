#pragma once

#include "ezfadvance/protocol.hpp"
#include "ezfadvance/save_bank_access.hpp"

#include <iosfwd>
#include <cstddef>
#include <cstdint>

namespace ezfadvance {

// Capture-derived cleanup of the complete four-bank EZ3 save area.
class SaveBankCleaner final {
public:
    explicit SaveBankCleaner(Transport& transport) noexcept
        : bank_access_(transport), protocol_(transport) {}

    bool clearAll(std::ostream& output, bool report_progress = true) const;
    bool clearRange(std::uint16_t first_selector,
                    std::size_t bank_count,
                    std::ostream& output,
                    bool report_progress = true) const;

private:
    SaveBankAccess bank_access_;
    Protocol protocol_;
};

} // namespace ezfadvance
