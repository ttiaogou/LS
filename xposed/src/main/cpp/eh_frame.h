#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sensor_probe {
struct UnwindRange {
    std::uint64_t address;
    std::uint64_t size;
};
struct UnwindTable {
    std::vector<UnwindRange> ranges;
    bool valid = false;
    const char* status = "unwind-invalid";
};

// DWARF32 .eh_frame subset: zR/zPLR CIEs and PC-relative signed-32 FDE starts.
// FDE bounds are not proof of function ownership or reachability.
UnwindTable parseEhFrame(std::span<const std::byte> bytes, std::uint64_t address);
}  // namespace sensor_probe
