# Standalone ARM64 Device Core Tests

Date: 2026-10-15, approximately 20:44-20:45 Asia/Shanghai.
Device: Redmi 22127RK46C / socrates, Android 16, arm64-v8a, root ADB shell.
Core source checkpoint: `6adaf26`; a default-off CMake option was added to build
the existing pure-core test executables for Android.

## Scope

These binaries executed on the actual phone, not an emulator or host process.
They validate ARM64 arithmetic, core control-flow/ELF parsing and NDK-checked
event layout. They do not install hooks, open a sensor queue, read connection
objects or send synthetic steps to an application. The passthrough suite uses a
fake original function, not a live SensorService connection. No mock-engine
hook-installation tests were run on the phone.

The device binaries are ordinary Debug builds, without device ASan/UBSan.
The separate host sanitizer run still passed all twelve CTest cases.

## Results

All eight executables returned exit code 0 through ADB:

| Executable | Output |
| --- | --- |
| `uid_pattern_test` | 9 UID pattern checks passed |
| `uid_receiver_test` | 44 receiver provenance checks passed |
| `eh_frame_test` | 2146 assertions, 5000 mutations passed |
| `code_closure_test` | 7924 code-closure checks passed, including 2000 mutations |
| `elf_function_test` | 14 ELF boundary checks and 5000 mutations passed |
| `passthrough_test` | 7 forwarding contract checks passed |
| `step_stream_test` | 2309 assertions, including 2000 transport/capacity ticks passed |
| `step_wire_test` | 146 event-encoding assertions passed |

`sensor_probe_file /system/lib64/libsensorservice.so` also returned 0 on-device:

```text
hot:  0x94000, size 1488
cold: 0x9bd00, size 1588
closureStatus=code-direct-closure-bounded fragments=2 bounded=true
closureReceiver=receiver-alias-proven-subset offset=56
runtimeVerified=false hookInstalled=false memoryAccessAuthorized=false
```

This last command reads the device file only; it does not compare loaded memory.
The earlier in-system_server probe's runtime verification is a separate test.

## Build and Run

```sh
cmake -S xposed/src/main/cpp -B /tmp/locationspoofer-device-coretests \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Debug \
  -DSENSOR_PROBE_DEVICE_TESTS=ON -DSENSOR_PROBE_PASSTHROUGH=OFF
cmake --build /tmp/locationspoofer-device-coretests --parallel 4
```

NDK used: `28.2.13676358`. Only the eight executable tests and file probe were
pushed, not `liblocation_accel.so`. The new directory was first checked absent,
then created at `/data/local/tmp/locationspoofer-coretests-6adaf26`.
Each binary was pushed with `adb -s 177d3f92 push` and invoked directly with
`adb -s 177d3f92 shell /data/local/tmp/locationspoofer-coretests-6adaf26/<name>`.
ADB preserved executable permissions; no chmod or root escalation was issued.
Temporary test files are retained for reproduction, not installed as a module.

Host/device SHA256 values match:

```text
step_stream_test 77c54ae0dbf4411b86980b31f2e49f1a70771afd45c2510f1ea038a9c7f96452
step_wire_test   e93fabfe24f47a738bdf4f7655c1d1cabd2a09f9772a1d436a8153926d90b873
```

## System State and Remaining Acceptance

System boot was complete before/after testing. system_server remained PID 2953;
installed package remained `3.0.0-native-probe`. No APK was replaced, module
settings changed or reboot issued. The queried crash buffer showed no new
matching test/system_server crash. The newest ANR file was a WhatsApp trace at
20:23:52, before this test, and was not attributed to the test programs.

This is device execution of the planning/encoding core, NOT end-to-end step
simulation acceptance. UID semantics/lifetime, real per-connection subscriptions,
wake-up/flush handling, transport acknowledgment and actual application sensor
callbacks remain unverified or unimplemented. No synthetic event was delivered.
