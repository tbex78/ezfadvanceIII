#include "ezfadvance/card_write_presenter.hpp"

#include <cassert>
#include <sstream>
#include <string>

namespace {

std::string render(const ezfadvance::CardWriteResult& result)
{
    std::ostringstream output;
    ezfadvance::printCardWriteSummary(result, output);
    return output.str();
}

void testFailureProposesReconnectAndRetry()
{
    const ezfadvance::CardWriteResult result;
    const auto output = render(result);
    assert(output.find("WRITE/FINALIZE FAILED.") != std::string::npos);
    assert(output.find("unplug and reconnect") != std::string::npos);
    assert(output.find("try again") != std::string::npos);
}

void testSuccessDoesNotProposeReconnect()
{
    ezfadvance::CardWriteResult result;
    result.status = ezfadvance::CardWriteStatus::success;
    result.verification_completed = true;
    const auto output = render(result);
    assert(output.find("WRITE + FULL READ-BACK VERIFICATION SUCCEEDED.") !=
           std::string::npos);
    assert(output.find("unplug") == std::string::npos);
}

} // namespace

int main()
{
    testFailureProposesReconnectAndRetry();
    testSuccessDoesNotProposeReconnect();
}
