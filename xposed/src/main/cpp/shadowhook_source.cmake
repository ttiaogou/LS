# Build the unmodified, pinned upstream sources without its newer CMake frontend.
set(SHADOWHOOK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party-shadowhook")
file(GLOB SHADOWHOOK_SOURCES CONFIGURE_DEPENDS
    "${SHADOWHOOK_ROOT}/*.c"
    "${SHADOWHOOK_ROOT}/arch/arm64/*.c"
    "${SHADOWHOOK_ROOT}/arch/arm64/*.S"
    "${SHADOWHOOK_ROOT}/common/*.c"
    "${SHADOWHOOK_ROOT}/third_party/*/*.c")
add_library(shadowhook SHARED ${SHADOWHOOK_SOURCES})
target_compile_features(shadowhook PRIVATE c_std_11)
target_include_directories(shadowhook PUBLIC "${SHADOWHOOK_ROOT}/include" PRIVATE
    "${SHADOWHOOK_ROOT}" "${SHADOWHOOK_ROOT}/arch/arm64" "${SHADOWHOOK_ROOT}/common"
    "${SHADOWHOOK_ROOT}/third_party/xdl" "${SHADOWHOOK_ROOT}/third_party/bsd" "${SHADOWHOOK_ROOT}/third_party/lss")
target_compile_options(shadowhook PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_libraries(shadowhook PRIVATE log)
target_link_options(shadowhook PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL
    -Wl,--gc-sections -Wl,--undefined-version "-Wl,--version-script=${SHADOWHOOK_ROOT}/shadowhook.map.txt")
add_library(shadowhook_nothing SHARED "${SHADOWHOOK_ROOT}/nothing/sh_nothing.c")
target_compile_options(shadowhook_nothing PRIVATE -Oz -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables)
target_link_options(shadowhook_nothing PRIVATE -Wl,-z,max-page-size=16384 -Wl,--strip-all
    -Wl,--as-needed -Wl,--hash-style=sysv -Wl,--build-id=none -Wl,-nostdlib -nostartfiles -nodefaultlibs)
