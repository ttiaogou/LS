#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sensor_probe {

struct UidCandidate {
    int offset = -1;
    std::size_t matches = 0;
    bool ambiguous = false;
};

// A syntactic candidate, not proof of the receiver object's layout.
UidCandidate findUidCandidate(std::span<const std::uint32_t> instructions);

}  // namespace sensor_probe
