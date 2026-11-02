#include "uid_receiver.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
constexpr std::uint32_t mov(unsigned dest, unsigned source) {
    return 0xaa0003e0U | (source << 16) | dest;
}
constexpr std::uint32_t load(unsigned base) { return 0xb9403800U | (base << 5) | 20; }
constexpr std::uint32_t kShift = 0x53047e88U, kCompare = 0x7109c51fU, kRet = 0xd65f03c0U;
constexpr std::uint32_t kNop = 0xd503201fU;
constexpr std::uint32_t jump(int delta) { return 0x14000000U | (std::uint32_t(delta) & 0x03ffffffU); }
constexpr std::uint32_t cbz(int delta) { return 0xb4000001U | ((std::uint32_t(delta) & 0x7ffffU) << 5); }
bool rejects(std::span<const std::uint32_t> words, const char* reason) {
    const auto proof = sensor_probe::proveUidReceiver(words);
    return proof.offset == -1 && std::strcmp(proof.status, reason) == 0;
}
}

int main() {
    using sensor_probe::proveUidReceiver;
    require(rejects({}, "receiver-invalid-extent"), "empty input");
    const std::array direct{load(0), kShift, kCompare, kRet};
    require(proveUidReceiver(direct).offset == 0x38, "direct receiver");
    const std::array saved{mov(19, 0), load(19), kShift, kCompare, kRet};
    require(proveUidReceiver(saved).offset == 0x38, "callee-saved alias");
    const std::array unrelated{load(19), kShift, kCompare, kRet};
    require(rejects(unrelated, "receiver-load-base-unproven"), "uninitialized alias rejected");
    const std::array copied{mov(19, 0), mov(22, 19), load(22), kShift, kCompare, kRet};
    require(proveUidReceiver(copied).offset == 0x38, "copy chain");
    const std::array narrowed{mov(19, 0), 0x2a0003f3U, load(19), kShift, kCompare, kRet};
    require(rejects(narrowed, "receiver-load-base-unproven"), "W write loses pointer provenance");
    const std::array adjusted{mov(19, 0), 0x91002273U, load(19), kShift, kCompare, kRet};
    require(rejects(adjusted, "receiver-load-base-unproven"), "interior pointer rejected");
    const std::array acrossCall{mov(19, 0), 0x94000020U, load(19), kShift, kCompare, kRet};
    require(proveUidReceiver(acrossCall).offset == 0x38, "AAPCS callee-saved survives call");
    const std::array callerSaved{mov(9, 0), 0x94000020U, load(9), kShift, kCompare, kRet};
    require(rejects(callerSaved, "receiver-load-base-unproven"), "caller-saved invalidated");
    const std::array localCall{mov(19, 0), 0x94000001U, load(19), kShift, kCompare, kRet};
    require(rejects(localCall, "receiver-local-call-unsupported"), "local subroutine unsupported");
    const std::array join{mov(19, 0), cbz(2), mov(19, 1), load(19), kShift, kCompare, kRet};
    require(rejects(join, "receiver-load-base-unproven"), "join requires proof on every path");
    const std::array safeJoin{mov(19, 0), cbz(2), kNop, load(19), kShift, kCompare, kRet};
    require(proveUidReceiver(safeJoin).offset == 0x38, "equivalent incoming aliases");
    const std::array loop{mov(19, 0), load(19), kShift, kCompare, cbz(3), mov(19, 1), jump(-5), kRet};
    require(rejects(loop, "receiver-load-base-unproven"), "back edge clobber reaches load");
    const std::array safeLoop{mov(19, 0), load(19), kShift, kCompare, cbz(-3), kRet};
    require(proveUidReceiver(safeLoop).offset == 0x38, "loop fixed point");
    const std::array escaped{mov(19, 0), cbz(100), load(19), kShift, kCompare, kRet};
    require(rejects(escaped, "receiver-control-flow-outside-extent"), "escaped cold path rejected");
    const std::array negative{mov(19, 0), jump(-2), load(19), kShift, kCompare, kRet};
    require(rejects(negative, "receiver-control-flow-outside-extent"), "negative branch sign extension");
    const std::array indirect{mov(19, 0), 0xd61f0100U, load(19), kShift, kCompare, kRet};
    require(rejects(indirect, "receiver-unsupported-instruction"), "indirect branch rejected");
    const std::array unknown{mov(19, 0), 0U, load(19), kShift, kCompare, kRet};
    require(rejects(unknown, "receiver-unsupported-instruction"), "unknown opcode rejected");
    const std::array pair{mov(19, 0), 0xa9404ff4U, load(19), kShift, kCompare, kRet};
    require(rejects(pair, "receiver-load-base-unproven"), "second LDP destination invalidates alias");
    const std::array store{mov(19, 0), 0xf90003f3U, load(19), kShift, kCompare, kRet};
    require(proveUidReceiver(store).offset == 0x38, "store does not clobber register");
    const std::array writeback{mov(19, 0), 0xf8008e60U, load(19), kShift, kCompare, kRet};
    require(rejects(writeback, "receiver-load-base-unproven"), "writeback invalidates base register");
    const std::array truncated{mov(19, 0), load(19), kShift, kCompare};
    require(rejects(truncated, "receiver-fallthrough-outside-extent"), "missing return rejected");
    const std::array unreachable{jump(4), load(0), kShift, kCompare, kRet};
    require(rejects(unreachable, "receiver-load-base-unproven"), "unreachable pattern not proof");
    const std::array zeroRegister{mov(19, 31), load(19), kShift, kCompare, kRet};
    require(rejects(zeroRegister, "receiver-load-base-unproven"), "XZR is not receiver");
    const std::vector<std::uint32_t> oversized(16385, kNop);
    require(rejects(oversized, "receiver-invalid-extent"), "analysis size limit");
    const std::array hot{mov(19, 0), jump(0x1ff), load(19), kShift, kCompare, kRet};
    const std::array cold{kNop, jump(-0x1ff)};
    std::array<sensor_probe::InstructionRegion, 2> regions{{{0x1000, hot}, {0x1800, cold}}};
    require(proveUidReceiver(regions, 0x1000).offset == 0x38, "cross-fragment branch and return");
    std::swap(regions[0], regions[1]);
    require(proveUidReceiver(regions, 0x1000).offset == 0x38, "input order independent");
    const std::array coldClobber{mov(19, 1), jump(-0x1ff)};
    regions = {{{0x1000, hot}, {0x1800, coldClobber}}};
    require(proveUidReceiver(regions, 0x1000).offset == -1, "cold clobber propagates into hot load");
    require(proveUidReceiver(regions, 0x1400).offset == -1, "entry in gap rejected");
    regions = {{{0x1000, hot}, {0x1004, cold}}};
    require(proveUidReceiver(regions, 0x1000).offset == -1, "overlapping fragments rejected");
    const std::array<sensor_probe::InstructionRegion, 1> missing{{{0x1000, hot}}};
    require(proveUidReceiver(missing, 0x1000).offset == -1, "missing cold fragment rejected");
    const std::array prefix{mov(19, 0)};
    const std::array suffix{load(19), kShift, kCompare, kRet};
    regions = {{{0x1000, prefix}, {0x1800, suffix}}};
    require(proveUidReceiver(regions, 0x1000).offset == -1, "gaps are not implicit fallthrough");
    const std::array<sensor_probe::InstructionRegion, 1> overflow{{{0xfffffffffffffffcULL, direct}}};
    require(proveUidReceiver(overflow, 0xfffffffffffffffcULL).offset == -1, "region address overflow");
    const std::array indirectCall{mov(19, 0), 0xd63f0100U, load(19), kShift, kCompare, kRet};
    require(proveUidReceiver(indirectCall).offset == 0x38, "BLR observes AAPCS64 preservation");
    const std::array indirectCaller{mov(9, 0), 0xd63f0100U, load(9), kShift, kCompare, kRet};
    require(proveUidReceiver(indirectCaller).offset == -1, "BLR invalidates caller-saved alias");
    // Encodings cross-checked with the NDK assembler and llvm-objdump.
    for (const auto instruction : {0x9e660013U, 0x9e290013U, 0x08dffc13U,
                                  0xa9bf0660U, 0xa8c10660U, 0xf8617813U}) {
        const std::array overwritten{mov(19, 0), instruction, load(19), kShift, kCompare, kRet};
        require(rejects(overwritten, "receiver-load-base-unproven"), "load/FP/writeback invalidates alias");
    }
    for (const auto instruction : {0xf8217a60U, 0x9e230013U}) {
        const std::array preserved{mov(19, 0), instruction, load(19), kShift, kCompare, kRet};
        require(proveUidReceiver(preserved).offset == 0x38, "store/FP destination leaves GPR alias intact");
    }
    const std::array exclusive{mov(19, 0), 0x88137c20U, load(19), kShift, kCompare, kRet};
    require(rejects(exclusive, "receiver-unsupported-instruction"), "STXR extra output must not be ignored");
    std::printf("%d receiver provenance checks passed\n", checks);
}
