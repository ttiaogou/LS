#include "target_locator.h"
#include "elf_function.h"
#include "mapped_file.h"
#include "code_closure.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <limits>
#include <vector>

namespace sensor_probe {
namespace {
constexpr char kLibrary[] = "/system/lib64/libsensorservice.so";
struct Search {
    std::uint64_t virtualAddress;
    std::uintptr_t address = 0;
    std::size_t available = 0;
};

int readableExecutableRange(dl_phdr_info* info, std::size_t, void* opaque) {
    auto& search = *static_cast<Search*>(opaque);
    if (info->dlpi_name == nullptr || std::strcmp(info->dlpi_name, kLibrary) != 0) return 0;
    if (search.virtualAddress > std::numeric_limits<std::uintptr_t>::max() - info->dlpi_addr) return 0;
    const auto address = info->dlpi_addr + search.virtualAddress;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || (segment.p_flags & (PF_R | PF_X | PF_W)) != (PF_R | PF_X)) continue;
        if (segment.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - info->dlpi_addr) continue;
        const auto begin = info->dlpi_addr + segment.p_vaddr;
        if (address >= begin && address - begin < segment.p_memsz) {
            search.address = address;
            search.available = segment.p_memsz - (address - begin);
            return 1;
        }
    }
    return 0;
}
}  // namespace

void* resolveLoadedServiceFunction(const char* symbol) {
    if (symbol == nullptr || *symbol == '\0') return nullptr;
    MappedFile file(kLibrary);
    const auto function = findElfFunction(file.bytes(), symbol);
    if (!function.found) return nullptr;
    Search search{function.virtualAddress};
    dl_iterate_phdr(readableExecutableRange, &search);
    if (search.address == 0 || search.available < function.size) return nullptr;
    auto* address = reinterpret_cast<void*>(search.address);
    if (std::memcmp(address, file.bytes().data() + function.fileOffset, function.size) != 0) return nullptr;
    return address;
}

TargetReport inspectLoadedTarget() {
    TargetReport report;
    MappedFile file(kLibrary);
    const auto function = findElfFunction(file.bytes(), kSendEventsSymbol);
    if (!function.found) { report.status = function.status; return report; }
    Search search{function.virtualAddress};
    dl_iterate_phdr(readableExecutableRange, &search);
    if (search.address == 0 || search.available < function.size) {
        report.status = "probe-function-outside-loaded-rx";
        return report;
    }
    // NOLOAD avoids loading SensorService into a process that does not already host it.
    void* handle = dlopen(kLibrary, RTLD_NOW | RTLD_NOLOAD);
    void* symbol = handle == nullptr ? nullptr : dlsym(handle, kSendEventsSymbol);
    auto* expected = reinterpret_cast<void*>(search.address);
    if (symbol != nullptr && symbol != expected) {
        report.status = "probe-dlsym-file-address-mismatch";
    } else if (std::memcmp(expected, file.bytes().data() + function.fileOffset, function.size) != 0) {
        report.status = "probe-loaded-code-mismatch";
    } else {
        report.address = expected;
        report.functionSize = function.size;
        report.via = symbol == nullptr ? "file-symbol-and-loaded-rx" : "dlsym-and-file-symbol";
        std::array<std::uint32_t, 16384> words{};
        const auto count = function.size / sizeof(std::uint32_t);
        std::memcpy(words.data(), expected, function.size);
        report.uid = findUidCandidate(std::span(words.data(), count));
        report.receiver = proveUidReceiver(std::span(words.data(), count));
        const auto closure = recoverCodeClosure(file.bytes(), function);
        report.closureStatus = closure.status;
        if (closure.bounded) {
            std::vector<InstructionRegion> regions;
            bool verified = true;
            for (const auto& fragment : closure.fragments) {
                Search part{fragment.range.virtualAddress};
                dl_iterate_phdr(readableExecutableRange, &part);
                if (part.address == 0 || part.available < fragment.range.size ||
                    std::memcmp(reinterpret_cast<const void*>(part.address), fragment.words.data(), fragment.range.size) != 0) {
                    verified = false;
                    report.closureStatus = "code-runtime-fragment-mismatch";
                    break;
                }
                regions.push_back({fragment.range.virtualAddress, fragment.words});
            }
            if (verified) {
                report.receiver = proveUidReceiver(regions, function.virtualAddress);
                report.codeFragments = regions.size();
                report.runtimeFragmentsVerified = true;
            }
        }
        report.status = report.uid.ambiguous ? "probe-uid-ambiguous-no-hook" :
            report.uid.offset < 0 ? "probe-uid-pattern-missing-no-hook" : "probe-symbol-candidate-no-hook";
    }
    if (handle != nullptr) dlclose(handle);
    return report;
}

}  // namespace sensor_probe
