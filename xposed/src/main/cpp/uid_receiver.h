#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sensor_probe {

struct UidReceiverProof {
    int offset = -1;
    std::size_t matches = 0;
    const char* status = "receiver-not-analyzed";
    std::uint64_t failureAddress = 0;
    std::uint32_t failureWord = 0;
};

struct InstructionRegion {
    std::uint64_t address;
    std::span<const std::uint32_t> instructions;
};

// Bounded must-alias analysis under AAPCS64. This does not authorize memory reads.
UidReceiverProof proveUidReceiver(std::span<const std::uint32_t> instructions);
UidReceiverProof proveUidReceiver(std::span<const InstructionRegion> regions, std::uint64_t entry);

}  // namespace sensor_probe
