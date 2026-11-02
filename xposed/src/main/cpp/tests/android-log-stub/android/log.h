#pragma once
// Host-test stand-in for the NDK logging header: only the entry point the diagnostic
// event dump uses, and it produces no output. The device build uses the real header.
#include <cstdarg>

enum { ANDROID_LOG_INFO = 4 };

inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
