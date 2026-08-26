#include "ezfadvance/card_writer.hpp"

#include "ezfadvance/verification_policy.hpp"

#include <iomanip>
#include <ostream>

namespace ezfadvance {

CardWriteResult CardWriter::write(const std::vector<std::uint8_t>& image,
                                  bool skip_verification,
                                  std::ostream& report) const
{
    CardWriteResult result;
    if (!backend_.preflight()) {
        result.status = CardWriteStatus::preflight_failed;
        return result;
    }

    report << "\nSimulating close-manager -> launch-v19 transition...\n";
    bool ok = backend_.initializeBridge();

    report << "\n========================================\n"
           << "MANAGER-PRIMED FULL WRITE\n"
           << "========================================\n"
           << "Manager-prime/write transition originally proven on macOS / Apple Silicon.\n"
           << "Flash-window pre-AA55 settle: fixed 125 ms.\n"
           << "ROM payload: one Windows-style 64-KiB BULK OUT request.\n";
    if (ok) ok = backend_.prepareGlobalWrite();
    if (ok) ok = backend_.selectWindowZeroForErase();
    if (ok) ok = backend_.erase(image.size());
    if (ok) ok = backend_.finalizeFlashState();
    if (ok) ok = backend_.selectWindowZeroForProgram();
    if (ok) ok = backend_.program(image);

    if (ok && skip_verification) {
        report << "\n--skip-verify supplied: skipping post-write ROM read-back "
                  "verification.\n"
               << "Sending non-readback flash status/reset cleanup only.\n";
        ok = backend_.finalizeFlashState();
        result.verification_skipped_by_user = ok;
    } else if (ok) {
        switch (VerificationPolicy{}.modeFor(image.size())) {
        case VerificationMode::exact_32_mib:
            ok = backend_.verifyExact32MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::partial_28_mib:
            ok = backend_.verifyPartial28MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::exact_24_mib:
            ok = backend_.verifyExact24MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::partial_20_mib:
            ok = backend_.verifyPartial20MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::exact_16_mib:
            ok = backend_.verifyExact16MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::partial_12_mib:
            ok = backend_.verifyPartial12MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::exact_8_mib:
            ok = backend_.verifyExact8MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::partial_first_window:
            ok = backend_.verifyPartialFirstWindow(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::tiny_tail_above_16_mib:
            ok = backend_.verifyTinyTailAbove16MiB(image);
            result.verification_completed = ok;
            break;
        case VerificationMode::unsupported_partial_higher_window:
            ok = backend_.finalizeFlashState();
            if (ok) {
                result.verification_skipped_unproven = true;
                report << "\nFull read-back verification skipped: image extent 0x"
                       << std::hex << image.size() << std::dec
                       << " is a partial higher-window geometry with no "
                          "capture-proven linear read mapping yet.\n"
                       << "Capture-proven full verification currently covers "
                          "all images below 8 MiB, exact 8/16/24/32 MiB, "
                          "partial 12/20/28 MiB, and "
                          "the dedicated tiny-tail-above-16-MiB case.\n"
                       << "Programming completed; no experimental verification "
                          "window selection was sent.\n";
            }
            break;
        }
    }

    result.status = ok ? CardWriteStatus::success : CardWriteStatus::operation_failed;
    return result;
}

} // namespace ezfadvance
