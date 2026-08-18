#include "ezfadvance/cartridge_format.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace {

template <typename Exception>
void expectMakeRejected(std::uint32_t target)
{
    bool threw = false;
    try {
        (void)ezfadvance::CartridgeFormat::makeArmBranch(target);
    } catch (const Exception&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main()
{
    using ezfadvance::CartridgeFormat;

    // Known 1 MiB ROM + loader placement captured by the project.
    const std::uint32_t known = CartridgeFormat::makeArmBranch(0x00100010u);
    assert(known == 0xEA040002u);
    const std::uint8_t known_bytes[] = {
        static_cast<std::uint8_t>(known),
        static_cast<std::uint8_t>(known >> 8),
        static_cast<std::uint8_t>(known >> 16),
        static_cast<std::uint8_t>(known >> 24),
    };
    assert(known_bytes[0] == 0x02);
    assert(known_bytes[1] == 0x00);
    assert(known_bytes[2] == 0x04);
    assert(known_bytes[3] == 0xEA);

    // Original entry target used in catalog metadata.
    assert(CartridgeFormat::armBranchTarget(0xEA000031u) == 0x000000CCu);

    // Positive, negative, and range-boundary signed imm24 decoding.
    assert(CartridgeFormat::armBranchTarget(0xEA00003Eu) == 0x00000100u);
    assert(CartridgeFormat::armBranchTarget(0xEAFFFFFFu) == 0x00000004u);
    assert(CartridgeFormat::armBranchTarget(0xEA7FFFFFu) == 0x02000004u);
    assert(!CartridgeFormat::armBranchTarget(0xEAFFFFFDu));
    assert(!CartridgeFormat::armBranchTarget(0xEB000000u));

    // Encoder boundaries and round trips.
    assert(CartridgeFormat::makeArmBranch(8u) == 0xEA000000u);
    assert(CartridgeFormat::makeArmBranch(0x02000004u) == 0xEA7FFFFFu);
    assert(CartridgeFormat::armBranchTarget(known) == 0x00100010u);
    expectMakeRejected<std::invalid_argument>(4u);
    expectMakeRejected<std::invalid_argument>(9u);
    expectMakeRejected<std::out_of_range>(0x02000008u);
}
