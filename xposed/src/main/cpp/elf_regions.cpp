#include "elf_regions.h"

#include <cstring>
#include <elf.h>
#include <limits>

namespace sensor_probe {
namespace {
bool range(std::span<const std::byte> file, std::uint64_t offset, std::uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
}
template <typename T> bool read(std::span<const std::byte> file, std::uint64_t offset, T& value) {
    if (!range(file, offset, sizeof(T))) return false;
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return true;
}
bool headerOf(std::span<const std::byte> file, Elf64_Ehdr& header) {
    return read(file, 0, header) && std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
        header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_ident[EI_DATA] == ELFDATA2LSB &&
        header.e_ident[EI_VERSION] == EV_CURRENT && header.e_version == EV_CURRENT &&
        header.e_type == ET_DYN && header.e_machine == EM_AARCH64 && header.e_ehsize == sizeof(header);
}
}

ElfSection findElfAllocatedSection(std::span<const std::byte> file, std::string_view name) {
    Elf64_Ehdr header;
    if (!headerOf(file, header) || header.e_shnum == 0 || header.e_shnum > 4096 ||
        header.e_shstrndx >= header.e_shnum || header.e_shentsize != sizeof(Elf64_Shdr) ||
        !range(file, header.e_shoff, header.e_shnum * sizeof(Elf64_Shdr))) return {};
    Elf64_Shdr strings;
    if (!read(file, header.e_shoff + header.e_shstrndx * sizeof(strings), strings) ||
        strings.sh_type != SHT_STRTAB || !range(file, strings.sh_offset, strings.sh_size)) return {};
    ElfSection result;
    for (unsigned i = 0; i < header.e_shnum; ++i) {
        Elf64_Shdr section;
        if (!read(file, header.e_shoff + i * sizeof(section), section)) return {};
        if (section.sh_name >= strings.sh_size || name.size() >= strings.sh_size - section.sh_name) continue;
        const auto* text = file.data() + strings.sh_offset + section.sh_name;
        if (std::memcmp(text, name.data(), name.size()) != 0 || text[name.size()] != std::byte{0}) continue;
        if (result.found || section.sh_type != SHT_PROGBITS || (section.sh_flags & SHF_ALLOC) == 0 ||
            section.sh_size == 0 || !range(file, section.sh_offset, section.sh_size)) return {};
        result = {true, section.sh_addr, static_cast<std::size_t>(section.sh_offset),
            static_cast<std::size_t>(section.sh_size)};
    }
    return result;
}

ElfFunction mapElfExecutableRange(std::span<const std::byte> file, std::uint64_t address, std::uint64_t size) {
    Elf64_Ehdr header;
    if (!headerOf(file, header) || header.e_phnum == 0 || header.e_phnum > 4096 ||
        header.e_phentsize != sizeof(Elf64_Phdr) ||
        !range(file, header.e_phoff, header.e_phnum * sizeof(Elf64_Phdr)) ||
        address % 4 != 0 || size == 0 || size > 65536 || size % 4 != 0 ||
        address > std::numeric_limits<std::uint64_t>::max() - size) return {};
    ElfFunction result;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr segment;
        if (!read(file, header.e_phoff + i * sizeof(segment), segment)) return {};
        if (segment.p_type != PT_LOAD || (segment.p_flags & (PF_R | PF_X | PF_W)) != (PF_R | PF_X) ||
            segment.p_filesz > segment.p_memsz || !range(file, segment.p_offset, segment.p_filesz) ||
            address < segment.p_vaddr) continue;
        const auto delta = address - segment.p_vaddr;
        if (delta > segment.p_filesz || size > segment.p_filesz - delta) continue;
        if (result.found) return {};
        result = {true, address, static_cast<std::size_t>(segment.p_offset + delta),
            static_cast<std::size_t>(size), "elf-rx-range-bounded"};
    }
    return result;
}
}  // namespace sensor_probe
