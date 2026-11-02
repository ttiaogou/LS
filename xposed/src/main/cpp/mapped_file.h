#pragma once

#include <cstddef>
#include <span>

namespace sensor_probe {

class MappedFile {
public:
    explicit MappedFile(const char* path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    std::span<const std::byte> bytes() const;
private:
    void* mapping_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace sensor_probe
