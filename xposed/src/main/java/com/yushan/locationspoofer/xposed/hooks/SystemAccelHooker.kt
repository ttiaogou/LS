package com.yushan.locationspoofer.xposed.hooks

import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Process
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.yushan.locationspoofer.utils.GaitTemplate
import com.yushan.locationspoofer.xposed.BuildConfig
import com.yushan.locationspoofer.xposed.LocationHooker
import com.yushan.locationspoofer.xposed.utils.RouteEngine
import com.yushan.locationspoofer.xposed.utils.XposedBridge
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest
import java.util.Random
import kotlin.math.pow

/**
 * 全局方案的传感器层：在 system_server 里把 liblocation_accel.so 装进原生 SensorService，
 * 由它按 UID 改写/合成传感器事件。
 *
 * 为什么需要这一层：定位那套 Hook 改变的是 LocationManager 的返回值，而**步数**来自
 * SensorService（原生 C++，走 ISensorEventConnection 直接投递给应用），Java 层 Hook
 * SensorManager 只能覆盖自己被注入的进程。全局方案要"不选目标 App 也生效"，就必须在这一层做。
 *
 * 与 scoped 方案的 Java 版 [SensorStepHooker] 共用同一套模拟参数（步数、步频、真实度、步态模板），
 * 两条路径对同一时刻算出的步数一致，跨进程不会互相打架。
 */
internal class SystemAccelHooker(private val module: LocationHooker) {

    @Volatile
    private var hooked = false

    @Volatile
    private var disabled = true

    private var libraryHash: String = ""
    private var compatibility: String = ""
    private var stepHandleCount = 0
    private var lastDiagnosticAt = 0L
    private var carrierManager: SensorManager? = null
    private val carrierListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent?) = Unit
        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    /** UID 白名单缓存：包名集合没变就不重复查 PackageManager（这个函数每秒会被调用一次）。 */
    private var uidCacheKey: String = ""
    private var uidCache: IntArray = IntArray(0)
    private var uidCacheAt: Long = 0L

    fun install() {
        if (!BuildConfig.GLOBAL_SCHEME) return
        if (!Process.is64Bit()) {
            log("跳过：非 64 位进程（native 库只有 arm64-v8a）")
            return
        }
        try {
            libraryHash = sha256Of(File(SENSOR_SERVICE_LIBRARY))
            System.loadLibrary("location_accel")
            hooked = SystemAccelNative.install(libraryHash)
            compatibility = runCatching { SystemAccelNative.compatibilityStatus() }.getOrDefault("unknown")
            log(
                "native install=$hooked compatibility=$compatibility " +
                    "libsensorservice.sha256=${libraryHash.take(16)}…"
            )
            if (hooked) {
                // Never wait for SensorManager during system startup.
                log("backend=${BuildConfig.NATIVE_SENSOR_BACKEND} simulation=false sensor catalog skipped")
                runCatching { SystemAccelNative.diagnostics() }.getOrNull()?.let { log("diagnostics=${it.joinToString()}") }
            }
        } catch (t: Throwable) {
            log("装传感器层失败: ${t::class.java.simpleName}: ${t.message}")
        }
    }

    /** 由配置轮询线程每秒调用一次。 */
    fun update(config: JSONObject?) {
        if (!hooked) return
        if (BuildConfig.NATIVE_SENSOR_BACKEND != "steps") {
            val now = SystemClock.elapsedRealtime()
            if (now - lastDiagnosticAt >= 30_000L) {
                lastDiagnosticAt = now
                runCatching { SystemAccelNative.diagnostics() }.getOrNull()?.let {
                    log("backend=${BuildConfig.NATIVE_SENSOR_BACKEND} simulation=false diagnostics=${it.joinToString()}")
                }
            }
            return
        }
        val cfg = config
        if (cfg == null || !isSimulating(cfg)) {
            stopCarrier()
            if (!disabled) {
                runCatching { SystemAccelNative.disable() }
                disabled = true
                log("模拟停止，已关闭事件投递")
            }
            return
        }
        if (BuildConfig.NATIVE_SENSOR_BACKEND == "steps") {
            // Never enumerate/register sensors from install() during service startup.
            val booted = runCatching {
                Class.forName("android.os.SystemProperties")
                    .getMethod("get", String::class.java).invoke(null, "sys.boot_completed") == "1"
            }.getOrDefault(false)
            if (!booted) return
            if (stepHandleCount == 0) {
                val catalog = buildStepSensorCatalog()
                val accepted = runCatching { SystemAccelNative.configureStepSensors(catalog) }.getOrDefault(false)
                if (!accepted) return
                stepHandleCount = catalog.size / 2
                log("experimental steps: catalog=$stepHandleCount accepted=true")
            }
            if (!startCarrier()) return
        }
        disabled = false
        val now = System.currentTimeMillis()
        val bootNow = SystemClock.elapsedRealtimeNanos()
        val steps = runCatching { SensorStepHooker.calculateCurrentStepsFloat(cfg, now) }.getOrDefault(0.0)
        val cadence = cadenceOf(cfg, now)
        val speed = runCatching { RouteEngine.calculateCurrentPosition(cfg, now).speed.toDouble() }.getOrDefault(0.0)
        val uids = targetUids(cfg, now)
        val template = gaitTemplate(cfg)
        val pressure = basePressureHpa(cfg, now)
        try {
            SystemAccelNative.configureSensors(
                true,
                uids,
                steps,
                cadence,
                speed,
                bootNow,
                bootNow + VALIDITY_NANOS,
                cfg.optLong("start_timestamp", now),
                cfg.optInt("realism_level", 0),
                template,
                cfg.optBoolean("barometer_fluctuation", false),
                pressure
            )
            if (BuildConfig.NATIVE_SENSOR_BACKEND == "steps") {
                val elapsed = SystemClock.elapsedRealtime()
                if (elapsed - lastDiagnosticAt >= 30_000L) {
                    lastDiagnosticAt = elapsed
                    log("experimental steps: submitted/unconfirmed diagnostics=${SystemAccelNative.diagnostics().joinToString()}")
                }
            }
        } catch (t: Throwable) {
            log("configureSensors 失败: ${t::class.java.simpleName}: ${t.message}")
        }
    }

    fun close() {
        stopCarrier()
        if (!hooked) return
        runCatching { SystemAccelNative.disable() }
        hooked = false
    }

    private fun startCarrier(): Boolean {
        if (carrierManager != null) return true
        return runCatching {
            val context = SystemHookUtils.getSystemContext() ?: return false
            val manager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager ?: return false
            val sensor = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER, false) ?: return false
            // A low-rate physical stream drives sendEvents even when the phone is
            // stationary. It never invokes sendEvents on a retained raw receiver.
            val accepted = manager.registerListener(carrierListener, sensor, 200_000,
                Handler(Looper.getMainLooper()))
            if (accepted) carrierManager = manager
            accepted
        }.getOrElse {
            log("experimental steps: carrier registration failed: ${it::class.java.simpleName}")
            false
        }
    }

    private fun stopCarrier() {
        val manager = carrierManager ?: return
        carrierManager = null
        runCatching { manager.unregisterListener(carrierListener) }
    }

    // ---- 下发参数 ----------------------------------------------------------

    private fun isSimulating(config: JSONObject): Boolean =
        config.optBoolean("active", false) && config.optBoolean("enable_step_simulation", true) &&
            config.optDouble("speed_m_s", 0.0) > 0.05

    /**
     * 交给 native 做秒内插值的瞬时步频：与步数同源——位移对时间的导数 ÷ 步幅。
     *
     * 速度为 0（已到达终点或暂停）时返回 0，native 就不会再外推步数。
     */
    private fun cadenceOf(config: JSONObject, now: Long): Double {
        val speed = runCatching { RouteEngine.calculateCurrentPosition(config, now).speed.toDouble() }
            .getOrDefault(0.0)
        val stride = StepDistanceModel.strideMeters(
            config.optDouble("speed_m_s", 0.0),
            SensorStepHooker.targetCadence(config)
        )
        return StepDistanceModel.cadenceSpm(speed, stride)
    }

    /**
     * 生效的 UID 集合：全局模式放行所有普通应用（10000..19999），否则只用配置里勾选的包。
     * 两类都会剔除状态里登记的豁免包（含厂商适配层追加的）。
     */
    private fun targetUids(config: JSONObject, now: Long): IntArray {
        val globalMode = config.optBoolean("system_hook_global_mode", false)
        val configured = config.optJSONArray("system_hook_packages")?.let { arr ->
            (0 until arr.length()).mapNotNull { arr.optString(it) }.filter { it.isNotEmpty() }
        } ?: emptyList()
        val key = "$globalMode|${configured.joinToString(",")}"
        if (key == uidCacheKey && now - uidCacheAt < UID_CACHE_TTL_MS) return uidCache
        val context = SystemHookUtils.getSystemContext()
        val pm = context?.packageManager
        if (pm == null) return uidCache
        val packages = if (globalMode) installedPackages(pm) else configured
        val exempt = SystemHookUtils.EXEMPT_PACKAGES
        val uids = sortedSetOf<Int>()
        for (pkg in packages) {
            if (pkg.substringBefore(':') in exempt) continue
            val uid = applicationUid(pm, pkg) ?: continue
            if (uid % 100000 in 10000 until 20000) uids.add(uid)
        }
        uidCacheKey = key
        uidCache = uids.toIntArray()
        uidCacheAt = now
        return uidCache
    }

    private fun installedPackages(pm: PackageManager): List<String> = try {
        pm.getInstalledApplications(0).mapNotNull { it.packageName }
    } catch (t: Throwable) {
        emptyList()
    }

    private fun applicationUid(pm: PackageManager, pkg: String): Int? = try {
        if (Build.VERSION.SDK_INT >= 33) {
            pm.getApplicationInfo(pkg, PackageManager.ApplicationInfoFlags.of(0)).uid
        } else {
            @Suppress("DEPRECATION")
            pm.getApplicationInfo(pkg, 0).uid
        }
    } catch (t: Throwable) {
        null
    }

    /** 192 个 float = 三轴 × 64 采样；没有自定义模板时用仓库自己的程序化步态采一个跨步周期。 */
    private fun gaitTemplate(config: JSONObject): FloatArray {
        val decoded = GaitTemplate.decode(config.optString("gait_template", ""))
        if (decoded != null) {
            return FloatArray(GaitTemplate.SAMPLES * 3) { i ->
                when (i / GaitTemplate.SAMPLES) {
                    0 -> decoded.x[i % GaitTemplate.SAMPLES]
                    1 -> decoded.y[i % GaitTemplate.SAMPLES]
                    else -> decoded.z[i % GaitTemplate.SAMPLES]
                }
            }
        }
        val session = RouteEngine.realismSession(config)
        val speed = config.optDouble("speed_m_s", 1.4)
        val rng = Random(GAIT_SEED)
        return FloatArray(GaitTemplate.SAMPLES * 3) { i ->
            val axis = i / GaitTemplate.SAMPLES
            // 相位 0..2 步 = 一个跨步；MotionRealism 内部对模板也是按 stepsFloat/2 取相位
            val stepsFloat = (i % GaitTemplate.SAMPLES) / GaitTemplate.SAMPLES.toDouble() * 2.0
            session.accelerometer(stepsFloat, speed, null, rng)[axis]
        }
    }

    private fun basePressureHpa(config: JSONObject, now: Long): Double {
        val altitude = runCatching { RouteEngine.realisticAltitude(config, now) }.getOrDefault(0.0)
        return 1013.25 * (1.0 - altitude / 44330.0).pow(5.255)
    }

    /**
     * 硬件步数传感器目录：(mHandle, type) 成对，**wakeup 与 non-wake 两种变体都登记**。
     *
     * 应用订阅哪一种由它自己决定（`getDefaultSensor` 与 `getSensorList` 可能给出不同变体），
     * native 侧再按"连接实际持有的句柄"决定给谁投递；wakeup 事件的 ACK 位、唤醒锁与电量
     * 记账都由原 sendEvents 按句柄完成（设备库 0x94000 处 `Sensor::isWakeUpSensor()` +
     * `event[+0x58] |= 0x80000000`），适配器不参与这部分。
     */
    private fun buildStepSensorCatalog(): IntArray {
        val context: Context = SystemHookUtils.getSystemContext() ?: return IntArray(0)
        val manager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager ?: return IntArray(0)
        val out = ArrayList<Int>()
        var wakeUp = 0
        var nonWake = 0
        for (type in intArrayOf(Sensor.TYPE_STEP_DETECTOR, Sensor.TYPE_STEP_COUNTER)) {
            for (sensor in manager.getSensorList(type)) {
                val handle = sensorHandle(sensor) ?: continue
                if ((0 until out.size step 2).any { out[it] == handle }) continue
                out.add(handle)
                out.add(type)
                if (sensor.isWakeUpSensor) ++wakeUp else ++nonWake
            }
        }
        log("步数传感器目录: ${out.size / 2} 组（wakeup=$wakeUp non-wake=$nonWake）")
        return out.toIntArray()
    }

    private fun sensorHandle(sensor: Sensor): Int? = try {
        val field = Sensor::class.java.getDeclaredField("mHandle")
        field.isAccessible = true
        field.getInt(sensor)
    } catch (t: Throwable) {
        null
    }

    // ---- 杂项 --------------------------------------------------------------

    private fun sha256Of(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun log(message: String) {
        val line = "[GlobalSensor] $message (process=${Process.myPid()}/${module.javaClass.simpleName})"
        XposedBridge.log(line)
        android.util.Log.i("LocationSpoofer", line)
    }

    private companion object {
        const val SENSOR_SERVICE_LIBRARY = "/system/lib64/libsensorservice.so"

        /** 每次下发给 native 的有效期；配置线程每秒都会续期，这里给足余量即可。 */
        const val VALIDITY_NANOS = 24L * 60L * 60L * 1_000_000_000L
        const val UID_CACHE_TTL_MS = 5_000L
        const val GAIT_SEED = 0x5EEDL
    }
}
