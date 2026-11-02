#include "target_locator.h"

#include <android/log.h>
#include <jni.h>
#include <cstdio>
#include <mutex>
#ifdef SENSOR_PROBE_STEPS
#include "step_hook.h"
#include <array>
#endif
#ifdef SENSOR_PROBE_PASSTHROUGH
#include "passthrough_hook.h"
#endif

namespace {
std::mutex reportMutex;
sensor_probe::TargetReport report;
}

#ifndef SENSOR_PROBE_JNI_PACKAGE
#error "SENSOR_PROBE_JNI_PACKAGE must be the underscore form of the JNI class package (see sensor-native/build.gradle.kts)"
#endif
#define JNI_CONCAT_INNER(a, b) a##b
#define JNI_CONCAT(a, b) JNI_CONCAT_INNER(a, b)
// Java_com_yushan_locationspoofer_xposed_hooks_SystemAccelNative_<method>
// SENSOR_PROBE_JNI_PACKAGE 是"类所在包"的下划线形式（含 ..._xposed_hooks），由构建传入；
// 这里只接类名与方法名，避免两边各拼一段而产生重复段。
#define JNI_METHOD(name) \
    JNI_CONCAT(JNI_CONCAT(Java_, SENSOR_PROBE_JNI_PACKAGE), JNI_CONCAT(_SystemAccelNative_, name))

// Own the root library's JNI entry; do not resolve the dependency's Java bridge.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

extern "C" JNIEXPORT jboolean JNICALL JNI_METHOD(install)(JNIEnv*, jclass, jstring) {
    std::lock_guard lock(reportMutex);
#ifdef SENSOR_PROBE_PASSTHROUGH
    // Avoid re-reading an entrypoint that this library has already patched.
#ifdef SENSOR_PROBE_STEPS
    if (sensor_probe::stepDeliveryInstalled()) return JNI_TRUE;
#else
    if (sensor_probe::passthroughInstalled()) return JNI_TRUE;
#endif
#endif
    report = sensor_probe::inspectLoadedTarget();
    __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe",
        "status=%s via=%s sendEvents=%p size=%zu uidCandidate=%d matches=%zu ready=false",
        report.status, report.via, report.address, report.functionSize, report.uid.offset, report.uid.matches);
    __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe",
        "receiverStatus=%s receiverOffset=%d memoryAccessAuthorized=false",
        report.receiver.status, report.receiver.offset);
    __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe", "closure=%s fragments=%zu runtimeVerified=%s",
        report.closureStatus, report.codeFragments, report.runtimeFragmentsVerified ? "true" : "false");
#ifdef SENSOR_PROBE_PASSTHROUGH
    if (report.address == nullptr) return JNI_FALSE;
#ifdef SENSOR_PROBE_STEPS
    const bool installed = sensor_probe::installStepDelivery(report);
#else
    const bool installed = sensor_probe::installPassthrough(report.address);
#endif
#ifdef SENSOR_PROBE_STEPS
    __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe", "%s", sensor_probe::stepDeliveryStatus());
#else
    __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe", "%s", sensor_probe::passthroughStatus());
#endif
    return installed ? JNI_TRUE : JNI_FALSE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL JNI_METHOD(configureStepSensors)(JNIEnv* env, jclass, jintArray pairs) {
#ifdef SENSOR_PROBE_STEPS
    if (pairs == nullptr) return JNI_FALSE;
    const auto count = env->GetArrayLength(pairs);
    if (count <= 0 || count > 32 || count % 2 != 0) return JNI_FALSE;
    std::array<jint, 32> values;
    env->GetIntArrayRegion(pairs, 0, count, values.data());
    if (env->ExceptionCheck()) return JNI_FALSE;
    return sensor_probe::configureStepCatalog({values.data(), static_cast<std::size_t>(count)}) ? JNI_TRUE : JNI_FALSE;
#else
    (void)env;
    (void)pairs;
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL JNI_METHOD(configureSensors)(
    JNIEnv* env, jclass, jboolean active, jintArray uids, jdouble steps, jdouble cadence,
    jdouble, jlong now, jlong expiry, jlong session, jint, jfloatArray, jboolean, jdouble) {
#ifdef SENSOR_PROBE_STEPS
    if (!active || uids == nullptr) { sensor_probe::disableStepDelivery(); return; }
    const auto count = env->GetArrayLength(uids);
    if (count > 4096) { sensor_probe::disableStepDelivery(); return; }
    std::array<jint, 4096> values;
    env->GetIntArrayRegion(uids, 0, count, values.data());
    if (env->ExceptionCheck()) { sensor_probe::disableStepDelivery(); return; }
    sensor_probe::configureStepSnapshot({true, static_cast<std::uint64_t>(session), steps,
        cadence, now, expiry}, {values.data(), static_cast<std::size_t>(count)});
#else
    (void)env; (void)active; (void)uids; (void)steps; (void)cadence; (void)now; (void)expiry; (void)session;
#endif
}

extern "C" JNIEXPORT jintArray JNICALL JNI_METHOD(uids)(JNIEnv* env, jclass) {
    return env->NewIntArray(0);
}

extern "C" JNIEXPORT jlongArray JNICALL JNI_METHOD(diagnostics)(JNIEnv* env, jclass) {
    // Steps uses 1/2 for unconfirmed synthesized / suppressed events. Probe and
    // passthrough leave these zero. Neither means an application acknowledged it.
    // 槽位 9/10（1 基）是析构器 ABI 的运行期证据：D2 调用次数与其中 VTT 非空的次数。
    // 槽位 11-16：接管对齐（偏移/物理基准/是否采纳）与事件构成（counter/脉冲/最后计数）。
    jlong counters[16]{};
#ifdef SENSOR_PROBE_PASSTHROUGH
    const auto& stats = sensor_probe::forwardingStats();
    counters[0] = static_cast<jlong>(stats.batches.load(std::memory_order_relaxed));
    counters[5] = static_cast<jlong>(stats.events.load(std::memory_order_relaxed));
    counters[6] = static_cast<jlong>(stats.failures.load(std::memory_order_relaxed));
#ifdef SENSOR_PROBE_STEPS
    counters[1] = static_cast<jlong>(sensor_probe::submittedStepEvents());
    counters[2] = static_cast<jlong>(sensor_probe::suppressedStepEvents());
    counters[8] = static_cast<jlong>(sensor_probe::destructorInvocations());
    counters[9] = static_cast<jlong>(sensor_probe::destructorVttObservations());
    counters[10] = static_cast<jlong>(sensor_probe::stepCounterShift());
    counters[11] = static_cast<jlong>(sensor_probe::stepPhysicalBase());
    counters[12] = sensor_probe::stepCounterBaseAdopted() ? 1 : 0;
    counters[13] = static_cast<jlong>(sensor_probe::stepCounterEvents());
    counters[14] = static_cast<jlong>(sensor_probe::stepDetectorPulses());
    counters[15] = static_cast<jlong>(sensor_probe::stepLastCounterValue());
#endif
#endif
    auto result = env->NewLongArray(16);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 16, counters);
    return result;
}

extern "C" JNIEXPORT void JNICALL JNI_METHOD(disable)(JNIEnv*, jclass) {
#ifdef SENSOR_PROBE_STEPS
    sensor_probe::disableStepDelivery();
#endif
}

extern "C" JNIEXPORT jstring JNICALL JNI_METHOD(compatibilityStatus)(JNIEnv* env, jclass) {
    std::lock_guard lock(reportMutex);
    const char* status = report.status;
#ifdef SENSOR_PROBE_PASSTHROUGH
    if (sensor_probe::passthroughInstalled()) status = sensor_probe::passthroughStatus();
#endif
#ifdef SENSOR_PROBE_STEPS
    status = sensor_probe::stepDeliveryStatus();
#endif
    char summary[512];
    std::snprintf(summary, sizeof(summary),
        "%s; receiver=%s; receiverOffset=%d; closure=%s; fragments=%zu; runtimeVerified=%s; memoryAccess=%s",
        status, report.receiver.status, report.receiver.offset, report.closureStatus,
        report.codeFragments, report.runtimeFragmentsVerified ? "true" : "false",
#ifdef SENSOR_PROBE_STEPS
        sensor_probe::stepDeliveryInstalled() ? "true-live-receiver-uid-only" : "false"
#else
        "false"
#endif
    );
    return env->NewStringUTF(summary);
}
