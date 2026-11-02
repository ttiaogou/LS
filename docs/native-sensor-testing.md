# Native Sensor Diagnostic Builds

> Historical device experiments, including failed builds. This is not the current
> setup guide. See [GLOBAL_SENSOR.md](../GLOBAL_SENSOR.md) for supported backends,
> configuration recovery and outstanding device tests.

These are development builds, not a replacement for the working sensor simulator.
The personal AMap key remains in app settings; it is not needed to compile the
native diagnostic layer and is not copied into this repository.

## Latest Device Result: Cold-Closure Probe

On 2026-10-15, with explicit user approval, installed the cold-closure probe
APK (SHA256 `92adb63d28881b0312ebb000b4ac9fbb66f6c3123c8c070648f70e9434cffecc`)
using `adb install -r` while LSPosed was running, then rebooted. Android completed
boot and system_server remained PID 2953 during the short observation.
At 20:19:59 its LSPosed log reported:

```text
native install=false
receiver=receiver-alias-proven-subset; receiverOffset=56
closure=code-direct-closure-bounded; fragments=2; runtimeVerified=true
memoryAccess=false
```

Thus both recovered code fragments matched loaded RX memory and the conditional
AAPCS64 receiver-alias analysis passed on the device. `install=false` is expected
for this probe: no native hook or connection-memory access is enabled. This does
not establish the UID field's semantic validity, object lifetime, subscription
ownership or synthetic event delivery. The queried crash/ANR evidence showed no
new matching module/system_server failure. A separate init SIGABRT was present;
it is not attributed to this module. Logs are retained in the ignored folder
`app/build/outputs/native-tests/device-20261015/cold-closure-probe-log`.
The currently installed package is `3.0.0-native-probe`, not the earlier
ten-minute-tested passthrough build. No synthetic sensor simulation is active.

## Backend Choices

| `nativeSensorBackend` | Engine | Sensor behavior |
| --- | --- | --- |
| omitted / `steps` | Independent native step layer + source-built ShadowHook | Step simulation |
| `probe` | Our JNI/locator; no hook engine | No hook; reports symbol/layout candidates; install=false |
| `passthrough` | Our forwarding code + source-built upstream ShadowHook 2.0.1 | Installs a pure forwarding hook; no simulation |

`legacy` is unsupported and rejected during configuration. Old local prebuilt
sensor binaries are always excluded. Scoped retains its existing Java step
simulation. All debug/release builds are arm64-only and use the same application
ID and signing configuration; installing one overwrites the app of the same ID.
The version name is suffixed `-native-probe` or `-native-passthrough`.

The source-built engine provenance is in `third_party-shadowhook/UPSTREAM.md`.
Its pinned commit is `854c775c2c3676e57a0f383597ebf420b5204161` under MIT.
The new backends exclude `xposed/src/global/jniLibs`; no pickFirst precedence
is used to resolve conflicting engine binaries. The upstream CMake frontend is
not invoked; our wrapper builds the unmodified source files with CMake 3.22.1.
`--undefined-version` permits architecture-specific names in the upstream
shared ARM/ARM64 export list; the proxy library still uses `--no-undefined`.

## Build

Use the project's JDK 17 toolchain plus a JDK suitable for Gradle/AGP:

```sh
./gradlew -PnativeSensorBackend=probe :app:assembleGlobalDebug
./gradlew -PnativeSensorBackend=passthrough :xposed:testGlobalDebugUnitTest :app:assembleGlobalDebug
```

The standard output path is reused by Gradle. Copies made for this work:

- `app/build/outputs/reference/app-global-arm64-v8a-before-native-probe.apk`: pre-existing global package backup.
- `app/build/outputs/native-tests/app-global-arm64-v8a-native-probe-debug.apk`: locator-only build.
- `app/build/outputs/native-tests/app-global-arm64-v8a-native-passthrough-debug.apk`: forwarding build.

Verify the version name and package contents before installing. The probe must
contain no ShadowHook libraries. The passthrough package must contain our
`location_accel` and source-built ShadowHook, not the vendor engine.

## Device Acceptance Still Required

No package was installed by this development step. Installing a system-process
module and testing the hook requires user approval and a way to disable the
module if startup fails. Back up the working APK first. A fresh system process
is required after native library replacement; Java hot reload is not a native
library replacement mechanism.

Start with the probe: expect a `[GlobalSensorProbe]` locator report and
`native install=false`. Record library hash, address, function size and lookup
method. A reported UID candidate is not used to read connection memory.

Only after the probe is checked, test passthrough: expect
`passthrough-installed-no-simulation`. Open an existing sensor viewer without
starting route simulation. `[GlobalSensor]` reports counters every 30 seconds:
`[batches, 0, 0, 0, 0, inputEvents, negativeReturns, 0]`.
Negative returns are observed original return codes, not automatically a crash.
Check that real sensor values, timestamps and registration/unregistration still
work, including screen off/on and multiple clients. Check crash/ANR logs and
at least ten minutes of operation before M3 can be called device-validated.

The hook remains installed until process exit, including after `disable()`:
it is always a pure forwarder, and avoiding live unhook prevents trampoline
lifetime races. It does not implement fake steps, UID targeting, gait, pressure,
expiry or event rewriting. No event or connection memory is dereferenced.

## Remaining Work

An offline step-planning and step event-encoding core (wake-up and non-wake
handles) is now implemented and
tested; see [native-step-planner.md](native-step-planner.md). It is not called
by JNI or the sendEvents proxy. No synthetic event has been sent on the device.

M2 stripped-symbol structural recovery and semantic validation of the UID field
remain unfinished; receiver-alias analysis passed for the supplied device code.
M3 passed a ten-minute device smoke test, not full interactive acceptance.
M4 still needs verified UID/connection ownership, live subscription/flush handling
and transport acknowledgment semantics before offline step planning can be
connected to actual delivery. M5/M6 remain pending.
No closed-source pseudocode was copied into the new implementation.

### Receiver-Provenance Follow-up

After the validated passthrough checkpoint, a bounded AAPCS64 must-alias gate
was added, with 25 host regression cases. All eight CTest cases pass under
ASan/UBSan. The device ELF is rejected with
`receiver-control-flow-outside-extent`: sendEvents has direct branches to cold
code outside its 1488-byte symbol. This is an important limitation of treating
the exported symbol size as a complete control-flow extent. Full-function byte
comparison covers only that extent, not the external cold paths.

Compatibility status now carries the receiver proof result, independently of
whether pure forwarding was installed. No UID/object memory reads or synthetic
events are enabled. This follow-up has not been installed on the phone; the
ten-minute test above applies to the earlier no-catalog APK, not this revision.

The follow-up passthrough APK build and global JVM unit-test task passed.
An uninstalled copy is retained as
`app/build/outputs/native-tests/app-global-arm64-v8a-native-receiver-gate-debug.apk`:

```text
SHA256 16fb5208b49a68f2b60cd2e93b9ac52c6ea70ed8f3d8735504029ac0e176183e
```

### Cold-Fragment Recovery Follow-up

The locator now uses bounded `.eh_frame` FDE parsing to recover direct non-call
branch destinations outside the hot symbol, with no contiguous gap scanning.
Root symbol and FDE must agree; selected ranges must be non-overlapping,
file-backed RX, at most eight fragments and 16384 instructions in total.
Unknown metadata formats fail closed. The parser follows the
[LSB exception-frame layout](https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html)
for its explicitly supported DWARF32/PC-relative subset; CFI instructions are not
interpreted and FDE bounds do not prove function ownership.

Offline results on the supplied sample:

```text
hot:  0x94000..0x945d0, 1488 bytes
cold: 0x9bd00..0x9c334, 1588 bytes
closure: code-direct-closure-bounded, 2 fragments
receiver: receiver-alias-proven-subset, offset 56 (0x38)
memoryAccessAuthorized=false
```

The cold range is separately corroborated by the mini-debug symbol
`sendEvents...cold.0` and the FDE at section offset 0x7c. No decompressor or
mini-debug symbol dependency is added to Android: runtime discovery uses FDEs.
Receiver dataflow now runs across explicit virtual-address regions, handles
known memory writebacks and FP-to-GPR effects, and assumes AAPCS64 preservation
for BL/BLR calls. Indirect jumps, unmodeled instructions and incomplete paths
still refuse proof. At runtime, all selected fragments must match loaded RX
memory byte-for-byte before the multi-region result is reported.

All ten ASan/UBSan CTest suites pass, including 44 receiver scenarios,
5000 unwind-table mutations and 2000 ELF-closure mutations. Global JVM tests
and both Android diagnostic builds passed. This revision is not installed or
device-verified; neither UID reads nor synthesis has been enabled.

Retained APKs under `app/build/outputs/native-tests/`:

```text
app-global-arm64-v8a-native-cold-closure-probe-debug.apk
SHA256 92adb63d28881b0312ebb000b4ac9fbb66f6c3123c8c070648f70e9434cffecc
app-global-arm64-v8a-native-cold-closure-passthrough-debug.apk
SHA256 ce52e68bc4b39078dc62a2dd2b6a13f94c4e1a6394bab6cf305a22a9c0d90885
```

## Device Probe: 2026-10-15

With user approval, installed the copied probe APK using `adb install -r` and
rebooted. Device 22127RK46C runs Android 16; ADB shell has root access.
Boot completed and package version is `3.0.0-native-probe`.

LSPosed recorded in system_server PID 2952:

```text
[GlobalSensor] native install=false compatibility=probe-symbol-candidate-no-hook
libsensorservice.sha256=11a60e76f0db0173...
```

The full library hash matches the previously analyzed device sample. This status
means the locator passed the file-symbol, loaded RX range and code-equality
checks, and found a non-ambiguous UID-pattern candidate. It does not prove the
candidate's receiver provenance. The detailed native log line was not retained
in the queried logcat buffers, so runtime address, lookup method and numerical
UID offset were not captured. No system_server crash was found in the queried
crash buffer; unrelated startup service crashes were present. No passthrough APK
had been installed at the end of the probe test. M3 device acceptance is still pending.

## Passthrough Attempt: 2026-10-15

With separate user approval, `adb install -r` of the copied passthrough APK
succeeded and a reboot was issued. ADB did not reconnect within the 150-second
wait or the subsequent check. No post-reboot hook status, counters or crash logs
could be collected. The device screen state was requested from the user.
This is an unresolved connection/startup observation, not evidence that the hook
installed successfully or that a boot loop occurred. No further reboot or
configuration changes were issued. The installed package is expected to be the
passthrough build, but its running version has not been queried after reboot.

## Verification Record

### No-Catalog Revision Device Test

With explicit user approval, installed the no-catalog APK and enabled only
`zygisksu` and `zygisk_lsposed` using KernelSU's module CLI. Other root modules
remained disabled. The first boot completed but LSPosed attempted to parse a
removed previous APK path, so that boot did not validate the sensor hook.
Reinstalling the same APK while LSPosed was running refreshed package handling;
a subsequent reboot loaded the revision in system_server PID 2941.

At 19:37:05 the module reported successful passthrough installation, catalog
skipped and initial zero counters. Boot completed. At 19:39:42 counters reached
760 batches / 5699 input events / 0 negative returns, with unchanged PID. This
supports the startup-enumeration stall hypothesis but does not replace a blocked
thread stack as root-cause evidence. The user reported working location
simulation but no simulated steps, distance or cadence; lack of synthetic steps
and cadence is expected, while the reported distance display remains ambiguous.

ADB disconnected during continued observation. The planned ten-minute and
interactive real-sensor acceptance checks remain incomplete; do not label M3
fully accepted. No simulation implementation was enabled by this test.

ADB subsequently reconnected with root access. At 19:47:13, more than ten minutes
after installation at 19:37:05, system_server remained PID 2941 and diagnostics
reported 373527 batches / 380464 input events / 0 negative original returns.
The queried crash buffer contained no matching module/system_server crash, and
the newest ANR files predated this boot. Current logs were saved under
`app/build/outputs/native-tests/device-20261015/passthrough-no-catalog-log`.
The startup and ten-minute passive forwarding smoke test therefore passed on
this device. Interactive validation of values, timestamps, screen-off/on and
multiple-client registration remains unconfirmed; this is not full M3 acceptance
or evidence that synthetic step delivery works.

### Follow-up After Recovery

ADB/root access returned. Current system_server PID 2886 had no module APK
native mapping; both `zygisk_lsposed/disable` and `zygisksu/disable` were present.
The retained earlier LSPosed log shows PID 2926 reporting
`native install=true compatibility=passthrough-installed-no-simulation` at
19:16:13, but no sensor-catalog or diagnostic message afterward. Background
module messages continued until at least 19:23, so this was not evidence of an
immediate process crash. No matching new tombstone or PID 2926 ANR file was found
in the queried directories. Logs were copied to the ignored build-output folder
`app/build/outputs/native-tests/device-20261015/lspd-log`.

Code inspection found that a successful diagnostic install unnecessarily called
`buildStepSensorCatalog()` synchronously on the system_server main thread. Its
SensorManager lookup/enumeration may wait for startup services. This is a
plausible stall point, not a confirmed root cause without a blocked thread stack.
Diagnostic backends now skip that lookup; legacy behavior is unchanged.
The global unit-test task and APK build passed. The new APK has not been
installed or device-validated, and disabled modules have not been re-enabled.

```text
app-global-arm64-v8a-native-passthrough-no-catalog-debug.apk
SHA256 6add25b0a0e91dc8cb7ef1bfd0d017915cf456aa9dc32b429bc6d219137390d4
```

- Global debug unit tests and both native diagnostic APK builds passed.
- The default scoped debug build and its unit tests passed.
- All seven host CTest cases passed with ASan/UBSan: UID pattern, ELF parsing,
  forwarding contract, hook success, initialization failure, hook failure and
  missing-trampoline rollback. Mock engine tests do not validate Android patching.
- `git diff --check` passed. Initial builds had no connected ADB device;
  the later locator-only device test is recorded above. Passthrough behavior
  remains unverified on the device.

SHA-256 of the copied APKs:

```text
probe        fb58c858b180fe717aa6672d89d412327b0401b7091a99fa83a086774446dfe7
passthrough  507dbd746c86c24a49a805a319a86b95db260fda53e8583d1ebace16611e1206
backup       10e1f6d097ef16ac3b9ae0f977f68b1acfc17c94beaa1eb5811897f5dfa4e39d
```
