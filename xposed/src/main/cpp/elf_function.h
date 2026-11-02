#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sensor_probe {

inline constexpr char kSendEventsSymbol[] =
    "_ZN7android13SensorService21SensorEventConnection10sendEventsEPK15sensors_event_tmPS2_PKNS_2wpIKS1_EE";

struct ElfFunction {
    bool found = false;
    std::uint64_t virtualAddress = 0;
    std::size_t fileOffset = 0;
    std::size_t size = 0;
    const char* status = "elf-invalid";
};

// Parses ELF metadata only; never executes the file or trusts raw file pointers.
ElfFunction findElfFunction(std::span<const std::byte> file, std::string_view name);

}  // namespace sensor_probe
