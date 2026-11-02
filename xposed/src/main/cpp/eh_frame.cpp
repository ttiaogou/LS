#include "eh_frame.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>

namespace sensor_probe {
namespace {
class Reader {
public:
    std::span<const std::byte> bytes;
    std::size_t pos = 0;
    template <typename T> bool read(T& value) {
        if (pos > bytes.size() || sizeof(T) > bytes.size() - pos) return false;
        std::memcpy(&value, bytes.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    bool uleb(std::uint64_t& value) {
        value = 0;
        for (unsigned i = 0; i < 10; ++i) {
            std::uint8_t byte;
            if (!read(byte) || (i == 9 && (byte & 0xfeU) != 0)) return false;
            value |= std::uint64_t(byte & 0x7fU) << (7 * i);
            if ((byte & 0x80U) == 0) return true;
        }
        return false;
    }
    bool sleb() {
        // Only consume the bounded CIE data-alignment factor; it is not used as a range.
        for (unsigned i = 0; i < 10; ++i) {
            std::uint8_t byte;
            if (!read(byte) || (i == 9 && byte != 0 && byte != 0x7f)) return false;
            if ((byte & 0x80U) == 0) return true;
        }
        return false;
    }
};
bool addSigned(std::uint64_t base, std::int32_t delta, std::uint64_t& result) {
    if (delta < 0) {
        const auto magnitude = std::uint64_t(-std::int64_t(delta));
        if (base < magnitude) return false;
        result = base - magnitude;
    } else {
        if (base > std::numeric_limits<std::uint64_t>::max() - std::uint32_t(delta)) return false;
        result = base + std::uint32_t(delta);
    }
    return true;
}
struct Cie { std::uint8_t encoding = 0; bool augmented = false; };
bool readCie(Reader& reader, Cie& cie) {
    std::uint8_t version;
    if (!reader.read(version) || (version != 1 && version != 3)) return false;
    std::string augmentation;
    for (;;) {
        std::uint8_t byte;
        if (!reader.read(byte) || augmentation.size() > 8) return false;
        if (byte == 0) break;
        augmentation += static_cast<char>(byte);
    }
    if (augmentation.empty() || augmentation[0] != 'z') return false;
    std::uint64_t ignored, augmentationLength;
    if (!reader.uleb(ignored) || !reader.sleb()) return false;
    if (version == 1) {
        std::uint8_t reg;
        if (!reader.read(reg)) return false;
    } else if (!reader.uleb(ignored)) return false;
    if (!reader.uleb(augmentationLength) || augmentationLength > reader.bytes.size() - reader.pos) return false;
    Reader args{reader.bytes.subspan(reader.pos, static_cast<std::size_t>(augmentationLength))};
    bool sawR = false;
    for (std::size_t i = 1; i < augmentation.size(); ++i) {
        std::uint8_t encoding;
        switch (augmentation[i]) {
            case 'R':
                if (sawR || !args.read(cie.encoding) || cie.encoding != 0x1b) return false;
                sawR = true;
                break;
            case 'L':
                if (!args.read(encoding)) return false;
                break;  // FDE augmentation is length-delimited and not interpreted.
            case 'P': {
                // Only skip known fixed-width personality pointers; never dereference them.
                if (!args.read(encoding) || (encoding & 0x70U) != 0x10U) return false;
                const auto format = encoding & 0x0fU;
                if (format != 0x03 && format != 0x0b) return false;
                std::uint32_t pointer;
                if (!args.read(pointer)) return false;
                break;
            }
            case 'S': break;
            default: return false;
        }
    }
    if (!sawR || args.pos != args.bytes.size()) return false;
    reader.pos += static_cast<std::size_t>(augmentationLength);
    cie.augmented = true;
    return true;
}
}  // namespace

UnwindTable parseEhFrame(std::span<const std::byte> bytes, std::uint64_t address) {
    UnwindTable result;
    if (bytes.empty() || bytes.size() > 4 * 1024 * 1024 ||
        address > std::numeric_limits<std::uint64_t>::max() - bytes.size()) return result;
    std::map<std::size_t, Cie> cies;
    Reader input{bytes};
    std::size_t records = 0;
    while (input.pos < bytes.size()) {
        if (++records > 65536) return {};
        const auto start = input.pos;
        std::uint32_t length;
        if (!input.read(length)) return {};
        if (length == 0) {
            for (const auto byte : bytes.subspan(input.pos)) if (byte != std::byte{0}) return {};
            break;
        }
        if (length < 4 || length > 65536 || length > bytes.size() - input.pos) return {};
        const auto end = input.pos + length;
        Reader record{bytes.subspan(input.pos, length)};
        std::uint32_t cieDistance;
        if (!record.read(cieDistance)) return {};
        if (cieDistance == 0) {
            Cie cie;
            if (!readCie(record, cie)) { result.status = "unwind-cie-unsupported"; return result; }
            cies.emplace(start, cie);
        } else {
            const auto pointerOffset = start + 4;
            if (cieDistance > pointerOffset) return {};
            const auto cie = cies.find(pointerOffset - cieDistance);
            if (cie == cies.end()) return {};
            const auto pcField = address + start + 8;
            std::int32_t delta, size;
            std::uint64_t pc;
            if (!record.read(delta) || !record.read(size) || size <= 0 || size > 65536 ||
                !addSigned(pcField, delta, pc) || pc % 4 != 0 || size % 4 != 0 ||
                pc > std::numeric_limits<std::uint64_t>::max() - std::uint32_t(size)) return {};
            std::uint64_t augmentationLength;
            if (!cie->second.augmented || !record.uleb(augmentationLength) ||
                augmentationLength > record.bytes.size() - record.pos) return {};
            result.ranges.push_back({pc, static_cast<std::uint32_t>(size)});
        }
        input.pos = end;
    }
    if (result.ranges.empty()) return {};
    std::sort(result.ranges.begin(), result.ranges.end(), [](auto a, auto b) { return a.address < b.address; });
    for (std::size_t i = 1; i < result.ranges.size(); ++i) {
        const auto& previous = result.ranges[i - 1];
        if (result.ranges[i].address < previous.address + previous.size) {
            result.status = "unwind-overlapping-ranges";
            return result;
        }
    }
    result.valid = true;
    result.status = "unwind-bounded-ranges";
    return result;
}
}  // namespace sensor_probe
