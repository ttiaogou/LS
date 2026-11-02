#include "eh_frame.h"
#include "unwind_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main() {
    using sensor_probe::parseEhFrame;
    auto good = fixture::unwind(0x3000, {{0x1000, 20}, {0x1800, 12}});
    auto parsed = parseEhFrame(good, 0x3000);
    require(parsed.valid && parsed.ranges.size() == 2 && parsed.ranges[1].address == 0x1800, "negative pcrel and two FDEs");
    auto forward = fixture::unwind(0x500, {{0x1000, 20}});
    require(parseEhFrame(forward, 0x500).valid, "positive pcrel");
    require(!parseEhFrame({}, 0).valid, "empty input");
    require(!parseEhFrame(good, std::numeric_limits<std::uint64_t>::max()).valid, "section address overflow");
    auto bad = good;
    fixture::put<std::uint32_t>(bad, 0, 0xffffffffU);
    require(!parseEhFrame(bad, 0x3000).valid, "DWARF64 or extended record rejected");
    bad = good; bad[8] = std::byte{4};
    require(!parseEhFrame(bad, 0x3000).valid, "unsupported CIE version");
    bad = good; bad[10] = std::byte{'Q'};
    require(!parseEhFrame(bad, 0x3000).valid, "unsupported augmentation");
    bad = good; bad[16] = std::byte{0x9b};
    require(!parseEhFrame(bad, 0x3000).valid, "indirect FDE start rejected");
    bad = good; bad[15] = std::byte{0x7f};
    require(!parseEhFrame(bad, 0x3000).valid, "augmentation length overflow");
    bad = good; fixture::put<std::uint32_t>(bad, 24, 23);
    require(!parseEhFrame(bad, 0x3000).valid, "CIE reference into record");
    bad = good; fixture::put<std::uint32_t>(bad, 24, 100);
    require(!parseEhFrame(bad, 0x3000).valid, "CIE reference underflow");
    bad = good; fixture::put<std::int32_t>(bad, 32, -4);
    require(!parseEhFrame(bad, 0x3000).valid, "negative extent");
    bad = good; fixture::put<std::int32_t>(bad, 32, 0);
    require(!parseEhFrame(bad, 0x3000).valid, "zero extent");
    bad = good; fixture::put<std::int32_t>(bad, 32, 3);
    require(!parseEhFrame(bad, 0x3000).valid, "unaligned extent");
    bad = good; fixture::put<std::int32_t>(bad, 32, 65540);
    require(!parseEhFrame(bad, 0x3000).valid, "extent limit");
    bad = good; bad[36] = std::byte{0x7f};
    require(!parseEhFrame(bad, 0x3000).valid, "FDE augmentation out of record");
    bad = good; bad.resize(35);
    require(!parseEhFrame(bad, 0x3000).valid, "truncated FDE");
    bad = good; bad.push_back(std::byte{1});
    require(!parseEhFrame(bad, 0x3000).valid, "nonzero trailing bytes");
    const auto overlap = fixture::unwind(0x3000, {{0x1000, 20}, {0x1008, 16}});
    require(!parseEhFrame(overlap, 0x3000).valid, "overlapping FDEs");
    const auto duplicate = fixture::unwind(0x3000, {{0x1000, 20}, {0x1000, 20}});
    require(!parseEhFrame(duplicate, 0x3000).valid, "duplicate FDEs");
    bad = good; fixture::put<std::int32_t>(bad, 28, std::numeric_limits<std::int32_t>::min());
    require(!parseEhFrame(bad, 0x3000).valid, "PC-relative underflow");
    // zPLR: indirect personality is skipped, never dereferenced; FDE encoding remains direct.
    auto augmented = good;
    augmented.insert(augmented.begin() + 20, 8, std::byte{0});
    fixture::put<std::uint32_t>(augmented, 0, 24);
    const unsigned char cie[]{1,'z','P','L','R',0,1,0x7c,30,7,0x9b,0,0,0,0,0xff,0x1b};
    std::memcpy(augmented.data() + 8, cie, sizeof(cie));
    // Moving FDEs requires adjusting both CIE references and PC-relative fields.
    for (const std::size_t start : {std::size_t{28}, std::size_t{48}}) {
        fixture::put<std::uint32_t>(augmented, start + 4, static_cast<std::uint32_t>(start + 4));
        const auto delta = fixture::get<std::int32_t>(augmented, start + 8);
        fixture::put<std::int32_t>(augmented, start + 8, delta - 8);
    }
    require(parseEhFrame(augmented, 0x3000).valid, "bounded zPLR augmentation");
    std::uint32_t seed = 0x53f19a7b;
    for (int i = 0; i < 5000; ++i) {
        bad = good;
        seed = seed * 1664525U + 1013904223U;
        const auto at = seed % bad.size();
        bad[at] ^= std::byte((seed >> 24) | 1U);
        const auto fuzzed = parseEhFrame(bad, 0x3000);
        if (fuzzed.valid) {
            for (const auto range : fuzzed.ranges)
                require(range.size > 0 && range.size <= 65536 && range.address % 4 == 0, "mutated range bounds");
        }
    }
    std::printf("EH-frame checks passed (%d assertions, 5000 mutations)\n", checks);
}
