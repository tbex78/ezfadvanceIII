#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace ezfadvance {

class WriterBackend {
public:
    virtual ~WriterBackend() = default;
    virtual bool preflight() = 0;
    virtual bool initializeBridge() = 0;
    virtual bool prepareGlobalWrite() = 0;
    virtual bool selectWindowZeroForErase() = 0;
    virtual bool erase(std::size_t image_size) = 0;
    virtual bool finalizeFlashState() = 0;
    virtual bool selectWindowZeroForProgram() = 0;
    virtual bool program(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyPartialFirstWindow(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyExact8MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyPartial12MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyExact16MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyTinyTailAbove16MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyPartial20MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyExact24MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyPartial28MiB(const std::vector<std::uint8_t>& image) = 0;
    virtual bool verifyExact32MiB(const std::vector<std::uint8_t>& image) = 0;
};

enum class CardWriteStatus {
    success,
    preflight_failed,
    operation_failed
};

struct CardWriteResult {
    CardWriteStatus status = CardWriteStatus::operation_failed;
    bool verification_completed = false;
    bool verification_skipped_by_user = false;
    bool verification_skipped_unproven = false;

    explicit operator bool() const noexcept { return status == CardWriteStatus::success; }
};

class CardWriter final {
public:
    explicit CardWriter(WriterBackend& backend) noexcept : backend_(backend) {}

    CardWriteResult write(const std::vector<std::uint8_t>& image,
                          bool skip_verification,
                          std::ostream& report) const;

private:
    WriterBackend& backend_;
};

} // namespace ezfadvance
