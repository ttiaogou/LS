#pragma once

#include "elf_function.h"
#include <span>
#include <string_view>

namespace sensor_probe {
struct ElfSection {
    bool found = false;
    std::uint64_t address = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};
ElfSection findElfAllocatedSection(std::span<const std::byte> file, std::string_view name);
ElfFunction mapElfExecutableRange(std::span<const std::byte> file, std::uint64_t address, std::uint64_t size);
}  // namespace sensor_probe
