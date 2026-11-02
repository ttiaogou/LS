#include "code_closure.h"
#include "arm64_flow.h"
#include "eh_frame.h"
#include "elf_regions.h"

#include <algorithm>
#include <cstring>

namespace sensor_probe {
CodeClosure recoverCodeClosure(std::span<const std::byte> file, const ElfFunction& root) {
    CodeClosure result;
    const auto mappedRoot = mapElfExecutableRange(file, root.virtualAddress, root.size);
    if (!root.found || !mappedRoot.found || mappedRoot.fileOffset != root.fileOffset) return result;
    const auto section = findElfAllocatedSection(file, ".eh_frame");
    if (!section.found) return result;
    const auto unwind = parseEhFrame(file.subspan(section.offset, section.size), section.address);
    if (!unwind.valid) { result.status = unwind.status; return result; }
    const auto rootFde = std::find_if(unwind.ranges.begin(), unwind.ranges.end(), [&](auto range) {
        return range.address == root.virtualAddress && range.size == root.size;
    });
    if (rootFde == unwind.ranges.end()) { result.status = "code-root-fde-mismatch"; return result; }
    auto append = [&](ElfFunction range) {
        CodeFragment fragment{range, std::vector<std::uint32_t>(range.size / 4)};
        std::memcpy(fragment.words.data(), file.data() + range.fileOffset, range.size);
        result.fragments.push_back(std::move(fragment));
    };
    append(mappedRoot);
    std::size_t totalWords = root.size / 4;
    for (std::size_t i = 0; i < result.fragments.size(); ++i) {
        // append() can reallocate the vector; never retain a reference across it.
        const auto wordCount = result.fragments[i].words.size();
        const auto start = result.fragments[i].range.virtualAddress;
        for (std::size_t j = 0; j < wordCount; ++j) {
            const auto flow = directFlow(result.fragments[i].words[j]);
            if (!flow.branch || flow.call) continue;
            std::uint64_t target;
            if (!branchAddress(start + j * 4, flow.deltaWords, target)) return {};
            const bool covered = std::any_of(result.fragments.begin(), result.fragments.end(), [&](const auto& f) {
                return target >= f.range.virtualAddress && target - f.range.virtualAddress < f.range.size;
            });
            if (covered) continue;
            const auto upper = std::upper_bound(unwind.ranges.begin(), unwind.ranges.end(), target,
                [](auto pc, auto range) { return pc < range.address; });
            if (upper == unwind.ranges.begin() || target - (upper - 1)->address >= (upper - 1)->size) {
                result.status = "code-branch-without-fde";
                return result;
            }
            const auto fde = *(upper - 1);
            const auto mapped = mapElfExecutableRange(file, fde.address, fde.size);
            if (!mapped.found) { result.status = "code-fragment-outside-rx"; return result; }
            if (result.fragments.size() >= 8 || mapped.size / 4 > 16384 - totalWords) {
                result.status = "code-closure-limit";
                return result;
            }
            totalWords += mapped.size / 4;
            append(mapped);
        }
    }
    result.bounded = true;
    result.status = "code-direct-closure-bounded";
    return result;
}
}  // namespace sensor_probe
