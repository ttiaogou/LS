#include "uid_pattern.h"

namespace sensor_probe {

UidCandidate findUidCandidate(std::span<const std::uint32_t> instructions) {
    UidCandidate result;
    for (std::size_t i = 0; i + 2 < instructions.size(); ++i) {
        const auto load = instructions[i];
        const auto shift = instructions[i + 1];
        const auto compare = instructions[i + 2];
        // LDR Wt,[Xn,#imm12*4]; UBFM Wd,Wt,#4,#31; SUBS WZR,Wd,#625.
        if ((load & 0xffc00000U) != 0xb9400000U ||
            (shift & 0xfffffc00U) != 0x53047c00U ||
            (compare & 0xfffffc1fU) != 0x7109c41fU) continue;
        const auto loaded = load & 31U;
        const auto shifted = shift & 31U;
        if (loaded == 31U || shifted == 31U || ((load >> 5) & 31U) == 31U ||
            ((shift >> 5) & 31U) != loaded || ((compare >> 5) & 31U) != shifted) continue;
        const int offset = static_cast<int>(((load >> 10) & 0xfffU) * 4U);
        if (offset == 0 || offset > 0x400) continue;
        ++result.matches;
        if (result.offset < 0) result.offset = offset;
        else if (result.offset != offset) result.ambiguous = true;
    }
    if (result.ambiguous) result.offset = -1;
    return result;
}

}  // namespace sensor_probe
