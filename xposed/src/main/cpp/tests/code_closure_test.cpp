#include "code_closure.h"
#include "elf_regions.h"
#include "unwind_fixture.h"
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main() {
    using sensor_probe::recoverCodeClosure;
    const sensor_probe::ElfFunction root{true, 0x1000, 0x1000, 20, "fixture"};
    const auto good = fixture::elf();
    const auto closure = recoverCodeClosure(good, root);
    require(closure.bounded && closure.fragments.size() == 2 && closure.fragments[1].range.size == 12,
        "FDE-bounded cold fragment recovered");
    auto wrongRoot = root; wrongRoot.size = 16;
    require(!recoverCodeClosure(good, wrongRoot).bounded, "root size must agree with FDE");
    wrongRoot = root; wrongRoot.fileOffset = 0x1004;
    require(!recoverCodeClosure(good, wrongRoot).bounded, "root mapping must agree");
    auto bad = good;
    fixture::put<std::uint32_t>(bad, 0x1004, 0x140002ffU);
    require(!recoverCodeClosure(bad, root).bounded, "branch to RX gap needs FDE");
    bad = good; fixture::put<std::uint32_t>(bad, 0x1004, 0x14000202U);
    require(!recoverCodeClosure(bad, root).bounded, "branch to FDE end is outside");
    bad = good; fixture::put<std::uint32_t>(bad, 0x1004, 0x14000200U);
    require(recoverCodeClosure(bad, root).bounded, "branch to FDE interior");
    bad = good;
    auto ph = fixture::get<Elf64_Phdr>(bad, sizeof(Elf64_Ehdr));
    ph.p_flags |= PF_W; fixture::put(bad, sizeof(Elf64_Ehdr), ph);
    require(!recoverCodeClosure(bad, root).bounded, "writable executable segment rejected");
    bad = good;
    auto sh = fixture::get<Elf64_Shdr>(bad, 0x300 + 2 * sizeof(Elf64_Shdr));
    sh.sh_offset = bad.size(); fixture::put(bad, 0x300 + 2 * sizeof(Elf64_Shdr), sh);
    require(!recoverCodeClosure(bad, root).bounded, "truncated EH section rejected");
    bad = good; bad[0x20b] = std::byte{'Q'};
    require(!recoverCodeClosure(bad, root).bounded, "missing named EH section");
    require(!sensor_probe::mapElfExecutableRange(good, 0x1ffc, 8).found, "RX file boundary");
    require(!sensor_probe::mapElfExecutableRange(good, 0x1001, 4).found, "unaligned code");
    require(!sensor_probe::mapElfExecutableRange(good, 0xfffffffffffffff0ULL, 32).found, "virtual overflow");
    const auto section = sensor_probe::findElfAllocatedSection(good, ".eh_frame");
    require(section.found && section.offset == 0x3000, "allocated section bounds");
    bad = good;
    auto header = fixture::get<Elf64_Ehdr>(bad, 0);
    header.e_phnum = 2; fixture::put(bad, 0, header);
    ph = fixture::get<Elf64_Phdr>(bad, sizeof(Elf64_Ehdr));
    fixture::put(bad, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), ph);
    require(!recoverCodeClosure(bad, root).bounded, "ambiguous RX mappings rejected");
    bad = good; fixture::put<std::int32_t>(bad, 0x3000 + 52, 0x1000);
    require(!recoverCodeClosure(bad, root).bounded, "cold FDE must fit RX file mapping");
    bad = good;
    std::vector<sensor_probe::UnwindRange> chain{{0x1000, 20}};
    for (unsigned i = 0; i < 8; ++i) {
        const auto pc = 0x1800 + i * 0x20;
        chain.push_back({pc, 8});
        fixture::put<std::uint32_t>(bad, pc, i == 7 ? 0xd65f03c0U : 0x14000008U);
        fixture::put<std::uint32_t>(bad, pc + 4, 0xd65f03c0U);
    }
    const auto frames = fixture::unwind(0x3000, chain);
    std::memcpy(bad.data() + 0x3000, frames.data(), frames.size());
    sh = fixture::get<Elf64_Shdr>(bad, 0x300 + 2 * sizeof(Elf64_Shdr));
    sh.sh_size = frames.size(); fixture::put(bad, 0x300 + 2 * sizeof(Elf64_Shdr), sh);
    require(!recoverCodeClosure(bad, root).bounded, "fragment count bounded");
    std::uint32_t seed = 0x9123bacf;
    for (unsigned i = 0; i < 2000; ++i) {
        bad = good;
        seed = seed * 1664525U + 1013904223U;
        bad[seed % bad.size()] ^= std::byte((seed >> 24) | 1U);
        const auto mutated = recoverCodeClosure(bad, root);
        if (!mutated.bounded) continue;
        require(mutated.fragments.size() <= 8, "mutated fragment count");
        std::size_t words = 0;
        for (const auto& fragment : mutated.fragments) {
            words += fragment.words.size();
            require(fragment.range.fileOffset <= bad.size() &&
                fragment.range.size <= bad.size() - fragment.range.fileOffset, "mutated file bounds");
        }
        require(words <= 16384, "mutated instruction budget");
    }
    std::printf("%d code-closure checks passed\n", checks);
}
