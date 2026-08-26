#include "ezfadvance/card_writer.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeBackend final : public ezfadvance::WriterBackend {
public:
    std::vector<std::string> calls;
    std::string fail_at;

    bool call(const char* name) {
        calls.emplace_back(name);
        return fail_at != name;
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

} // namespace

int main()
{
    {
        FakeBackend backend;
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write(
            std::vector<std::uint8_t>(0x800000), false, report);
        expect(static_cast<bool>(result), "exact-8-MiB write should succeed");
        expect(result.verification_completed, "verification should be recorded");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window",
                               "erase", "finalize", "program-window", "program",
                               "verify-8"});
    }
    {
        FakeBackend backend;
        backend.fail_at = "preflight";
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write({1}, false, report);
        expect(result.status == ezfadvance::CardWriteStatus::preflight_failed,
               "preflight failure status");
        expect_calls(backend, {"preflight"});
    }
    {
        FakeBackend backend;
        backend.fail_at = "erase";
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write({1}, false, report);
        expect(result.status == ezfadvance::CardWriteStatus::operation_failed,
               "erase failure status");
        expect_calls(backend, {"preflight", "initialize", "prepare", "erase-window", "erase"});
    }
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
        std::ostringstream report;
        const auto result = ezfadvance::CardWriter(backend).write(
            std::vector<std::uint8_t>(0x900000), false, report);
        expect(result.verification_skipped_unproven, "unproven geometry should be reported");
        expect(backend.calls.back() == "finalize", "unproven geometry must finish flash state");
    }

    std::cout << "card writer tests passed\n";
    return 0;
}
