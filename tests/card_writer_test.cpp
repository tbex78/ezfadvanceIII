#include "ezfadvance/card_writer.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeBackend final : public ezfadvance::WriterBackend {
public:
    std::vector<std::string> calls;
    std::string fail_at;
    unsigned fail_on_occurrence = 1;
    unsigned matching_calls = 0;

    bool call(const char* name) {
        calls.emplace_back(name);
        if (fail_at != name) return true;
        ++matching_calls;
        return matching_calls != fail_on_occurrence;
    }

    bool preflight() override { return call("preflight"); }
    bool initializeBridge() override { return call("initialize"); }
    bool prepareGlobalWrite() override { return call("prepare"); }
    bool selectWindowZeroForErase() override { return call("erase-window"); }
    bool erase(std::size_t) override { return call("erase"); }
    bool finalizeFlashState() override { return call("finalize"); }
    bool selectWindowZeroForProgram() override { return call("program-window"); }
    bool program(const std::vector<std::uint8_t>&) override { return call("program"); }
    bool verifyPartialFirstWindow(const std::vector<std::uint8_t>&) override { return call("verify-partial-first"); }
    bool verifyExact8MiB(const std::vector<std::uint8_t>&) override { return call("verify-8"); }
    bool verifyPartial12MiB(const std::vector<std::uint8_t>&) override { return call("verify-12"); }
    bool verifyExact16MiB(const std::vector<std::uint8_t>&) override { return call("verify-16"); }
    bool verifyTinyTailAbove16MiB(const std::vector<std::uint8_t>&) override { return call("verify-tiny-tail"); }
    bool verifyPartial20MiB(const std::vector<std::uint8_t>&) override { return call("verify-20"); }
    bool verifyExact24MiB(const std::vector<std::uint8_t>&) override { return call("verify-24"); }
    bool verifyPartial28MiB(const std::vector<std::uint8_t>&) override { return call("verify-28"); }
    bool verifyExact32MiB(const std::vector<std::uint8_t>&) override { return call("verify-32"); }
};

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void expect_calls(const FakeBackend& backend,
                  std::initializer_list<const char*> expected)
{
    std::vector<std::string> wanted;
    for (const char* item : expected) wanted.emplace_back(item);
    expect(backend.calls == wanted, "backend call sequence differs");
}

const std::initializer_list<const char*> write_prefix = {
    "preflight", "initialize", "prepare", "erase-window", "erase",
    "finalize", "program-window", "program"
};

void expect_dispatch(std::size_t image_size, const char* verification_call)
{
    FakeBackend backend;
    std::ostringstream report;
    const auto result = ezfadvance::CardWriter(backend).write(
        std::vector<std::uint8_t>(image_size), false, report);
    expect(static_cast<bool>(result), "verification dispatch should succeed");
    expect(result.verification_completed, "verification should be recorded");
    std::vector<std::string> wanted;
    for (const char* item : write_prefix) wanted.emplace_back(item);
    wanted.emplace_back(verification_call);
    expect(backend.calls == wanted, "verification mode dispatched incorrectly");
}

void expect_failure(const char* failing_call,
                    std::initializer_list<const char*> expected)
{
    FakeBackend backend;
    backend.fail_at = failing_call;
    std::ostringstream report;
    const auto result = ezfadvance::CardWriter(backend).write({1}, false, report);
    expect(result.status == ezfadvance::CardWriteStatus::operation_failed,
           "operation failure status");
    expect_calls(backend, expected);
}

} // namespace

int main()
{
    expect_dispatch(1, "verify-partial-first");
    expect_dispatch(0x800000, "verify-8");
    expect_dispatch(0xC00000, "verify-12");
    expect_dispatch(0x1000000, "verify-16");
    expect_dispatch(0x1000001, "verify-tiny-tail");
    expect_dispatch(0x1400000, "verify-20");
    expect_dispatch(0x1800000, "verify-24");
    expect_dispatch(0x1C00000, "verify-28");
    expect_dispatch(0x2000000, "verify-32");

    {
        FakeBackend backend;
        backend.fail_at = "preflight";
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write({1}, false, report);
        expect(result.status == ezfadvance::CardWriteStatus::preflight_failed,
               "preflight failure status");
        expect_calls(backend, {"preflight"});
    }
    expect_failure("initialize", {"preflight", "initialize"});
    expect_failure("prepare", {"preflight", "initialize", "prepare"});
    expect_failure("erase-window", {"preflight", "initialize", "prepare",
                                     "erase-window"});
    expect_failure("erase", {"preflight", "initialize", "prepare",
                              "erase-window", "erase"});
    expect_failure("finalize", {"preflight", "initialize", "prepare",
                                 "erase-window", "erase", "finalize"});
    expect_failure("program-window", {"preflight", "initialize", "prepare",
                                       "erase-window", "erase", "finalize",
                                       "program-window"});
    expect_failure("program", {"preflight", "initialize", "prepare",
                                "erase-window", "erase", "finalize",
                                "program-window", "program"});
    expect_failure("verify-partial-first", {"preflight", "initialize", "prepare",
                                             "erase-window", "erase", "finalize",
                                             "program-window", "program",
                                             "verify-partial-first"});
    {
        FakeBackend backend;
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write({1}, true, report);
        expect(result.verification_skipped_by_user, "skip-verify cleanup should succeed");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window",
                               "erase", "finalize", "program-window", "program", "finalize"});
    }
    {
        FakeBackend backend;
        backend.fail_at = "finalize";
        backend.fail_on_occurrence = 2;
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write({1}, true, report);
        expect(result.status == ezfadvance::CardWriteStatus::operation_failed,
               "skip-verify cleanup failure status");
        expect(!result.verification_skipped_by_user,
               "failed cleanup must not report successful skip");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window",
                               "erase", "finalize", "program-window", "program",
                               "finalize"});
    }
    {
        FakeBackend backend;
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write(
            std::vector<std::uint8_t>(0x900000), false, report);
        expect(result.verification_skipped_unproven, "unproven geometry should be reported");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window",
                               "erase", "finalize", "program-window", "program",
                               "finalize"});
    }
    {
        FakeBackend backend;
        backend.fail_at = "finalize";
        backend.fail_on_occurrence = 2;
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write(
            std::vector<std::uint8_t>(0x900000), false, report);
        expect(result.status == ezfadvance::CardWriteStatus::operation_failed,
               "unsupported-geometry cleanup failure status");
        expect(!result.verification_skipped_unproven,
               "failed cleanup must not report unsupported skip");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window",
                               "erase", "finalize", "program-window", "program",
                               "finalize"});
    }

    std::cout << "card writer tests passed\n";
    return 0;
}
