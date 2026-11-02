# Experimental Native Step Delivery

**UNSAFE DEVICE RESULT:** The ELF-resolver artifact installed on 2026-10-15
triggered repeated system_server SIGSEGVs in the SensorEventConnection destructor
path. Do not install/reuse it for step testing. Root cause found and fixed in source
on the same day (destructor-ABI mismatch in our own proxy, see
`docs/native-destroy-abi.md`); the artifact that crashed remains unsafe, and the
fixed build has not been device-tested yet. User-approved rollback to the
known Probe completed; boot and a one-minute stable-PID observation passed.

## Status

2026-10-15: `nativeSensorBackend=steps` (the repository default since the same date,
set in `gradle.properties`) adds the integration of the
independent step planner, event encoder, JNI configuration and SensorService
sendEvents hook. This is NOT real-device acceptance and NOT a claim of universal
global step support. Initial implementation was built without installation.
The first later installation/reboot and its failed installation gate are recorded
below. No LSPosed scope change or independent helper sampling was performed.

`steps` is the default; `legacy` is no longer supported. `probe` and `passthrough` do not synthesize events.
The new path uses the existing source-built ShadowHook, not the closed sensor
binary. Existing location code and personal map configuration are unchanged.

## Data Path

1. Installation requires runtime-verified hot/cold fragments and a unique receiver
   UID proof agreeing with the separate UID candidate. The extra hasSensor,
   removeSensor and D2 destructor symbols must resolve from the service ELF and
   match their complete runtime bytes in the loaded read-only executable mapping.
2. No SensorManager enumeration occurs during installation. The config worker
   waits for `sys.boot_completed=1`, then obtains the step handle/type pairs of both
   the wake-up and the non-wake variants. It retries if no supported catalog is
   available.
3. Active simulation registers a non-wake accelerometer at 5 Hz as a physical
   carrier. Its Java callback is empty. It drives the existing SensorService
   event loop, rather than calling a saved raw connection from a timer thread.
   Disable/close unregisters it. Registration and its effect on this ROM are
   unverified on device.
4. Existing controller code supplies absolute steps, cadence, BOOTTIME sample and
   expiry, session and selected application UIDs. Steps come from the distance
   travelled along the simulated path (`steps = session start + travelled / stride`,
   `stride = speed / (cadence/60)`), and the supplied cadence is that model's
   derivative, so the native intra-second extrapolation stays on the same curve and
   stops by itself when the travelled distance is capped at the destination.
   Five-second planner freshness still applies even though the existing controller
   sends a longer expiry.
5. The proxy reads UID only from a live receiver after the installation gate.
   It queries hasSensor for the current catalog (both variants). A catalog alone
   never authorizes delivery to an unsubscribed connection.
6. A bounded table stores at most 64 serialized connection streams. No-subscription
   clients do not occupy new slots. removeSensor and D2 hooks increment an atomic
   epoch around the original call; an in-flight lifecycle change refuses synthesis
   without discarding other connections' progress or counter alignment. D2 marks
   only the destroyed receiver for deferred reclamation under the stream mutex.
   A reused address therefore starts a new stream, while unsubscribe/resubscribe
   on a surviving connection retains its counter history and frozen offset.
   Lifecycle proxies never take the stream mutex, including re-entry from the
   original sendEvents path.
7. The adapter marks matching physical counter/detector events with handle -1
   only after hasSensor verifies that handle is absent. It preserves original
   slot indices and all other events byte-for-byte, appends planned events, and supplies a scratch buffer
   large enough for the expanded input. The original function still executes
   subscription, first-flush, AppOps, privacy and cache handling.

## Transport Semantics

The Android 16 downstream source shows that sendEvents may filter a whole batch,
cache it on socket error, or drop old cached events. Its status is not proof that
an application received the synthetic batch:
[SensorEventConnection.cpp](https://github.com/LineageOS/android_frameworks_native/blob/lineage-23.0/services/sensorservice/SensorEventConnection.cpp).
This is source evidence for the adapter design, not proof that Xiaomi's binary
has identical behavior.

The adapter calls `StepStream::abandon` after original returns, independently of
the status. It deliberately retires uncertain progress without claiming an ACK.
There is no retry of an uncertain batch, which prevents duplicate detector pulses
after caching but allows loss if filtering/cache overflow occurred. Diagnostics
slots 1/2 count synthesized submissions / suppressed physical events, NOT receipt; slots 9/10
are the destructor-ABI evidence - `D2` invocations and how many of them carried a non-null VTT
(see the matrix in `docs/native-destroy-abi.md`).
Original result and errno are retained.

For layout verification, build the diagnostic passthrough backend
(`-PnativeSensorBackend=passthrough`): it hooks and forwards without rewriting and dumps the
first real step events it sees (`raw step event:` lines on the `GlobalSensorProbe` tag, at most
eight per process). `probe` itself only inspects structure and never sees events, so it cannot
sample. Procedure: `docs/offset-adaptation.md` 4.5.

## Explicit Limits

- Wake-up and non-wake step handles are both planned. Wake-up semantics are handled
  by the ROM's own `sendEvents`, which classifies by handle and marks the events
  itself: the device binary at `0x94000` calls `Sensor::isWakeUpSensor()` (+0x427),
  `isWakeUpSensorEvent` (+0x483), `BatteryService::noteWakeupSensorEventImpl`
  (+0x499) and ORs the ACK bit into the flags word (+0x502: `event[+0x58] |=
  0x80000000`). The adapter therefore never fabricates ACK or wake-lock state, and
  the appended events enter the same accounting as physical ones. Measured after
  delivery on 2026-10-15: `WakeLock Status: not held` and `WakeLockRefCount 0` for
  every connection, including the two-variant client. A long soak is still the
  right way to rule out slow refcount drift.
  See [SensorService.cpp](https://github.com/LineageOS/android_frameworks_native/blob/lineage-23.0/services/sensorservice/SensorService.cpp).
- Metadata and its original weak-pointer flush map retain their indices. The
  appended records are never metadata and do not access that map. Metadata with
  no original map falls back unchanged, because selecting the filtering/scratch
  path in that case would dereference a missing map.
- Inputs over 256 events, unknown version, invalid/stale configuration, missing
  subscription, multiple subscribed handles of the same type, table exhaustion,
  reserved handle -1 already subscribed, or mutex contention fall back to original traffic. These are fail-open limits,
  not promises of complete replacement.
- Only live sendEvents calls submit. The carrier does not guarantee wake-from-
  suspend delivery; no new wake lock is acquired.
- No direct BitTube/socket writes, no sensor-privacy or AppOps bypass, no retained
  raw connection calls, no app process hooks, no accelerometer/barometer rewrite.
- Partial installation leaves the established sendEvents proxy in passthrough;
  newly installed lifecycle hooks are rolled back where possible.

## Verification

Host ASan/UBSan: 18 CTest scenarios passed, including batch construction,
enlarged scratch bounds, metadata index preservation / missing-map fallback, expired/invalid parameters, uncertain
error/no-replay, ownership retirement, receiver proof gates, actual proxy helper
integration with mocked platform functions, UID selection, subscription/lifetime
changes and lifecycle install rollback. Mocked tests do not validate ROM ABI,
boot behavior, carrier delivery, wake locks or actual client callbacks.

```sh
cmake --build /tmp/locationspoofer-probe-asan --parallel 4
ctest --test-dir /tmp/locationspoofer-probe-asan --output-on-failure
JAVA_HOME=/home/yushan/.gradle/jdks/jetbrains_s_r_o_-21-amd64-linux.2 \
./gradlew --offline --no-daemon \
  -Porg.gradle.java.installations.paths=/tmp/jdk-17.0.20.1+1 \
  :xposed:testGlobalDebugUnitTest :app:assembleGlobalDebug
```

Do not treat this package as the previous known-working global package. The
previous boot stall makes installation of any new system_server hook a separate
risk decision. The user's latest instruction is to finish global step delivery
before helper sampling; that sampling remains deferred. Wake-up support remains
unfinished; the flush-index implementation still requires ROM validation before
claiming the requested full global implementation.

## Read-Only Device Inventory

An existing-device `dumpsys sensorservice` query (not a new listener/sample) on
2026-10-15 confirmed both classes exist on socrates:

| Kind | Non-wake handle | Wake handle |
| --- | --- | --- |
| Step detector | `0x010000b5` | `0x010000b6` |
| Step counter | `0x010000bf` | `0x010000c0` |

The physical non-wake accelerometer is `0x0100000b`; no wake-up accelerometer
appeared in that directory. Handles are evidence only and are NOT hard-coded.
The backend now registers both pairs, so a client is served whichever variant it
holds. Presence in the catalog does not prove which variant a particular application
chooses, nor prove fake event delivery; the two-variant acceptance below is the
device evidence.

## Build Artifact

Full Android `steps` APK build and global JVM tests passed. Signature verification
passed with the existing debug certificate (no signing/map-key identity change).
The separately named artifact is intentionally experimental and was NOT installed:

`app/build/outputs/native-tests/app-global-arm64-v8a-native-steps-nonwake-experimental-debug.apk`

SHA-256: `72a03f8363a4c1df7b55ddf4ffa1fe95e1bdf772bcfcaf62d76e44b8ab38e7e3`

## First Installation And Resolver Fix

The user subsequently authorized installation and reboot. That original artifact
installed successfully; boot completed with system_server PID 2972. At 21:35:32
the runtime fragment/receiver proof passed, but `native install=false` and
`compatibility=steps-no-service` prevented any native step hook installation.
The phone was not stuck and no helper sampling was started.

Confirmed code-path issue: the main locator already supported file-symbol plus
loaded RX mapping validation when dlopen/dlsym failed. The new lifecycle lookup
incorrectly required a successful `dlopen(RTLD_NOLOAD)` handle anyway. Android
linker namespace isolation is a possible reason for that handle failure, but no
dlerror was captured, so the exact loader rejection is NOT established.

The fix resolves all three helper symbols from the exact service ELF and loaded
RX mapping, validating each full function against disk before returning its
address. Missing or changed symbols still refuse installation. This does not
load another service copy, remove byte validation, or use unverified offsets.
The native log now reports the step backend's failure status rather than the
unrelated passthrough status. The resolver fix has NOT been installed or rebooted
on the phone.

## ELF Resolver Artifact Device Failure

The user authorized installing/rebooting the resolver fix. Artifact SHA-256:
`f6a98ec0ba638f10e2fe4e98e0ffdb2de526c419b6d5850dd1a182d1a3bb8866`.
The gate now passed (`steps-installed-experimental`), confirming that
the old dlopen requirement was bypassed successfully. This is NOT boot acceptance.

system_server PID 2922 crashed at 21:47:21, and replacement PID 6194 also crashed
at 21:48:00. Both tombstones show SIGSEGV in
`SensorEventConnection::~SensorEventConnection()+48` (`libsensorservice.so+0x98e38`),
with our native library at frame 1 (`+0x43880`). The next observed PID was 10000.
`sys.boot_completed=1` therefore did not mean the system process was stable.
DIAGNOSED (2026-10-15, later): the defect was ours, not the ROM's or ShadowHook's.
The ROM's `~SensorEventConnection()` (D2) is the virtual-base form `D2(this, VTT)`,
while our proxy was declared `void(void*)` and therefore forwarded only `x0`; the
stale `x1` became a bogus VTT, `D2+0x2c` wrote it as the object's vptr, and `D2+0x30`
faulted on `[x8-0x18]` — reproducing both recorded fault addresses exactly
(`x8=4 → 0xffffffffffffffec`, `x8=6 → 0xffffffffffffffffee`). Fixed by forwarding
every integer argument register, pinned by two compile-time assertions. Full evidence
and re-test plan: `docs/native-destroy-abi.md`. Host tests pass (18/18, ASan+UBSan);
the fixed artifact is NOT yet installed on the device. No independent helper sampling
was performed.

Both tombstones were copied to ignored local artifacts:
`app/build/outputs/native-tests/device-20261015/steps-elf-resolver-tombstone_10.txt`
and `steps-elf-resolver-tombstone_11.txt`. They contain private process memory and
must not be committed/published. User approval was requested to reinstall the
known cold-closure Probe and reboot; no unapproved rollback was performed.

## Build Integration (default since 2026-10-16)

`gradle.properties` sets `nativeSensorBackend=steps`, so a plain
`:app:assembleGlobalDebug` already ships this layer:

- `versionName` ends in `-native-steps`;
- the APK carries `lib/arm64-v8a/liblocation_accel.so` built from `sensor-native`
  (695 KB) plus the ShadowHook AAR's `libshadowhook.so`;
- the JNI export package is not hardcoded in C++: `sensor-native` derives
  `SENSOR_PROBE_JNI_PACKAGE` from the `:xposed` module's Gradle `namespace` plus the Java class's
  `hooks` subpackage, so renaming the module cannot silently leave the exported
  `Java_…SystemAccelNative_*` names behind. Gradle builds pass it automatically, manual
  `location_accel` builds must pass it (the source has an `#error`), core-test builds need
  nothing, and `:app:verifyJniExports` compares the declarations with the symbols in the packed
  APK;
- the closed prebuilt blobs under `xposed/src/global/jniLibs/` are not inputs
  (`xposed/build.gradle.kts` empties `jniLibs`, and their digests differ from what is
  packaged);
- on device the module logs `backend=steps`, then `步数传感器目录: 4 组（wakeup=2
  non-wake=2）` and growing `diagnostics=…`, which also proves the install succeeded
  because the catalog is only configured after a successful hook install.

All scoped/global debug and release builds are **arm64-v8a only**. A 32-bit process
is skipped by native installation. `-PnativeSensorBackend=legacy` now fails during
configuration; it is not a supported fallback. Old local prebuilt sensor binaries
are always excluded, regardless of the selected diagnostic backend. Release CI
requires exactly two arm64 APKs (one scoped and one global); debug APKs use the
same ABI policy and are uploaded separately as Actions artifacts. All four
variants are built sequentially in CI. Signing settings are unchanged.

## Device Acceptance: Fixed Destructor ABI (2026-10-15 22:14)

Artifact: `app/build/outputs/apk/global/debug/app-global-arm64-v8a-debug.apk`
(`versionName=3.0.0-native-steps`, packaged `liblocation_accel.so` Build ID
`e58394a18a3e5095db4b27708a0230e6e4ff625b`), installed with `adb install -r` over the
approved Probe build. The known-good Probe `92adb63d…` was pulled from the device
first and kept for rollback.

Boot: `system_server` PID 2941 held from 22:14:08 through 22:15:15+ with
`sys.boot_completed=1`, and the crash buffer carried no `libsensorservice` /
`liblocation_accel` entry. The previous artifact crashed twice inside this same window.

Install status line:

```
native install=true compatibility=steps-installed-experimental;
receiver=receiver-alias-proven-subset; receiverOffset=56;
closure=code-direct-closure-bounded; fragments=2; runtimeVerified=true;
memoryAccess=true-live-receiver-uid-only; libsensorservice.sha256=11a60e76f0db0173…
experimental steps: submitted/unconfirmed diagnostics=24 → 768 → 1257 → 2724
```

End-to-end delivery measured with the independent probe app
(`dev.stepfaker.globalprobe/.ProbeActivity`, third-party UID):

```
GlobalClientProbe: SENSOR type=18 value=1.0 timestamp=…
GlobalClientProbe: SENSOR type=19 value=2821.0 … 2829.0   (one step per ~0.7 s)
GlobalClientProbe: SUMMARY elapsedMs=4778 locations=12 counterEvents=12 counterDelta=2829.0 detectorEvents=9
GlobalClientProbe: LOCATION provider=gps lat=23.06591829 lon=113.97334962 accuracy=1.6
```

Subscription side (`dumpsys sensorservice`): WeChat (`com.tencent.mm`, uid 10305) holds
`step_counter Non-wakeup 0x010000bf` connections, while the system's
`com.miui.server.stepcounter.StepCounterManagerService` (uid 1000) holds
`step_detector Wakeup 0x010000b6`. Only app UIDs in 10000..19999 receive synthetic
events; the system UID keeps real data.

Operational note for the next run: a freshly installed probe APK is in the `stopped`
state, so `am start -n <pkg>/<activity>` fails with "Activity class does not exist"
(stopped packages are excluded from intent resolution). Launch it with
`am start -f 0x20 …` or `monkey -p <pkg> -c android.intent.category.LAUNCHER 1`.

## Device Acceptance: Wake-up And Non-Wake Handles (2026-10-15 22:30)

Artifact: packaged `liblocation_accel.so` Build ID
`faaef999e5efc118aeffbd75555db9be316943e2` (`versionName=3.0.0-native-steps`).
`system_server` PID 2906 held from boot through the whole run, `sys.boot_completed=1`,
the crash buffer stayed clean, and `dumpsys sensorservice` reported
`WakeLock Status: not held` with `WakeLockRefCount 0` for every connection —
immediately and again 20 seconds after synthetic wake-up events were delivered.

The independent probe now registers **every** sensor of each step type instead of the
default one and reports the variant of each received event:

```
GlobalClientProbe: START pid=19438 uid=10345
GlobalClientProbe: SENSOR register type=19 wakeUp=false accepted=true name=step_counter  Non-wakeup
GlobalClientProbe: SENSOR register type=19 wakeUp=true  accepted=true name=step_counter  Wakeup
GlobalClientProbe: SENSOR register type=18 wakeUp=false accepted=true name=step_detector Non-wakeup
GlobalClientProbe: SENSOR register type=18 wakeUp=true  accepted=true name=step_detector Wakeup
GlobalClientProbe: SENSOR type=19 wakeUp=false value=2727.0 timestamp=215985583302
GlobalClientProbe: SENSOR type=19 wakeUp=true  value=2727.0 timestamp=215985583302
GlobalClientProbe: SENSOR type=18 wakeUp=false value=1.0    timestamp=216399080306
GlobalClientProbe: SENSOR type=18 wakeUp=true  value=1.0    timestamp=216399080306
GlobalClientProbe: SUMMARY elapsedMs=17424 locations=28 counterEvents=71 counterDelta=34.0
                   detectorEvents=66 counterNonWake=36 counterWakeUp=35
                   detectorNonWake=33 detectorWakeUp=33
```

Both counters advance in lockstep and both detectors pulse on the same timestamps, so
the fan-out is one planned event per subscribed handle rather than a shared event.

Host coverage: `steps-both-variants` (23 assertions) drives a four-handle catalog
through the adapter and requires one counter event per subscribed counter handle and
one detector pulse per subscribed detector handle, on the anchoring batch and on a
later batch that carries pulses.

## Device Acceptance: Stopping The Simulation Stops Delivery (2026-10-16 00:28)

The stop path was exercised from the app UI on the default (arm64, `steps`) build:

```
00:28:27  probe: counter 2726 for both wake-up and non-wake   (synthetic, ~2.2 steps/s)
00:28:54  [GlobalSensor] 模拟停止，已关闭事件投递
00:29:35  probe: counter 0.0 only, no detector pulses, no growth
```

Module diagnostics froze at `30160, 419, 5, 0, 0, 53559, 0, 0` and were unchanged 8 seconds
later (they had been growing by thousands every 6 seconds while the session ran);
`system_server` did not restart, no wake lock was held, and the crash buffer stayed empty.
Location rewriting stops with the same `active` flag.

The counter drops from the synthetic value to the physical one on stop, which is inherent
to suppressing real events and injecting synthetic ones; a client that accumulated the
synthetic value sees that drop once. Camera-ready smoothness would require keeping an
offset after stop, i.e. continuing to spoof, which contradicts "only while simulating".

## Operational Note: Replacing The Module APK Requires A Reboot (2026-10-15 22:44)

Reinstalling the module APK while it is injected leaves `system_server` in a state
where the Java hooks keep working but the step layer stops delivering. Observed on
this device after two `adb install -r` runs of a rebuilt APK:

- location rewriting continued (`SysLoc rewrote location`, 140 lines in 75 s) and the
  module still matched global callers, so the module looked healthy;
- in the same 75 s window there was **no** `[GlobalSensor]` line at all — no install
  line, no catalog line, no 30-second diagnostics line — and the independent probe
  received only physical values (`value=0.0`, `counterDelta=0.0`);
- `adb reboot` restored everything immediately: `native install=true
  compatibility=steps-installed-experimental`, `步数传感器目录: 4 组（wakeup=2
  non-wake=2）`, growing `diagnostics=4446, 0, 0, 0, 0, 4441, …`, and the probe again
  received lockstep wake-up and non-wake counters (2619 → 2621) with matching
  detector pulses.

LSPosed reported `load_kind: hot-reload` for that process, which is consistent with a
reloaded module instance not reinstalling the native layer, but the exact mechanism was
not determined and this note claims only the operational rule: **after replacing the
module APK, reboot before judging step delivery.**

Do not try to confirm the loaded library with `grep liblocation_accel /proc/<pid>/maps`:
with APK-embedded native libraries the mapping is attributed to `base.apk`, so the
library name never appears and the check reports a false negative.

## Approved Probe Rollback

The user approved rollback. The known cold-closure Probe APK (SHA-256
`92adb63d28881b0312ebb000b4ac9fbb66f6c3123c8c070648f70e9434cffecc`)
was installed with `adb install -r` successfully and the phone rebooted. App data
was retained. Installed version is `3.0.0-native-probe`.

At 21:50:19 the new system_server PID 2915 logged the diagnostic-only receiver
proof with `memoryAccessAuthorized=false`. After a further 60-second observation,
`sys.boot_completed=1` and PID remained 2915. The new boot's crash buffer showed
an init SIGABRT at 21:50:07, but no system_server crash during this observation.
This is a bounded recovery check, not a long-duration stability claim. No helper
sampling, native simulation or new lifecycle hooks were enabled after rollback.
