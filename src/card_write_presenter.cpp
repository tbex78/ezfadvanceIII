#include "ezfadvance/card_write_presenter.hpp"

#include <ostream>

namespace ezfadvance {

void printCardWriteSummary(const CardWriteResult& result,
                           std::ostream& output)
{
    output << "\n========================================\n";
    if (!result) {
        output << "WRITE/FINALIZE FAILED.\n"
               << "Please unplug and reconnect the EZF Advance III, "
                  "then try again.\n";
    } else if (result.verification_skipped_by_user) {
        output << "WRITE SUCCEEDED; READ-BACK VERIFICATION SKIPPED BY REQUEST.\n";
    } else if (result.verification_skipped_unproven) {
        output << "WRITE SUCCEEDED; FULL READ-BACK VERIFICATION SKIPPED.\n";
    } else if (result.verification_completed) {
        output << "WRITE + FULL READ-BACK VERIFICATION SUCCEEDED.\n";
    } else {
        output << "WRITE SUCCEEDED.\n";
    }
    output << "========================================\n";
}

} // namespace ezfadvance
