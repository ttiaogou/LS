#pragma once

#include <cstdint>

// Host-only API stub. Never linked into an Android build.
enum shadowhook_mode_t { SHADOWHOOK_MODE_UNIQUE = 1 };
constexpr std::uint32_t SHADOWHOOK_HOOK_WITH_UNIQUE_MODE = 2;
int shadowhook_init(shadowhook_mode_t mode, bool debuggable);
void* shadowhook_hook_func_addr_2(void* target, void* proxy, void** original, std::uint32_t flags, ...);
int shadowhook_unhook(void* handle);
