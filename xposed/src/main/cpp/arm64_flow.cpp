#include "arm64_flow.h"
#include <limits>

namespace sensor_probe {
namespace {
std::int64_t signExtend(std::uint32_t value, unsigned bits) {
    return (value & (std::uint32_t{1} << (bits - 1))) != 0
        ? std::int64_t(value) - (std::int64_t{1} << bits) : value;
}
}
DirectFlow directFlow(std::uint32_t word) {
    DirectFlow flow;
    if ((word & 0x7c000000U) == 0x14000000U) {
        flow.branch = true;
        flow.call = (word & 0x80000000U) != 0;
        flow.fallthrough = flow.call;
        flow.deltaWords = signExtend(word & 0x03ffffffU, 26);
    } else if ((word & 0xff000010U) == 0x54000000U ||
               (word & 0x7e000000U) == 0x34000000U) {
        flow.branch = true;
        flow.deltaWords = signExtend((word >> 5) & 0x7ffffU, 19);
    } else if ((word & 0x7e000000U) == 0x36000000U) {
        flow.branch = true;
        flow.deltaWords = signExtend((word >> 5) & 0x3fffU, 14);
    } else if ((word & 0xfffffc1fU) == 0xd63f0000U) flow.call = true;  // BLR Xn, AAPCS64 call boundary.
    else if (word == 0xd65f03c0U) flow.fallthrough = false;
    return flow;
}
bool branchAddress(std::uint64_t pc, std::int64_t deltaWords, std::uint64_t& target) {
    if (deltaWords < -(std::int64_t{1} << 25) || deltaWords >= (std::int64_t{1} << 25)) return false;
    const auto delta = deltaWords * 4;
    if (delta < 0) {
        if (pc < std::uint64_t(-delta)) return false;
        target = pc - std::uint64_t(-delta);
    } else {
        if (pc > std::numeric_limits<std::uint64_t>::max() - std::uint64_t(delta)) return false;
        target = pc + std::uint64_t(delta);
    }
    return true;
}
}  // namespace sensor_probe
