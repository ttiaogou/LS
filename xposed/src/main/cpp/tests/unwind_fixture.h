#pragma once

#include "eh_frame.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <vector>

namespace fixture {
template <typename T> void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
template <typename T> T get(const std::vector<std::byte>& bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}
inline std::vector<std::byte> unwind(std::uint64_t sectionAddress,
    const std::vector<sensor_probe::UnwindRange>& ranges) {
    std::vector<std::byte> bytes(20);
    put<std::uint32_t>(bytes, 0, 16);
    const unsigned char cie[]{1, 'z', 'R', 0, 1, 0x7c, 30, 1, 0x1b, 0x0c, 31, 0};
    std::memcpy(bytes.data() + 8, cie, sizeof(cie));
    for (const auto& range : ranges) {
        const auto start = bytes.size();
        bytes.resize(start + 20);
        put<std::uint32_t>(bytes, start, 16);
        put<std::uint32_t>(bytes, start + 4, static_cast<std::uint32_t>(start + 4));
        put<std::int32_t>(bytes, start + 8, static_cast<std::int32_t>(
            std::int64_t(range.address) - std::int64_t(sectionAddress + start + 8)));
        put<std::int32_t>(bytes, start + 12, static_cast<std::int32_t>(range.size));
    }
    bytes.resize(bytes.size() + 4);
    return bytes;
}
inline std::vector<std::byte> elf() {
    std::vector<std::byte> file(0x4000);
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_DYN;
    header.e_machine = EM_AARCH64;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 1;
    header.e_shoff = 0x300;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 3;
    header.e_shstrndx = 1;
    put(file, 0, header);
    Elf64_Phdr load{};
    load.p_type = PT_LOAD;
    load.p_flags = PF_R | PF_X;
    load.p_offset = load.p_vaddr = 0x1000;
    load.p_filesz = load.p_memsz = 0x1000;
    put(file, sizeof(header), load);
    const char names[] = "\0.shstrtab\0.eh_frame\0";
    std::memcpy(file.data() + 0x200, names, sizeof(names));
    Elf64_Shdr strings{};
    strings.sh_name = 1;
    strings.sh_type = SHT_STRTAB;
    strings.sh_offset = 0x200;
    strings.sh_size = sizeof(names);
    put(file, 0x300 + sizeof(Elf64_Shdr), strings);
    const auto frames = unwind(0x3000, {{0x1000, 20}, {0x1800, 12}});
    Elf64_Shdr eh{};
    eh.sh_name = 11;
    eh.sh_type = SHT_PROGBITS;
    eh.sh_flags = SHF_ALLOC;
    eh.sh_addr = eh.sh_offset = 0x3000;
    eh.sh_size = frames.size();
    put(file, 0x300 + 2 * sizeof(Elf64_Shdr), eh);
    std::memcpy(file.data() + 0x3000, frames.data(), frames.size());
    // Receiver copied in hot code; cold block returns to the UID comparison.
    const std::uint32_t hot[]{0xaa0003f3U, 0x140001ffU, 0xb9403a74U, 0x53047e88U, 0x7109c51fU};
    const std::uint32_t cold[]{0xd503201fU, 0x17fffe01U, 0xd65f03c0U};
    std::memcpy(file.data() + 0x1000, hot, sizeof(hot));
    std::memcpy(file.data() + 0x1800, cold, sizeof(cold));
    return file;
}
}  // namespace fixture
