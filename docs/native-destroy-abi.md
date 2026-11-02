# Destructor ABI Crash: Root Cause and Fix

Status: root cause identified from the two device tombstones and the on-disk service
ELF; fix applied to our own hook with a compile-time guard; host tests pass
(18/18, ASan+UBSan). **Not yet re-tested on the device.**

## Conclusion

The crash was in our hook, not in ShadowHook, the ROM, or the transport design.
`SensorEventConnection::~SensorEventConnection()` (D2) on this ROM is the
virtual-base form and takes a second argument: `D2(this, VTT)`. Our proxy was
declared `void destroyProxy(void*)`, so when it called the trampoline it forwarded
only `x0` and left `x1` holding stale garbage. The original body then used that
garbage as the virtual-table-table pointer, wrote a bogus vptr into the object, and
faulted dereferencing it. Forwarding every integer argument register fixes it.

## Evidence chain

1. **Tombstone 10** (`steps-elf-resolver-tombstone_10.txt`, 21:47:21, system_server
   pid 2922, tid `HwBinder:2922_2`):
   `signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xffffffffffffffec`.
2. **Backtrace** (both tombstones identical in shape):

   ```
   #00 pc 0x98e38  libsensorservice.so  ~SensorEventConnection()+48
   #01 pc 0x43880  liblocation_accel.so  (BuildId c2a0f6cf…)
   #02 pc 0x98de4  libsensorservice.so  ~SensorEventConnection()+28
   #03 pc …        libutils.so           RefBase::decStrong(void const*) const
   ```

3. **Resolving frame 1**: the tombstone BuildId matches the unstripped artifact
   `sensor-native/build/intermediates/cxx/Debug/1u4914t4/obj/arm64-v8a/liblocation_accel.so`,
   where `nm` places `0x43880` inside `sensor_probe::(anonymous namespace)::destroyProxy(void*)`
   (symbol starts at `0x43860`). Frame 1 is therefore *our destructor proxy*, i.e. we
   called the destructor — not the reverse.
4. **Resolving frames 0 and 2** against the on-disk service ELF
   (`libsensorservice.so`, sha256 `11a60e76…`): `D0` (deleting destructor) = `0x98dc8`
   size 64, `D2` (complete-object destructor) = `0x98e08` size 248. So `0x98e38` =
   `D2+0x30` and `0x98de4` = `D0+0x1c`.
5. **D0 passes a VTT in x1** (disassembly, `tools/dis_destructor.py` in the local
   analysis bundle):

   ```
   D0+0x18   ldr x1,[x1,#40]      ; load the virtual-table table
   D0+0x1c   bl  <D2>             ; D2(this = x0, VTT = x1)
   ```
6. **D2 dereferences x1 before ever defining it**:

   ```
   D2+0x18   ldr  x8,[x1,#0]      ; x8 = VTT[0]
   D2+0x1c   mov  x21,x0
   D2+0x2c   str  x8,[x0,#0]      ; store it as this->vptr
   D2+0x30   ldur x8,[x8,#-0x18]  ; <-- crash PC
   D2+0x34   ldr  x9,[x1,#136]
   D2+0x38   str  x9,[x0,x8]
   D2+0x3c   ldr  x8,[x1,#144]
   ```
7. **The register dump reproduces the arithmetic exactly.** With a narrowed proxy the
   stale `x1` no longer holds the VTT:
   - tombstone 10: `x1 = 0x7633936b38`, so `D2+0x18` loads `x8 = 4` (dump: `x8 = 4`),
     then `D2+0x30` computes `4 − 0x18 = 0xffffffffffffffec` — the recorded fault address.
   - tombstone 11 (21:48:00, system_server PID 6194, tid `GnssDfs thread`, fault addr
     `0xffffffffffffffee`): `x8 = 6`, and `6 − 0x18 = 0xffffffffffffffee`.
   - Both dumps also show `x19 = x21 = x22 = x0` (D2's own moves) and `x20 = x1`
     (`mov x20,x1` at `D2+0x24`), confirming the prologue ran with the clobbered `x1`.
   Two crashes, two different stale values, both landing on the same instruction's
   fault address: this is conclusive, not a correlation.
8. Consequence visible in the trace itself: `D2+0x2c` wrote the bogus vptr into the
   object *before* faulting, so even a survivable variant would corrupt every
   connection being destroyed. The crash thread name differs between the two events
   (`HwBinder` vs `GnssDfs`), i.e. any thread that drops the last reference is affected.

## Fix

`xposed/src/main/cpp/step_hook.cpp`:

```cpp
using Destroy = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);

void destroyProxy(void* receiver, void* vtt, void* second, void* third, void* fourth, void* fifth,
    void* sixth, void* seventh) {
    invalidate();
    reinterpret_cast<Destroy>(originalDestroy)(receiver, vtt, second, third, fourth, fifth, sixth, seventh);
    const int saved = errno;
    invalidate();
    errno = saved;
}
```

Forwarding every integer argument register is harmless for a one-argument destructor
and required for the virtual-base form, so the hook no longer depends on which ABI the
ROM happens to use. `x1` is preserved end to end: ShadowHook hands us the original
argument registers, we forward them unchanged, and the trampoline resumes `D2` with
`x1` intact.

## ABI verification matrix

| Element | Shape on this ROM | How it is verified | Evidence |
|---|---|---|---|
| Destructor `~SensorEventConnection` | virtual-base `D2(this, VTT)` | compile-time: `Destroy` and `AllArgumentDestroy` are the same type, and so is `destroyProxy`; runtime: `destroyProxy` records every call and whether VTT was non-null, exposed as diagnostics slots 9/10 | `static_assert` x2 in `step_hook.cpp`; `steps-success` asserts `destructorInvocations()==1 && destructorVttObservations()==1`, and a null-VTT call does not count |
| Eight integer argument registers (x0-x7) | caller clobbers x1 before `D2` unless forwarded | compile-time guard plus the host test requiring the VTT and errno to survive | `steps-success` (`destroyedVtt == vtt`, `errno == ERANGE`); crash root cause in "Evidence chain" |
| `hasSensor` | `const` member, `(const this, handle) -> bool` (`_ZNK…hasSensorEi`) | compile-time: `HasSensor`/`AllArgumentHasSensor` and the proxy shape; host test: the live receiver and every catalog handle are forwarded, including the reserved-handle probe | `static_assert`s; `steps-success` asserts `hasSensorReceiver == &receiver`, `hasSensorHandle == 11`, `hasSensorSawReservedProbe` |
| `removeSensor` | member, `(this, handle) -> bool` | compile-time: `RemoveSensor`/`AllArgumentRemoveSensor` and the proxy shape; host test: arguments forwarded, result and errno retained | `static_assert`s; `steps-success` asserts `removedReceiver`/`removedHandle` and `errno == EDOM` |
| JNI export names | `Java_<class package>_SystemAccelNative_*` | derived from the Java class's own package and verified automatically: `sensor-native` passes `<xposed namespace>_hooks` (the class package) as `SENSOR_PROBE_JNI_PACKAGE`, and `:app:verifyJniExports` - wired into `check` and into the release workflow - compares the `native` declarations in `SystemAccelNative.java` with the dynamic symbols of the packed `liblocation_accel.so`, failing with a missing/extra list. Manual `location_accel` builds must pass the define; the source has an `#error` if it is absent | the rename (`cdf626a`) left the old underscore-form names behind, and the first derivation appended `_xposed_hooks_` on top of a namespace already ending in `xposed`, yielding `..._xposed_xposed_hooks_...`; the automated check catches both, and the global APK exports exactly the seven `Java_com_yushan_locationspoofer_xposed_hooks_SystemAccelNative_*` |
| Event layout | `ASensorEvent`, 104 bytes | compile-time `sizeof`/`offsetof` asserts against the NDK header, plus a real-device sample | `step_wire.cpp` asserts; `-PnativeSensorBackend=passthrough` raw step-event dump (see `docs/offset-adaptation.md` 4.5) |


The check reports "JNI 导出名一致：7 个方法" against the packed global APK; the seven methods are
`install`, `configureStepSensors`, `configureSensors`, `uids`, `diagnostics`, `disable` and
`compatibilityStatus`.

Host suite: 20/20 suites pass with `-DSENSOR_PROBE_SANITIZERS=ON`, including
`event_dump` (sampling contract) and `steps-success` (the ABI assertions above).

## Guard and verification

Two `static_assert`s pin both the trampoline alias and the proxy to a literal
eight-argument type. Verified in both directions:

| Variant | Result |
|---|---|
| Fixed source, host build with `-DSENSOR_PROBE_SANITIZERS=ON` | builds; **20/20** ctest suites pass (`steps-success` and `event_dump` included) |
| Proxy narrowed to one argument (alias left wide) | **compile error** — proxy `static_assert` fires |
| Alias and proxy narrowed (guard's literal type untouched) | **compile error** — trampoline `static_assert` fires |
| All three narrowed, i.e. the guard's reference type edited too | compiles (a guard cannot defeat deliberate removal) |

An earlier runtime-only assertion (`require(destroyedVtt == vtt, …)`) was measured
against a narrowed-proxy build and **passed**, because on the host compiler the second
argument survives by luck. That is why the guard is compile-time; the runtime check is
kept only as documentation of intent.

## Recommended structural follow-up

Consider hooking `D0` (`0x98dc8`, exported, `(this)` only) instead of `D2`. `D0` is
what `RefBase::decStrong` actually calls, it is the same invalidation point, and it has
no VTT parameter, so the lifetime hook stops depending on the destructor ABI
altogether. Keep the eight-argument forwarder regardless, since the class is
vendor-modified and the ABI may differ on other builds.

## Device re-test plan

1. Build `-PnativeSensorBackend=steps` from the fixed source; keep the known-good Probe
   APK (`92adb63d…`) available for an approved rollback.
2. Install/reboot, then verify: stable system_server PID for ≥10 minutes,
   `steps-installed-experimental`, and growing steps in a selected application.
3. Watch for any new tombstone; if the destructor path faults again, switch the
   lifetime hook to `D0` before further attempts.

## Residual unknowns

- Whether Xiaomi's `D2` takes `(this, VTT)` on every build (assumed; hence the
  unconditional eight-argument forwarding).
- Whether hooking `D0` changes any observed behaviour beyond invalidation.
- The two tombstones are from one ROM build; no other device evidence exists yet.
- Second-order question not yet answered: whether the connection is ever destroyed
  concurrently with `sendEvents` on another thread, which the epoch guard only
  partially covers.
