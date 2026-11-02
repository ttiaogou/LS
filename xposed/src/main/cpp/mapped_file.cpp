#include "mapped_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sensor_probe {

MappedFile::MappedFile(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat info{};
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 64 * 1024 * 1024) {
        auto* address = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (address != MAP_FAILED) { mapping_ = address; size_ = static_cast<std::size_t>(info.st_size); }
    }
    close(fd);
}

MappedFile::~MappedFile() {
    if (mapping_ != nullptr) munmap(mapping_, size_);
}

std::span<const std::byte> MappedFile::bytes() const {
    return {static_cast<const std::byte*>(mapping_), size_};
}

}  // namespace sensor_probe
