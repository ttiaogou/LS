# Native Step Planning Core

Standalone execution of these cores on the Android 16 device passed; see
[native-device-core-tests.md](native-device-core-tests.md). This does not change
the no-delivery integration status below.

The original checkpoint was offline-only. The opt-in experimental `steps` backend
now connects JNI configuration and sendEvents submission to this core; see
[native-step-delivery.md](native-step-delivery.md). Probe and passthrough retain
their no-simulation behavior. The new backend has not been installed or validated
on the phone, which remains on the cold-closure probe build.

## Planning Contract

Each `StepStream` belongs to one externally verified connection lifetime. Its
caller must supply a fresh nonzero lifetime identity, serialize
plan/transport/commit, and never reuse the identity for a recycled pointer.
This code does not discover or validate connection ownership itself.

`StepSubscription` contains explicit counter/detector handles and an increasing
epoch. Every change, including unsubscribe/resubscribe with identical handles,
needs a newer epoch. Active handles must be nonnegative and distinct. A global
sensor catalog is not evidence of a particular connection's subscription.

`StepSnapshot` carries the shared controller's absolute cumulative steps, cadence,
session tag, monotonic sample time and expiry. There is no hard-coded boot-step
baseline. Supported cadence is 0..600 steps/minute. Double-based planning is
bounded to exact integers 0..2^53-1; the separate wire encoder preserves uint64.

The planner projects only up to five seconds from a sample and never past its
expiry. This freshness limit is independent of the Kotlin controller's longer
validity field; the removed prebuilt legacy backend is not supported.
Inactive/expired snapshots produce a committable no-event pause transition.
Invalid numeric inputs, future/stale samples and clock rollback are refused.

Counters are emitted initially and when the accepted integer value changes;
they cannot decrease within a stream. Detector pulses follow integer crossings
with interpolated, increasing timestamps, never later than `nowNanos`.
Initialization, a new session/subscription, a gap over five seconds, an
implausible step jump, or a backlog over 32 steps rebases detector progress
without historical pulse backfill. Such discontinuities set `Batch.rebased`.

Each plan uses a caller-owned output span, emits at most 16 events, and performs
no allocations, locks, clock reads or transport operations. If the span fills,
accepted pulses advance only to their last crossing. The counter waits behind
pending earlier detector timestamps; a later plan drains the remainder.

## Acknowledgment Is Not sendEvents Success

Planning and encoding never mutate committed progress. `commit(batch, true)`
is valid only after the ENTIRE planned batch is known to be accepted by a future
transport. `false` must mean ZERO events were accepted, allowing deterministic
retry. Zero-event initialization/pause transitions need no transport I/O.
Owner, lifetime and revision checks reject foreign, recycled-lifetime, duplicate
and stale acknowledgments.

Unknown or partially accepted transport outcomes are not solved by this API.
Do not map a nonnegative sendEvents result directly to `true`, or a negative
result directly to an assumption that no side effects occurred. SensorService
filtering, caching and flush semantics must first be established. Prefix
acknowledgment or a different queue protocol may be necessary before integration.
Likewise, revision checks do not prevent duplicate I/O if callers transmit
multiple competing plans before committing; per-connection serialization is
mandatory.

`abandon(batch)` separately retires progress WITHOUT acknowledging delivery.
The experimental sendEvents adapter uses it after every attempted handoff, even
when original returns an error, because events may already be cached. This
deliberately trades potential loss for no replay of uncertain detector pulses;
it must never be reported as confirmed application receipt.

## Non-Wake Event Encoding

`encodeNonWakeStep` zero-initializes a 104-byte ARM64 event, sets version, handle,
type and timestamp, and writes a uint64 counter for type 19 or float 1.0 for
type 18. Bad input leaves the output unchanged. Flags and reserved bytes remain
zero. Android builds statically compare size, alignment and member offsets
against the installed NDK's `ASensorEvent` definition.

The encoder does not support wake-up sensor ACK flags, wake locks, flush metadata,
permission checks, queue registration or sensor handle validity. It is not
permission to pass a cast pointer into SensorService. A future adapter must
validate its exact ABI and transport rules, or explicitly refuse unsupported
sensor classes.

## Verification

All twelve CTest suites passed under ASan/UBSan. Step-stream tests cover
initialization, projection, monotonic counts, lifetime/epoch changes, failed and
stale acknowledgments, zero/small buffers, bounded backlog draining, sample
freshness, numeric/time limits and 2000 deterministic capacity/transport ticks.
Wire tests cover headers, uint64 versus float payloads, zeroed fields, rejected
inputs, and planning-to-local-buffer encoding without acknowledgment.

```sh
cmake -S xposed/src/main/cpp -B /tmp/locationspoofer-probe-asan -DSENSOR_PROBE_SANITIZERS=ON
cmake --build /tmp/locationspoofer-probe-asan
ctest --test-dir /tmp/locationspoofer-probe-asan --output-on-failure
./gradlew --offline -PnativeSensorBackend=passthrough :xposed:testGlobalDebugUnitTest :app:assembleGlobalDebug
```

The Android NDK compilation, full passthrough APK build and global JVM test task
passed. Standalone `:sensor-native:assembleDebug` AAR packaging could not finish
offline because `com.android.tools.lint:lint-gradle:32.3.1` was not cached; no lint
configuration was weakened. No new package was installed or device reboot issued
for this checkpoint.
