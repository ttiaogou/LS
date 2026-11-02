# Independent Global API Probe

Build only when explicitly requested:

```sh
./gradlew --offline -PincludeGlobalProbe=true :global-probe:assembleDebug
```

Package `dev.stepfaker.globalprobe` has no module dependency, Xposed metadata or
network permission. It requests fine/coarse location and activity-recognition
permissions only. Do not add it to LSPosed scope. It is excluded from default
project configuration unless `includeGlobalProbe=true` is passed.

With user consent, install its debug APK, grant those permissions, and launch
`dev.stepfaker.globalprobe/.ProbeActivity`. It samples GPS/network callbacks and
default step-counter/detector events in the foreground for up to 60 seconds,
then unregisters. Leaving the activity stops sampling; it keeps the screen on
only during the foreground sample. Start/Stop buttons allow another sample.

Logs use `GlobalClientProbe`. Coordinates and step values are written to local
logcat and shown on-screen, never uploaded. Do not publish raw captures. Record
the fresh PID/UID, sensor registration outcomes, callback counts and summary.
Check the client's process maps and LSPosed scope to establish whether the
module was injected; source independence alone does not establish that fact.

Compare received locations with the running route/system-server rewrite logs.
Callbacks alone do not prove fake coordinates were received. Physical movement
can generate real step events even when the module's native synthesis is off;
zero step events while stationary likewise does not prove a broken listener.
Current native probe/passthrough backends cannot pass synthetic-step acceptance.

No package has been installed by creation of this tool. Device deployment and
granting its permissions require user approval. This app does not start or
change the simulator's route, module settings or LSPosed scope.
