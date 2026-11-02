# StepFaker Step Simulation Integration

The scoped build now uses StepFaker's direct-listener scheduling approach,
adapted to LocationSpoofer's libxposed API 102 and existing configuration channel.
StepFaker itself is not modified or required at runtime.

## Control Mapping

| LocationSpoofer configuration | Behavior |
| --- | --- |
| `active` | Simulation session must be active. |
| `enable_step_simulation` | Enables step and existing accelerometer simulation. |
| `speed_m_s` | Values at or below 0.05 stop synthetic events. |
| `is_auto_cadence` | Uses the existing speed-to-cadence mapping when enabled. |
| `step_cadence_spm` | Manual cadence, clamped to the existing 60-240 SPM range. |
| `realism_level`, `speed_fluctuation_pct` | Modulates the speed along the path; steps and cadence follow it. |
| `gait_template` | Existing accelerometer template is retained. |

No StepFaker broadcast receiver, package allowlist, separate preferences, or GPS
getter hooks are carried over. LocationSpoofer remains the sole controller.

## Event Delivery

- Successful registrations are captured per listener and sensor.
- A 20 ms callback check emits events only when an integer step is due.
- Step counters share a cumulative process-local counter; detectors emit pulses.
- Steps are derived from the distance travelled along the simulated path, not from
  elapsed time: `steps = steps_at_session_start + travelled / stride`, where
  `stride = speed / (cadence/60)`. Cadence handed to the event scheduler is the
  derivative of the same model (`speed / stride`), so the two never drift apart.
  Reaching the destination (travelled capped) stops the count; a session reset
  rebases the distance baseline without rewinding steps.
- Real step events for the same captured listener are suppressed during simulation.
- Sensor-specific unregister calls stop only that sensor's ticker.
- Hot reload cancels old tickers and restores the counter and delivery progress.
- Config/session changes do not recalculate already accumulated steps.
- Existing accelerometer synthesis stays in place; unrelated sensor types are untouched.

This remains Java hooking inside scoped application processes. It is not native
SensorService injection and does not add global-build step simulation.

## Verification

Build and run module tests:

```sh
./gradlew :xposed:testScopedDebugUnitTest :app:assembleScopedDebug
```

The project requires JDK 17 for `core-geo` and a suitable JDK for Gradle/AGP.
`StepDistanceModelTest` covers the stride/cadence round trip, monotonic
accumulation, the capped-travel arrival case, session rebasing, and invalid inputs
(`StepCadenceClock` and its test were replaced by this model).

Device acceptance still requires checking manual/automatic cadence, stopping,
sensor-specific unregister, and hot reload. Logs tagged `[StepSimulation]`
distinguish ticker registration from first successful callback delivery.
The original mock-sensor discovery remains, but a failed Android registration
is not treated as successful and does not start a synthetic ticker.
