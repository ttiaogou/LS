#include "elf_function.h"
#include "mapped_file.h"
#include "uid_pattern.h"
#include "uid_receiver.h"
#include "code_closure.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "Usage: sensor_probe_file <ARM64 ELF library>\n"); return 2; }
    sensor_probe::MappedFile file(argv[1]);
    const auto function = sensor_probe::findElfFunction(file.bytes(), sensor_probe::kSendEventsSymbol);
    std::printf("status=%s vaddr=0x%llx fileOffset=0x%zx size=%zu\n", function.status,
        static_cast<unsigned long long>(function.virtualAddress), function.fileOffset, function.size);
    if (!function.found) return 1;
    std::vector<std::uint32_t> instructions(function.size / sizeof(std::uint32_t));
    std::memcpy(instructions.data(), file.bytes().data() + function.fileOffset, function.size);
    const auto uid = sensor_probe::findUidCandidate(instructions);
    std::printf("uidCandidate=%d matches=%zu ambiguous=%s runtimeVerified=false hookInstalled=false\n",
        uid.offset, uid.matches, uid.ambiguous ? "true" : "false");
    const auto receiver = sensor_probe::proveUidReceiver(instructions);
    std::printf("receiverStatus=%s receiverOffset=%d memoryAccessAuthorized=false\n",
        receiver.status, receiver.offset);
    const auto closure = sensor_probe::recoverCodeClosure(file.bytes(), function);
    std::printf("closureStatus=%s fragments=%zu bounded=%s\n", closure.status,
        closure.fragments.size(), closure.bounded ? "true" : "false");
    std::vector<sensor_probe::InstructionRegion> regions;
    for (const auto& fragment : closure.fragments) {
        std::printf("fragment=0x%llx size=%zu fileOffset=0x%zx\n",
            static_cast<unsigned long long>(fragment.range.virtualAddress), fragment.range.size, fragment.range.fileOffset);
        regions.push_back({fragment.range.virtualAddress, fragment.words});
    }
    if (closure.bounded) {
        const auto proof = sensor_probe::proveUidReceiver(regions, function.virtualAddress);
        std::printf("closureReceiver=%s offset=%d failurePC=0x%llx word=0x%08x memoryAccessAuthorized=false\n",
            proof.status, proof.offset, static_cast<unsigned long long>(proof.failureAddress), proof.failureWord);
    }
    return uid.offset >= 0 ? 0 : 1;
}
