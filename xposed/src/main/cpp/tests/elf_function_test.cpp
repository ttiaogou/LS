#include "elf_function.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <limits>

namespace {
using Image = std::array<std::byte, 2048>;
template <typename T>
void put(Image& image, std::size_t offset, const T& value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}
Image fixture() {
    Image image{};
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_version = EV_CURRENT;
    header.e_type = ET_DYN;
    header.e_machine = EM_AARCH64;
    header.e_ehsize = sizeof(header);
    header.e_phoff = 64;
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 1;
    header.e_shoff = 128;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 3;
    put(image, 0, header);
    Elf64_Phdr segment{};
    segment.p_type = PT_LOAD;
    segment.p_flags = PF_R | PF_X;
    segment.p_offset = 1024;
    segment.p_vaddr = 0x4000;
    segment.p_filesz = 64;
    segment.p_memsz = 64;
    put(image, 64, segment);
    Elf64_Shdr symbols{};
    symbols.sh_type = SHT_DYNSYM;
    symbols.sh_offset = 512;
    symbols.sh_size = sizeof(Elf64_Sym);
    symbols.sh_entsize = sizeof(Elf64_Sym);
    symbols.sh_link = 2;
    put(image, 128 + sizeof(Elf64_Shdr), symbols);
    Elf64_Shdr strings{};
    strings.sh_type = SHT_STRTAB;
    strings.sh_offset = 768;
    strings.sh_size = sizeof(sensor_probe::kSendEventsSymbol);
    put(image, 128 + 2 * sizeof(Elf64_Shdr), strings);
    std::memcpy(image.data() + 768, sensor_probe::kSendEventsSymbol, strings.sh_size);
    Elf64_Sym symbol{};
    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    symbol.st_shndx = 1;
    symbol.st_value = 0x4000;
    symbol.st_size = 12;
    put(image, 512, symbol);
    return image;
}
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}

int main() {
    using sensor_probe::findElfFunction;
    const auto name = sensor_probe::kSendEventsSymbol;
    auto image = fixture();
    auto result = findElfFunction(image, name);
    require(result.found && result.fileOffset == 1024 && result.size == 12, "valid bounded function");
    require(!findElfFunction({}, name).found, "empty file");
    require(!findElfFunction(std::span(image.data(), 63), name).found, "truncated header");
    require(!findElfFunction(image, "absent").found, "missing symbol");
    image[EI_DATA] = std::byte{ELFDATA2MSB};
    require(!findElfFunction(image, name).found, "wrong endian");
    image = fixture();
    put(image, offsetof(Elf64_Ehdr, e_shoff), std::numeric_limits<Elf64_Off>::max());
    require(!findElfFunction(image, name).found, "overflowed section offset");
    image = fixture();
    put(image, 512 + offsetof(Elf64_Sym, st_size), Elf64_Xword{128});
    require(!findElfFunction(image, name).found, "function crosses file-backed segment");
    image = fixture();
    put(image, 512 + offsetof(Elf64_Sym, st_shndx), Elf64_Half{SHN_UNDEF});
    require(!findElfFunction(image, name).found, "undefined function");
    image = fixture();
    put(image, 64 + offsetof(Elf64_Phdr, p_flags), Elf64_Word{PF_R});
    require(!findElfFunction(image, name).found, "non-executable function");
    image = fixture();
    image[768 + sizeof(sensor_probe::kSendEventsSymbol) - 1] = std::byte{1};
    require(!findElfFunction(image, name).found, "unterminated symbol name");
    image = fixture();
    put(image, 128 + sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_link), Elf64_Word{0xffffffffU});
    require(!findElfFunction(image, name).found, "out-of-range linked string table");
    image = fixture();
    put(image, 512 + offsetof(Elf64_Sym, st_name), Elf64_Word{0xffffffffU});
    require(!findElfFunction(image, name).found, "out-of-range symbol name");
    image = fixture();
    put(image, 512 + offsetof(Elf64_Sym, st_size), Elf64_Xword{0});
    require(!findElfFunction(image, name).found, "zero-length function");
    image = fixture();
    put(image, offsetof(Elf64_Ehdr, e_machine), Elf64_Half{EM_X86_64});
    require(!findElfFunction(image, name).found, "wrong machine");
    std::uint32_t seed = 0x5eed1234U;
    for (unsigned i = 0; i < 5000; ++i) {
        image = fixture();
        for (unsigned j = 0; j < 4; ++j) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            image[seed % image.size()] = std::byte{static_cast<unsigned char>(seed >> 24)};
        }
        result = findElfFunction(image, name);
        if (result.found) require(result.fileOffset <= image.size() &&
            result.size <= image.size() - result.fileOffset, "mutation returned out-of-range function");
    }
    std::puts("14 ELF boundary checks and 5000 deterministic mutations passed");
}
