# ShadowHook Source Provenance

- Project: https://github.com/bytedance/android-inline-hook
- Tag: `v2.0.1`
- Tag object: `e6b23d6883feab6052feff8e3e6959d710845a92`
- Commit: `854c775c2c3676e57a0f383597ebf420b5204161`
- Imported directory: `shadowhook/src/main/cpp` (unmodified).
- License: upstream MIT license in `LICENSE`; retain embedded third-party notices.

This directory is from ByteDance upstream, not from LocationSpoofer APK binaries.
`xposed/src/main/cpp/shadowhook_source.cmake` builds these sources with the
installed Android CMake toolchain; the upstream CMakeLists.txt is retained but
not invoked. The application does not call the optional ShadowHook Java bridge.
