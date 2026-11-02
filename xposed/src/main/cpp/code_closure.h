#pragma once
#include "elf_function.h"
#include <vector>

namespace sensor_probe {
struct CodeFragment {
    ElfFunction range;
    std::vector<std::uint32_t> words;
};
struct CodeClosure {
    std::vector<CodeFragment> fragments;
    bool bounded = false;
    const char* status = "code-closure-unavailable";
};
// Bound direct non-call destinations via FDEs. Indirect flow is NOT resolved.
CodeClosure recoverCodeClosure(std::span<const std::byte> file, const ElfFunction& root);
}  // namespace sensor_probe
