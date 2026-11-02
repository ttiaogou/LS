#pragma once
#include <cstdint>

namespace sensor_probe {
struct DirectFlow {
    std::int64_t deltaWords = 0;
    bool branch = false;
    bool call = false;
    bool fallthrough = true;
};
DirectFlow directFlow(std::uint32_t word);
bool branchAddress(std::uint64_t pc, std::int64_t deltaWords, std::uint64_t& target);
}  // namespace sensor_probe
