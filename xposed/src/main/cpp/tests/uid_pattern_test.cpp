#include "uid_pattern.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
constexpr std::uint32_t load(unsigned target, unsigned base, unsigned offset) {
    return 0xb9400000U | ((offset / 4) << 10) | (base << 5) | target;
}
constexpr std::uint32_t shift(unsigned dest, unsigned source) {
    return 0x53047c00U | (source << 5) | dest;
}
constexpr std::uint32_t compare(unsigned source) { return 0x7109c41fU | (source << 5); }
}

int main() {
    using sensor_probe::findUidCandidate;
    require(findUidCandidate({}).offset == -1, "empty input");
    const std::array valid{load(20, 19, 0x38), shift(8, 20), compare(8)};
    auto result = findUidCandidate(valid);
    require(result.offset == 0x38 && result.matches == 1 && !result.ambiguous, "valid candidate");
    const std::array renamed{load(7, 22, 0x48), shift(11, 7), compare(11)};
    require(findUidCandidate(renamed).offset == 0x48, "register allocation independence");
    const std::array broken{load(20, 19, 0x38), shift(8, 21), compare(8)};
    require(findUidCandidate(broken).offset == -1, "load to shift dataflow");
    const std::array brokenCompare{load(20, 19, 0x38), shift(8, 20), compare(9)};
    require(findUidCandidate(brokenCompare).offset == -1, "shift to compare dataflow");
    const std::array conflict{valid[0], valid[1], valid[2], renamed[0], renamed[1], renamed[2]};
    result = findUidCandidate(conflict);
    require(result.offset == -1 && result.ambiguous && result.matches == 2, "ambiguous layouts rejected");
    const std::array stack{load(20, 31, 0x38), shift(8, 20), compare(8)};
    require(findUidCandidate(stack).offset == -1, "stack load rejected");
    require(findUidCandidate(std::span(valid.data(), 2)).offset == -1, "truncated pattern");
    const std::array wrongImmediate{valid[0], valid[1], valid[2] ^ (1U << 10)};
    require(findUidCandidate(wrongImmediate).offset == -1, "wrong compare immediate");
    std::puts("9 UID pattern checks passed");
}
