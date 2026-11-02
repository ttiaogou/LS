#include "elf_function.h"

#include <cstring>
#include <elf.h>

namespace sensor_probe {
namespace {
bool range(std::span<const std::byte> file, std::uint64_t offset, std::uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
}

template <typename T>
bool read(std::span<const std::byte> file, std::uint64_t offset, T& result) {
    if (!range(file, offset, sizeof(T))) return false;
    std::memcpy(&result, file.data() + offset, sizeof(T));
    return true;
}
}  // namespace

ElfFunction findElfFunction(std::span<const std::byte> file, std::string_view name) {
    ElfFunction result;
    Elf64_Ehdr header{};
    if (!read(file, 0, header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT ||
        header.e_type != ET_DYN || header.e_machine != EM_AARCH64 ||
        header.e_ehsize != sizeof(Elf64_Ehdr) || header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_shentsize != sizeof(Elf64_Shdr) || header.e_phnum == 0 ||
        header.e_phnum > 4096 || header.e_shnum == 0 || header.e_shnum > 4096 ||
        !range(file, header.e_phoff, header.e_phnum * sizeof(Elf64_Phdr)) ||
        !range(file, header.e_shoff, header.e_shnum * sizeof(Elf64_Shdr))) return result;

    result.status = "elf-symbol-missing";
    for (unsigned i = 0; i < header.e_shnum; ++i) {
        Elf64_Shdr section{};
        if (!read(file, header.e_shoff + i * sizeof(section), section)) return {};
        if (section.sh_type != SHT_DYNSYM && section.sh_type != SHT_SYMTAB) continue;
        if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_link >= header.e_shnum ||
            section.sh_size % sizeof(Elf64_Sym) != 0 || !range(file, section.sh_offset, section.sh_size) ||
            section.sh_size / sizeof(Elf64_Sym) > 1000000) return {};
        Elf64_Shdr strings{};
        if (!read(file, header.e_shoff + section.sh_link * sizeof(strings), strings) ||
            strings.sh_type != SHT_STRTAB || !range(file, strings.sh_offset, strings.sh_size)) return {};

        for (std::uint64_t j = 0; j < section.sh_size / sizeof(Elf64_Sym); ++j) {
            Elf64_Sym symbol{};
            if (!read(file, section.sh_offset + j * sizeof(symbol), symbol)) return {};
            if (symbol.st_name >= strings.sh_size || name.size() >= strings.sh_size - symbol.st_name) continue;
            const auto* text = file.data() + strings.sh_offset + symbol.st_name;
            if (std::memcmp(text, name.data(), name.size()) != 0 || text[name.size()] != std::byte{0}) continue;
            if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_shndx == SHN_UNDEF ||
                symbol.st_shndx >= header.e_shnum || symbol.st_size == 0 || symbol.st_size > 65536 ||
                symbol.st_value % 4 != 0 || symbol.st_size % 4 != 0) {
                result.found = false;
                result.status = "elf-function-invalid";
                return result;
            }
            bool mapped = false;
            std::size_t offset = 0;
            for (unsigned k = 0; k < header.e_phnum; ++k) {
                Elf64_Phdr segment{};
                if (!read(file, header.e_phoff + k * sizeof(segment), segment)) return {};
                if (segment.p_type != PT_LOAD || (segment.p_flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
                    segment.p_filesz > segment.p_memsz || !range(file, segment.p_offset, segment.p_filesz) ||
                    symbol.st_value < segment.p_vaddr) continue;
                const auto delta = symbol.st_value - segment.p_vaddr;
                if (delta <= segment.p_filesz && symbol.st_size <= segment.p_filesz - delta) {
                    if (mapped) { result.found = false; result.status = "elf-mapping-ambiguous"; return result; }
                    mapped = true;
                    offset = segment.p_offset + delta;
                }
            }
            if (!mapped) { result.found = false; result.status = "elf-function-outside-rx"; return result; }
            if (result.found && (result.virtualAddress != symbol.st_value || result.size != symbol.st_size)) {
                result.found = false;
                result.status = "elf-symbol-ambiguous";
                return result;
            }
            result = {true, symbol.st_value, offset, static_cast<std::size_t>(symbol.st_size), "elf-symbol-bounded"};
        }
    }
    return result;
}

}  // namespace sensor_probe
