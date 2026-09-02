#pragma once

#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ezfadvance {

enum class CartridgeKind {
    unknown,
    ez3_flash,
    official_gba_rom
};

constexpr bool hasEz3Catalog(CartridgeKind kind) noexcept
{
    return kind == CartridgeKind::ez3_flash;
}

struct ArmBranchProbe {
    std::array<std::uint8_t, 4> bytes{};
    std::optional<std::uint32_t> target;
};

// Capture-derived, non-destructive manager read session shared by the card
// inspector and save reader. It has no erase or program operation.
class ReadOnlyCartridge final {
public:
    explicit ReadOnlyCartridge(libusb_device_handle* handle) noexcept;
    explicit ReadOnlyCartridge(
        Transport& transport,
        std::function<void(std::chrono::milliseconds)> sleep = {}) noexcept;

    bool initialize();
    CartridgeKind kind() const noexcept { return kind_; }
    static CartridgeKind classifyProbeBehavior(
        const std::array<std::uint8_t, 4>& before_flash_id,
        const std::array<std::uint8_t, 4>& after_flash_id) noexcept;
    bool read(std::uint32_t byte_address,
              std::uint8_t* destination,
              std::size_t length);
    bool read(std::uint32_t byte_address,
              std::vector<std::uint8_t>& output,
              std::size_t length);
    std::optional<ArmBranchProbe> readArmBranch(
        std::uint32_t byte_address = 0);
    std::optional<GbaHeader> readGbaHeader(std::uint32_t byte_address);

    bool prepareLinear16MiB();
    bool prepareLinear24MiB();
    bool prepareLinear32MiB();
    // Select the smallest capture-proven linear mapping containing address.
    // Lower-8-MiB addresses require no mapping command.
    bool prepareLinearForAddress(std::uint32_t address);

    // Capture-derived readiness transition: three successful readiness polls
    // followed by the observed one-second quiet interval. This is not a full
    // bridge reset for a following destructive workflow.
    bool finishSession();

private:
    static constexpr std::uint32_t default_linear_read_limit = 0x00800000u;

    bool startup();
    bool probeUnlockTail();
    bool probePrefix(std::uint8_t a0, std::uint8_t a1,
                     std::uint8_t b0, std::uint8_t b1,
                     std::uint8_t c0, std::uint8_t c1,
                     bool include_tail = true);
    bool probeReset(bool use_f0);
    bool flashIdProbe(std::uint8_t a0, std::uint8_t a1,
                      std::uint8_t b0, std::uint8_t b1,
                      std::uint8_t c0, std::uint8_t c1);
    bool tx92TwoAt(std::uint32_t word_address,
                   std::uint8_t first, std::uint8_t second,
                   const char* label);
    std::optional<std::array<std::uint8_t, 4>> read91Sub2Four();
    bool flashStatus();
    bool finishReadMapping(const char* prefix);
    CartridgeKind classifyCartridgeContent();

    std::unique_ptr<BulkTransport> owned_transport_;
    Transport& transport_;
    Protocol protocol_;
    std::function<void(std::chrono::milliseconds)> sleep_;
    CartridgeKind kind_ = CartridgeKind::unknown;
    std::uint32_t linear_read_limit_ = default_linear_read_limit;
};

} // namespace ezfadvance
