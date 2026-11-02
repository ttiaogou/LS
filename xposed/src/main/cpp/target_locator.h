#pragma once

#include "uid_pattern.h"
#include "uid_receiver.h"

namespace sensor_probe {

struct TargetReport {
    void* address = nullptr;
    UidCandidate uid;
    UidReceiverProof receiver;
    std::size_t functionSize = 0;
    const char* via = "none";
    const char* status = "not-probed";
    const char* closureStatus = "code-closure-not-checked";
    std::size_t codeFragments = 0;
    bool runtimeFragmentsVerified = false;
};

TargetReport inspectLoadedTarget();
// Resolve from the exact on-disk service ELF and loaded read-only executable
// mapping, validating the complete symbol bytes. Does not need a dlopen handle.
void* resolveLoadedServiceFunction(const char* symbol);

}  // namespace sensor_probe
