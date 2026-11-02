#include "uid_receiver.h"
#include "uid_pattern.h"
#include "arm64_flow.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <vector>

namespace sensor_probe {
namespace {
constexpr std::size_t kMaxInstructions = 16384;
void forget(std::uint32_t& aliases, unsigned reg) {
    if (reg < 31) aliases &= ~(std::uint32_t{1} << reg);
}

bool transfer(std::uint32_t word, const DirectFlow& flow, std::uint32_t& aliases) {
    if (flow.branch || flow.call) {
        if (flow.call) aliases &= 0x3ff80000U;  // Only X19..X29 survive an AAPCS64 call.
        return true;
    }
    if (word == 0xd65f03c0U || word == 0xd503201fU || word == 0xd503245fU ||
        word == 0xd503249fU || word == 0xd50324dfU) return true;  // RET/NOP/BTI
    if (word == 0xd503233fU || word == 0xd50323bfU) {
        forget(aliases, 30);  // PACIASP/AUTIASP alter the link register.
        return true;
    }
    const auto dest = word & 31U;
    if ((word & 0xffe0ffe0U) == 0xaa0003e0U) {  // MOV Xd,Xm (unshifted ORR alias).
        const auto source = (word >> 16) & 31U;
        const bool fromReceiver = source < 31 && (aliases & (std::uint32_t{1} << source)) != 0;
        forget(aliases, dest);
        if (dest < 31 && fromReceiver) aliases |= std::uint32_t{1} << dest;
        return true;
    }
    const auto memory = word & 0xffc00000U;
    if (memory == 0xb9000000U || memory == 0xf9000000U) return true;  // STR unsigned, no writeback.
    if (memory == 0xb9400000U || memory == 0xf9400000U) {
        forget(aliases, dest);
        return true;
    }
    if (memory == 0x29000000U || memory == 0xa9000000U) return true;  // STP offset, no writeback.
    if (memory == 0x29400000U || memory == 0xa9400000U) {
        forget(aliases, dest);
        forget(aliases, (word >> 10) & 31U);
        return true;
    }
    // A small explicit subset only. W-register writes also invalidate the X alias.
    if ((word & 0x1f000000U) == 0x11000000U ||  // ADD/SUB immediate
        (word & 0x7fe0ffe0U) == 0x2a0003e0U ||  // MOV Wd,Wm
        (word & 0x7f800000U) == 0x52800000U ||  // MOVZ W/X
        (word & 0x7f800000U) == 0x12800000U ||  // MOVN W/X
        (word & 0x7f800000U) == 0x72800000U ||  // MOVK W/X
        (word & 0xffc00000U) == 0x53000000U) {  // UBFM W (including UID LSR)
        forget(aliases, dest);
        return true;
    }
    // Scalar data-processing instructions have at most the encoded Rd as a
    // GPR destination. They never establish an exact receiver alias here.
    if ((word & 0x1c000000U) == 0x10000000U ||  // ADR/ADRP and immediate data processing
        (word & 0x0e000000U) == 0x0a000000U ||  // Logical/arithmetic/select/multiply
        (word & 0xfff00000U) == 0xd5300000U) {  // MRS
        forget(aliases, dest);
        return true;
    }
    const bool vector = (word & 0x04000000U) != 0;
    const bool load = (word & 0x00c00000U) != 0;
    if ((word & 0x3b000000U) == 0x39000000U ||  // Unsigned immediate load/store
        (word & 0x3b000000U) == 0x18000000U) {  // Literal load (PRFM over-invalidates only)
        if (!vector && (load || (word & 0x3b000000U) == 0x18000000U)) forget(aliases, dest);
        return true;
    }
    if ((word & 0x3b000000U) == 0x38000000U) {
        const auto mode = (word >> 10) & 3U;
        if ((word & 0x00200000U) != 0) {
            if (mode != 2) return false;  // Not register-offset: could be atomic, with another output.
        } else if (mode == 1 || mode == 3) forget(aliases, (word >> 5) & 31U);
        if (!vector && load) forget(aliases, dest);
        return true;
    }
    if ((word & 0x3a000000U) == 0x28000000U) {
        if (!vector && (word & 0x00400000U) != 0) {
            forget(aliases, dest);
            forget(aliases, (word >> 10) & 31U);
        }
        const auto mode = (word >> 23) & 3U;
        if (mode == 1 || mode == 3) forget(aliases, (word >> 5) & 31U);
        return true;
    }
    if ((word & 0x3ffffc00U) == 0x08dffc00U) {  // LDAR/LDARB/LDARH
        forget(aliases, dest);
        return true;
    }
    if ((word & 0x3ffffc00U) == 0x089ffc00U) return true;  // STLR variants
    const auto unary = word & 0xfffffc00U;
    if (unary == 0x1e260000U || unary == 0x9e660000U || unary == 0x9e290000U) {  // FMOV/FCVTPU to GPR
        forget(aliases, dest);
        return true;
    }
    if (unary == 0x1e270000U || unary == 0x9e670000U ||  // FMOV from GPR to FP
        unary == 0x9e230000U ||  // UCVTF S,X
        unary == 0x0e205800U || unary == 0x2e303800U ||  // CNT/UADDLV
        (word & 0xffffffe0U) == 0x6f00e400U ||  // MOVI .2D,#0
        (word & 0xff20fc00U) == 0x1e200800U ||  // FMUL S/D
        (word & 0xff20fc00U) == 0x1e201800U ||  // FDIV S/D
        (word & 0xffe0fc1fU) == 0x1e202000U) return true;  // FCMP S
    return false;
}
}  // namespace

UidReceiverProof proveUidReceiver(std::span<const std::uint32_t> instructions) {
    const InstructionRegion region{0, instructions};
    return proveUidReceiver(std::span(&region, 1), 0);
}

UidReceiverProof proveUidReceiver(std::span<const InstructionRegion> regions, std::uint64_t entry) {
    UidReceiverProof result;
    struct Node { std::uint64_t address; std::uint32_t word; };
    std::vector<Node> nodes;
    if (regions.empty() || regions.size() > 8) {
        result.status = "receiver-invalid-extent";
        return result;
    }
    for (const auto& region : regions) {
        if (region.instructions.empty() || region.address % 4 != 0 ||
            region.instructions.size() > kMaxInstructions - nodes.size() ||
            region.instructions.size() > (std::numeric_limits<std::uint64_t>::max() - region.address) / 4) {
            result.status = "receiver-invalid-extent";
            return result;
        }
        for (std::size_t i = 0; i < region.instructions.size(); ++i)
            nodes.push_back({region.address + i * 4, region.instructions[i]});
    }
    std::sort(nodes.begin(), nodes.end(), [](auto a, auto b) { return a.address < b.address; });
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        if (nodes[i - 1].address == nodes[i].address) {
            result.status = "receiver-overlapping-regions";
            return result;
        }
    }
    auto indexAt = [&](std::uint64_t address) {
        const auto it = std::lower_bound(nodes.begin(), nodes.end(), address,
            [](auto node, auto pc) { return node.address < pc; });
        return it != nodes.end() && it->address == address ? std::size_t(it - nodes.begin()) : nodes.size();
    };
    const auto entryIndex = indexAt(entry);
    if (entryIndex == nodes.size()) { result.status = "receiver-entry-outside-regions"; return result; }
    std::vector<std::size_t> loads;
    int offset = -1;
    for (std::size_t i = 0; i + 2 < nodes.size(); ++i) {
        if (nodes[i + 2].address - nodes[i].address != 8) continue;
        const std::uint32_t triple[]{nodes[i].word, nodes[i + 1].word, nodes[i + 2].word};
        const auto candidate = findUidCandidate(triple);
        if (candidate.offset < 0) continue;
        ++result.matches;
        if (offset >= 0 && offset != candidate.offset) {
            result.status = "receiver-ambiguous-pattern";
            return result;
        }
        offset = candidate.offset;
        loads.push_back(i);
    }
    if (loads.empty()) { result.status = "receiver-no-pattern"; return result; }
    auto failAt = [&](std::size_t i, const char* status) {
        result.status = status;
        result.failureAddress = nodes[i].address;
        result.failureWord = nodes[i].word;
        return result;
    };
    // Reject escaped cold paths before interpreting opcodes: they can re-enter
    // with different register values and are not covered by the symbol extent.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto flow = directFlow(nodes[i].word);
        if (!flow.branch) continue;
        std::uint64_t target;
        if (!branchAddress(nodes[i].address, flow.deltaWords, target))
            return failAt(i, "receiver-control-flow-outside-extent");
        const bool inside = indexAt(target) != nodes.size();
        if (flow.branch && !flow.call && !inside) {
            return failAt(i, "receiver-control-flow-outside-extent");
        }
        if (flow.call && inside) {
            return failAt(i, "receiver-local-call-unsupported");
        }
    }
    std::vector<std::uint32_t> incoming(nodes.size());
    std::vector<bool> reached(nodes.size()), queued(nodes.size());
    std::deque<std::size_t> pending{entryIndex};
    incoming[entryIndex] = 1;  // X0 is the receiver at the function entry.
    reached[entryIndex] = queued[entryIndex] = true;
    auto merge = [&](std::size_t successor, std::uint32_t aliases) {
        const auto combined = reached[successor] ? incoming[successor] & aliases : aliases;
        if (!reached[successor] || combined != incoming[successor]) {
            incoming[successor] = combined;
            reached[successor] = true;
            if (!queued[successor]) { pending.push_back(successor); queued[successor] = true; }
        }
    };
    while (!pending.empty()) {
        const auto i = pending.front();
        pending.pop_front();
        queued[i] = false;
        const auto flow = directFlow(nodes[i].word);
        auto aliases = incoming[i];
        if (!transfer(nodes[i].word, flow, aliases)) {
            return failAt(i, "receiver-unsupported-instruction");
        }
        if (flow.branch && !flow.call) {
            std::uint64_t target;
            if (!branchAddress(nodes[i].address, flow.deltaWords, target))
                return failAt(i, "receiver-control-flow-outside-extent");
            merge(indexAt(target), aliases);
        }
        if (flow.fallthrough) {
            const auto next = indexAt(nodes[i].address + 4);
            if (next == nodes.size()) {
                return failAt(i, "receiver-fallthrough-outside-extent");
            }
            merge(next, aliases);
        }
    }
    for (const auto i : loads) {
        const auto base = (nodes[i].word >> 5) & 31U;
        if (!reached[i] || (incoming[i] & (std::uint32_t{1} << base)) == 0) {
            return failAt(i, "receiver-load-base-unproven");
        }
    }
    result.offset = offset;
    result.status = "receiver-alias-proven-subset";
    return result;
}
}  // namespace sensor_probe
