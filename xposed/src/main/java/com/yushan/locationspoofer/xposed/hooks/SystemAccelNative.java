package com.yushan.locationspoofer.xposed.hooks;

/**
 * liblocation_accel.so 的 JNI 桥（类名/方法名必须与 native 导出符号严格一致，
 * 不要重命名类或方法，否则 System.loadLibrary 后调用会抛 UnsatisfiedLinkError）。
 *
 * native 侧实现：ShadowHook 内联挂钩 system_server 里 libsensorservice.so 的
 * android::SensorService::SensorEventConnection::sendEvents，按 UID 过滤并改写/合成
 * 步数计(19)/步测器(18)/加速度(1,10,35)/气压(6) 事件。
 */
public final class SystemAccelNative {

    private SystemAccelNative() {
    }

    /** 传入 /system/lib64/libsensorservice.so 的 SHA-256（小写十六进制），返回是否挂钩成功。 */
    public static native boolean install(String librarySha256);

    /**
     * 登记本机硬件步数传感器目录，元素为成对的 (sensorHandle, sensorType)。
     * 不登记就不会产生任何步数事件。
     */
    public static native boolean configureStepSensors(int[] handleTypePairs);

    /**
     * 下发当前模拟状态。
     *
     * @param active               是否正在模拟
     * @param targetUids           生效的应用 UID（仅 10000..19999 的普通应用 UID 会被采纳）
     * @param steps                当前累计步数（整数部分为步数，小数部分为相位）
     * @param cadenceSpm           步频（步/分钟）
     * @param speedMps             速度（米/秒）
     * @param nowBoottimeNanos     CLOCK_BOOTTIME 当前值（纳秒）
     * @param expiryBoottimeNanos  本次下发失效的 BOOTTIME 时刻（纳秒）
     * @param startTimestamp       模拟会话开始时刻（System.currentTimeMillis）
     * @param realismLevel         真实度等级
     * @param gaitTemplate         192 个 float（三轴 × 64 采样）的跨步周期模板，可为 null
     * @param barometerFluctuation 是否叠加气压抖动
     * @param basePressureHpa      基准气压（百帕）
     */
    public static native void configureSensors(boolean active, int[] targetUids, double steps, double cadenceSpm,
                                               double speedMps, long nowBoottimeNanos, long expiryBoottimeNanos,
                                               long startTimestamp, int realismLevel, float[] gaitTemplate,
                                               boolean barometerFluctuation, double basePressureHpa);

    /** 当前已观察到的连接 UID（诊断用）。 */
    public static native int[] uids();

    /** [事件计数, 已投递 UID 数, ...] 诊断计数（诊断用）。 */
    public static native long[] diagnostics();

    /** 停止投递（保留 hook）。 */
    public static native void disable();

    /** 结构校验/兼容性状态串，如 known-profile / matched / uid-constructor-c2-mismatch。 */
    public static native String compatibilityStatus();
}
