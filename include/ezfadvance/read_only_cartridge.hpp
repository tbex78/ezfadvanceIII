#pragma once

#include "ezfadvance/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ezfadvance {

// Capture-derived, non-destructive manager read session shared by the card
// inspector and save reader. It has no erase or program operation.
class ReadOnlyCartridge final {
public:
    explicit ReadOnlyCartridge(libusb_device_handle* handle) noexcept;

    bool initialize();
    bool read(std::uint32_t byte_address,
              std::uint8_t* destination,
              std::size_t length);
    bool read(std::uint32_t byte_address,
              std::vector<std::uint8_t>& output,
              std::size_t length);

    bool prepareLinear16MiB();
    bool prepareLinear24MiB();
    bool prepareLinear32MiB();

    // Capture-derived readiness transition: three successful readiness polls
    // followed by the observed one-second quiet interval. This is not a full
    // bridge reset for a following destructive workflow.
    bool finishSession();

private:
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
    bool read91Sub2Four();
    bool flashStatus();
    bool finishReadMapping(const char* prefix);

    BulkTransport transport_;
    Protocol protocol_;
};

} // namespace ezfadvance
