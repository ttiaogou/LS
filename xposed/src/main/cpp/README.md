# Native Sensor Backend

The Gradle default is the experimental `steps` backend: runtime-validated
SensorService hooks and per-connection step delivery. Standalone CMake defaults
to a diagnostic build with no installed hook. `probe` only inspects the target;
`passthrough` installs forwarding hooks and may sample real step events for
layout diagnostics, but does not generate simulated steps.

Gradle integration is selected through `nativeSensorBackend`; the repository default is
`steps`. `legacy` is unsupported and rejected during configuration. All supported
backends exclude the vendor jniLibs directory; passthrough builds pinned ShadowHook
upstream sources from `third_party-shadowhook`. See
[`docs/native-sensor-testing.md`](../../../../docs/native-sensor-testing.md).

Host tests:

```sh
cmake -S xposed/src/main/cpp -B /tmp/locationspoofer-probe-host
cmake --build /tmp/locationspoofer-probe-host
ctest --test-dir /tmp/locationspoofer-probe-host --output-on-failure
/tmp/locationspoofer-probe-host/sensor_probe_file /path/to/libsensorservice.so
```

Android build (use the installed NDK path):

```sh
cmake -S xposed/src/main/cpp -B /tmp/locationspoofer-probe-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
  -DSENSOR_PROBE_JNI_PACKAGE=com_yushan_locationspoofer_xposed_hooks
cmake --build /tmp/locationspoofer-probe-arm64
```

`SENSOR_PROBE_JNI_PACKAGE` 是承载 JNI 方法的类所在包（`<模块 namespace>.hooks`）。
Gradle 构建会自动传入；手工构建 `location_accel` 时必须显式给出，否则源码里的 `#error`
会报错。只跑核心测试（`-DSENSOR_PROBE_DEVICE_TESTS=ON`）或宿主测试时不需要它。
The output stays in the build directory. Gradle packages source-built libraries
from `sensor-native`; the old `src/global/jniLibs` directory is excluded.

Before M2 acceptance: obtain runtime evidence, prove the load base derives from
the connection receiver, and validate symbol ABI/call paths. If dlsym is hidden
by the linker namespace, the probe can use the disk ELF symbol's virtual address
with the loaded module base, but still requires executable-range and byte checks.
This is symbol-table fallback, not structural recovery of a stripped function.
Structural fallback is not implemented. A matching instruction triple is only a candidate, not
proof of a safe UID offset. Instruction relocation is delegated to ShadowHook;
receiver analysis only models known PAC/BTI register effects. Synthetic event
delivery is enabled only in the explicit steps build, after runtime validation.

## Receiver Provenance Gate

`uid_receiver.cpp` adds a bounded must-alias analysis starting with X0 as the
receiver under AAPCS64. Its small explicit instruction subset propagates exact
64-bit register copies, invalidates W-register/arithmetic/load destinations,
preserves only callee-saved aliases across calls, and intersects aliases at
control-flow joins until a fixed point. Known scalar data-processing, memory and
selected FP/SIMD instruction effects are modeled conservatively. BL/BLR calls
are assumed to obey AAPCS64; indirect jumps, unknown reachable instructions,
local direct calls and fallthrough outside supplied regions are rejected.
All non-call direct branches must land in an explicitly supplied region.
Stack spills are not recovered.
Even a successful subset proof does not establish object lifetime, UID semantics,
subscription ownership or permission to read connection memory.

The supplied device sample's single 1488-byte symbol returns
`receiver-control-flow-outside-extent`. `code_closure.cpp` now bounds the direct
branch destination closure using `.eh_frame` FDEs: eight fragments / 16384 words
maximum, root symbol/FDE agreement, non-overlapping FDEs, file-backed RX mappings,
and no writable code segments. It does not expand a contiguous scan across gaps.
The parser supports only DWARF32 zR/zPLR-style CIEs with direct PC-relative
signed-32 FDE starts. Missing/unsupported metadata is a refusal, not a heuristic
fallback. FDE bounds do not establish function ownership or resolve indirect
jumps. See the [exception-frame format](https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html).

The sample's closure contains `0x94000..0x945d0` and `0x9bd00..0x9c334`.
The cold range is independently corroborated by its FDE and the mini-debug
`sendEvents...cold.0` symbol. Across these two regions the offline analyzer
reports `receiver-alias-proven-subset`, offset `0x38`, under its ABI assumptions.
Runtime analysis compares every fragment's full bytes with the same loaded
module before using the multi-region result. This gate remains diagnostic only:
passthrough does not read object memory or alter events. Compatibility status
reports closure and runtime checks even when early native logs are unavailable.

The receiver test suite covers 44 cases, including multi-region control flow,
loops, merges, clobbers, writeback and unreachable patterns. Unwind parsing and
code closure have dedicated suites, including 5000 mutated metadata fixtures.

## Offline Step Core

`step_stream.cpp` provides per-lifetime/per-subscription-epoch step planning with
explicit all-or-none transport acknowledgment. `step_wire.cpp` encodes local,
step events for both the wake-up and non-wake handles with Android NDK layout
assertions. The steps build connects them to JNI and the proxy. Core tests do
not install hooks or access live connection objects. See
[`docs/native-step-planner.md`](../../../../docs/native-step-planner.md) for
freshness/backlog policies, acknowledgment limitations and integration blockers.
The host build currently runs twenty CTest cases, including lifecycle hook scenarios.

For standalone device execution, configure an Android build with
`-DSENSOR_PROBE_DEVICE_TESTS=ON -DSENSOR_PROBE_PASSTHROUGH=OFF`.
It builds nine pure-core test executables and the file-only probe. The option
defaults off and does not change APK behavior. Device run instructions/results
are in [`docs/native-device-core-tests.md`](../../../../docs/native-device-core-tests.md).

The Kotlin controller no longer supports the legacy prebuilt backend.
The passthrough library links its source-built engine and owns JNI_OnLoad, so
the optional upstream Java bridge is not invoked. The probe has no engine.
Do not silently replace the working global build with these diagnostic packages.

## Recorded Offline Check

Input: the user-supplied device `libsensorservice.so`, SHA-256
`11a60e76f0db01732cae419a4a1f3bd65d3725d550a2d8f2bcc7b67c8e7b3d79`.
The standalone parser reports virtual address/file offset `0x94000`, function
size `1488`, and one UID candidate at `0x38`. This does not establish live
module identity, receiver provenance, callback ABI, or that all sensor paths
are covered. No vendor-specific constructor fingerprints are used.

Host tests include 9 instruction-pattern checks, 14 ELF boundary checks, and
5000 deterministic mutated ELF fixtures, plus 7 passthrough contract checks.
Sanitizer build:

```sh
cmake -S xposed/src/main/cpp -B /tmp/locationspoofer-probe-asan -DSENSOR_PROBE_SANITIZERS=ON
cmake --build /tmp/locationspoofer-probe-asan
ctest --test-dir /tmp/locationspoofer-probe-asan --output-on-failure
```
