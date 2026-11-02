@file:Suppress(
    "UNUSED_PARAMETER",
    "UNUSED_VARIABLE",
    "UNNECESSARY_NOT_NULL_ASSERTION",
    "DEPRECATION",
    "NAME_SHADOWING",
    "FunctionName",
    "PrivatePropertyName",
    "SpellCheckingInspection",
    "RedundantUnitReturnType",
    "RemoveRedundantQualifierName",
    "OPT_IN_USAGE",
    "unused",
    "UnusedImport"
)

package com.yushan.locationspoofer.xposed.hooks

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.yushan.locationspoofer.utils.GaitTemplate
import com.yushan.locationspoofer.xposed.LocationHooker
import com.yushan.locationspoofer.xposed.utils.RouteEngine
import com.yushan.locationspoofer.xposed.utils.XposedHelpers
import com.yushan.locationspoofer.xposed.utils.XposedBridge
import org.json.JSONObject
import java.lang.reflect.Constructor
import java.lang.reflect.Field
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

object SensorStepHooker {

    data class CapturedSensorListener(
        val listener: Any,
        val sensor: Sensor?,
        val handler: Handler?
    ) {
        @Volatile var active = true
        var deliveredSteps = -1L
        var ticker: Runnable? = null
        var deliveryLogged = false
        var creationFailureLogged = false
    }

    val capturedListeners = CopyOnWriteArrayList<CapturedSensorListener>()
    private val injecting = ThreadLocal<Boolean>()

    // Keep the legacy reload slots; the distance model owns accumulation now.
    private const val INITIAL_BOOT_STEPS = 2350.0
    @Volatile private var stepBase: Double = INITIAL_BOOT_STEPS
    @Volatile private var lastStepsFloat: Double = INITIAL_BOOT_STEPS
    @Volatile private var lastStepInitTime: Long = 0L

    /** 上一次采样的累计位移（米）：步数只由它的增量推进，会话重置时只拉回基准不回退步数。 */
    @Volatile private var lastTraveled: Double = 0.0

    /**
     * 上一次生效的会话起点（配置里的 start_timestamp）。
     * 新会话或模块刚启动时看到的第一个会话，先把位移基准对齐到当前位置：
     * 步数只从"此刻之后新走的距离"增长，不会把会话开始到现在已经走过的位移一次性补上。
     */
    @Volatile private var lastSessionStart: Long = Long.MIN_VALUE

    private val noiseRng = java.util.Random()
    @Volatile private var cachedTemplateSource: String? = null
    @Volatile private var cachedTemplate: GaitTemplate? = null

    // 缓存虚拟 Sensor 实例
    private var mockStepCounterSensor: Sensor? = null
    private var mockStepDetectorSensor: Sensor? = null
    private var mockAccelerometerSensor: Sensor? = null

    // 跟踪 handle 到 sensor type 的映射
    private val handleToTypeMap = ConcurrentHashMap<Int, Int>()

    fun hookSensorStepSimulation(classLoader: ClassLoader) {
        val targetClasses = listOf(
            "android.hardware.SensorManager",
            "android.hardware.SystemSensorManager"
        )

        for (className in targetClasses) {
            val managerClass = XposedHelpers.findClassIfExists(className, classLoader) ?: continue

            // 1. Hook getDefaultSensor: 当设备无物理计步传感器时注入虚拟 Sensor
            try {
                XposedHelpers.hookAllMethods(managerClass, "getDefaultSensor") { chain, _ ->
                    val result = chain.proceed(chain.args.toTypedArray())
                    val type = (chain.args.firstOrNull() as? Int) ?: return@hookAllMethods result
                    if (result == null && (type == Sensor.TYPE_STEP_COUNTER || type == Sensor.TYPE_STEP_DETECTOR)) {
                        return@hookAllMethods getOrCreateMockSensor(type, classLoader)
                    }
                    return@hookAllMethods result
                }
            } catch (_: Throwable) {}

            // 2. Hook getSensorList
            try {
                XposedHelpers.hookAllMethods(managerClass, "getSensorList") { chain, _ ->
                    val result = chain.proceed(chain.args.toTypedArray())
                    val type = (chain.args.firstOrNull() as? Int) ?: return@hookAllMethods result
                    if (type == Sensor.TYPE_STEP_COUNTER || type == Sensor.TYPE_STEP_DETECTOR) {
                        if (result is List<*> && result.isEmpty()) {
                            val mock = getOrCreateMockSensor(type, classLoader)
                            if (mock != null) return@hookAllMethods listOf(mock)
                        }
                    }
                    return@hookAllMethods result
                }
            } catch (_: Throwable) {}

            // 3. Hook registerListener / registerListenerImpl
            val regMethods = listOf("registerListener", "registerListenerImpl")
            for (methodName in regMethods) {
                try {
                    XposedHelpers.hookAllMethods(managerClass, methodName) { chain, _ ->
                        val args = chain.args
                        var listener: Any? = null
                        var sensor: Sensor? = null
                        var handler: Handler? = null

                        for (arg in args) {
                            if (arg != null) {
                                if (arg is SensorEventListener || LocationHooker.hasTypeByName(arg.javaClass, "android.hardware.SensorEventListener")) {
                                    listener = arg
                                } else if (arg is Sensor) {
                                    sensor = arg
                                } else if (arg is Handler) {
                                    handler = arg
                                }
                            }
                        }

                        val result = chain.proceed(chain.args.toTypedArray())
                        if (listener != null && result == true) {
                            val targetSensor = sensor
                            if (targetSensor != null && (targetSensor.type == Sensor.TYPE_STEP_COUNTER || targetSensor.type == Sensor.TYPE_STEP_DETECTOR || targetSensor.type == Sensor.TYPE_ACCELEROMETER)) {
                                val targetHandler = handler ?: Handler(Looper.myLooper() ?: Looper.getMainLooper())
                                synchronized(capturedListeners) {
                                    if (!capturedListeners.any { it.listener === listener && it.sensor === targetSensor }) {
                                        val entry = CapturedSensorListener(listener, targetSensor, targetHandler)
                                        capturedListeners.add(entry)
                                        startListenerTicker(entry)
                                    }
                                }

                                val handle = getSensorHandle(targetSensor)
                                if (handle != null) {
                                    handleToTypeMap[handle] = targetSensor.type
                                }
                            }
                        }

                        return@hookAllMethods result
                    }
                } catch (_: Throwable) {}
            }

            // 4. Hook unregisterListener / unregisterListenerImpl
            val unregMethods = listOf("unregisterListener", "unregisterListenerImpl")
            for (methodName in unregMethods) {
                try {
                    XposedHelpers.hookAllMethods(managerClass, methodName) { chain, _ ->
                        val args = chain.args
                        val listener = args.firstOrNull { it != null && (it is SensorEventListener || LocationHooker.hasTypeByName(it.javaClass, "android.hardware.SensorEventListener")) }
                        val sensor = args.filterIsInstance<Sensor>().firstOrNull()
                        val result = chain.proceed(chain.args.toTypedArray())
                        if (listener != null) {
                            synchronized(capturedListeners) {
                                capturedListeners.filter { it.listener === listener && (sensor == null || it.sensor === sensor) }.forEach {
                                    stopListenerTicker(it)
                                    capturedListeners.remove(it)
                                }
                            }
                        }
                        return@hookAllMethods result
                    }
                } catch (_: Throwable) {}
            }
        }

        // Java queue dispatch inside the scoped app, not native SensorService.
        hookSensorEventQueue(classLoader)
    }

    private fun hookSensorEventQueue(classLoader: ClassLoader) {
        try {
            val eventQueueClass = XposedHelpers.findClassIfExists("android.hardware.SystemSensorManager\$SensorEventQueue", classLoader)
                ?: XposedHelpers.findClassIfExists("android.hardware.SystemSensorManager\$BaseEventQueue", classLoader)
                ?: return

            XposedHelpers.hookAllMethods(eventQueueClass, "dispatchSensorEvent") { chain, _ ->
                if (injecting.get() == true) return@hookAllMethods chain.proceed(chain.args.toTypedArray())
                val config = (XposedHelpers.module as? LocationHooker)?.readConfig()
                if (config != null && isSimulating(config)) {
                    val handle = chain.args.firstOrNull() as? Int
                    val values = chain.args.getOrNull(1) as? FloatArray

                    if (handle != null && values != null) {
                        val sensorType = handleToTypeMap[handle]
                        val speed = config.optDouble("speed_m_s", 0.0)

                        val queueListener = runCatching { XposedHelpers.getObjectField(chain.thisObject!!, "mListener") }.getOrNull()
                        val direct = capturedListeners.any { it.active && it.listener === queueListener && it.sensor?.type == sensorType }
                        // Only suppress the real stream belonging to a captured step listener.
                        if (direct && (sensorType == Sensor.TYPE_STEP_COUNTER || sensorType == Sensor.TYPE_STEP_DETECTOR) && speed > 0.05) {
                            return@hookAllMethods null
                        }
                        if (sensorType == Sensor.TYPE_STEP_COUNTER && values.isNotEmpty()) {
                            // 仅对明确判定的计步器传感器进行步数计算，严禁误判光感/距离传感器
                            val curSteps = calculateCurrentSteps(config)
                            values[0] = curSteps.toFloat()
                        } else if (sensorType == Sensor.TYPE_STEP_DETECTOR && values.isNotEmpty()) {
                            if (speed > 0.1) {
                                values[0] = 1.0f
                            }
                        } else if (sensorType == Sensor.TYPE_ACCELEROMETER && values.size >= 3) {
                            // 加速度计传感器：仅在运动中(speed > 0.1)叠加生理跑步/步行震动特征，静态时绝不篡改
                            if (speed > 0.1) {
                                applySyntheticVibration(values, config, speed)
                            }
                        }
                    }
                }
                return@hookAllMethods chain.proceed(chain.args.toTypedArray())
            }
        } catch (_: Throwable) {}
    }

    internal fun saveReloadState(): Array<Any?> = arrayOf(
        capturedListeners.map { arrayOf<Any?>(it.listener, it.sensor, it.handler, it.deliveredSteps) }.toTypedArray(),
        stepBase, lastStepsFloat, lastStepInitTime,
        mockStepCounterSensor, mockStepDetectorSensor, mockAccelerometerSensor,
        handleToTypeMap.map { arrayOf(it.key, it.value) }.toTypedArray(),
        lastTraveled, lastSessionStart
    )

    internal fun clearReloadState() {
        capturedListeners.forEach { stopListenerTicker(it) }
        capturedListeners.clear()
        handleToTypeMap.clear()
    }

    internal fun restoreReloadState(value: Any?) {
        val state = value as? Array<*> ?: return
        (state.getOrNull(0) as? Array<*>)?.filterIsInstance<Array<*>>()?.forEach {
            val entry = CapturedSensorListener(it[0]!!, it[1] as? Sensor, it[2] as? Handler ?: Handler(Looper.getMainLooper()))
            entry.deliveredSteps = (it.getOrNull(3) as? Long) ?: -1L
            capturedListeners.add(entry)
        }
        stepBase = state[1] as Double
        lastStepsFloat = state[2] as Double
        lastStepInitTime = state[3] as Long
        lastTraveled = (state.getOrNull(8) as? Double) ?: 0.0
        lastSessionStart = (state.getOrNull(9) as? Long) ?: Long.MIN_VALUE
        mockStepCounterSensor = state[4] as? Sensor
        mockStepDetectorSensor = state[5] as? Sensor
        mockAccelerometerSensor = state[6] as? Sensor
        (state[7] as? Array<*>)?.filterIsInstance<Array<*>>()?.forEach {
            handleToTypeMap[it[0] as Int] = it[1] as Int
        }
        capturedListeners.forEach { startListenerTicker(it) }
    }

    private fun currentConfig(): JSONObject? = (XposedHelpers.module as? LocationHooker)?.readConfig()

    private fun isSimulating(config: JSONObject): Boolean =
        config.optBoolean("active", false) && config.optBoolean("enable_step_simulation", true) &&
            config.optDouble("speed_m_s", 0.0) > 0.05

    private fun stopListenerTicker(entry: CapturedSensorListener) {
        entry.active = false
        entry.ticker?.let { entry.handler?.removeCallbacks(it) }
    }

    private fun startListenerTicker(entry: CapturedSensorListener) {
        val handler = entry.handler ?: return
        val sensor = entry.sensor ?: return
        if (sensor.type != Sensor.TYPE_STEP_COUNTER && sensor.type != Sensor.TYPE_STEP_DETECTOR) return
        val ticker = object : Runnable {
            override fun run() {
                if (!entry.active) return
                try {
                    val config = currentConfig()
                    if (config != null) {
                        val total = calculateCurrentSteps(config)
                        if (isSimulating(config)) {
                            if (entry.deliveredSteps < 0) entry.deliveredSteps = total
                            if (total > entry.deliveredSteps) {
                                val value = if (sensor.type == Sensor.TYPE_STEP_COUNTER) total.toFloat() else 1f
                                if (sensor.type == Sensor.TYPE_STEP_COUNTER) {
                                    if (deliver(entry, floatArrayOf(value))) entry.deliveredSteps = total
                                } else {
                                    // Preserve detector pulses across a briefly busy callback looper.
                                    for (i in 0 until (total - entry.deliveredSteps).coerceAtMost(16L).toInt()) {
                                        if (!deliver(entry, floatArrayOf(1f))) break
                                        entry.deliveredSteps++
                                    }
                                }
                            }
                        } else {
                            entry.deliveredSteps = total
                        }
                    }
                } catch (error: Throwable) {
                    XposedBridge.log("[StepSimulation] Ticker failed: " + error)
                } finally {
                    if (entry.active) handler.postDelayed(this, 20L)
                }
            }
        }
        entry.ticker = ticker
        handler.postDelayed(ticker, 20L)
        XposedBridge.log("[StepSimulation] Started direct ticker: " + entry.listener.javaClass.name + ", type=" + sensor.type)
    }

    private fun deliver(entry: CapturedSensorListener, values: FloatArray): Boolean {
        if (!entry.active) return false
        val config = currentConfig() ?: return false
        if (!isSimulating(config)) return false
        val sensor = entry.sensor ?: return false
        val event = createSensorEvent(sensor, values)
        if (event == null) {
            if (!entry.creationFailureLogged) {
                entry.creationFailureLogged = true
                XposedBridge.log("[StepSimulation] Cannot construct SensorEvent: type=" + sensor.type)
            }
            return false
        }
        injecting.set(true)
        try {
            if (entry.listener is SensorEventListener) entry.listener.onSensorChanged(event)
            else XposedHelpers.callMethod(entry.listener, "onSensorChanged", event)
            if (!entry.deliveryLogged) {
                entry.deliveryLogged = true
                XposedBridge.log("[StepSimulation] First event delivered: type=" + sensor.type + ", value=" + values[0])
            }
            return true
        } finally {
            injecting.remove()
        }
    }

    private fun applySyntheticVibration(values: FloatArray, config: JSONObject, speed: Double) {
        val now = System.currentTimeMillis()
        val stepsFloat = calculateCurrentStepsFloat(config, now)
        val motionSpeed = RouteEngine.calculateCurrentPosition(config, now).speed.toDouble().takeIf { it > 0.0 } ?: speed
        val synthetic = RouteEngine.realismSession(config).accelerometer(stepsFloat, motionSpeed, gaitTemplate(config), noiseRng)
        values[0] = synthetic[0]
        values[1] = synthetic[1]
        values[2] = synthetic[2]
    }

    /** 用户录制的步态模板（配置里的编码字符串），按字符串内容缓存解码结果 */
    private fun gaitTemplate(config: JSONObject): GaitTemplate? {
        val source = config.optString("gait_template", "")
        if (source != cachedTemplateSource) {
            cachedTemplate = GaitTemplate.decode(source)
            cachedTemplateSource = source
        }
        return cachedTemplate
    }

    /**
     * 计算当前仿真总步数
     */
    fun calculateCurrentSteps(config: JSONObject, now: Long = System.currentTimeMillis()): Long =
        calculateCurrentStepsFloat(config, now).toLong()

    /**
     * 累计步数：**由沿模拟路径走过的距离推出**（见 [StepDistanceModel]），整数部分是步数、
     * 小数部分是当前这一步的相位（供加速度波形对齐）。
     *
     * 与位置引擎共用同一个速度模型，因此：到达终点（位移封顶）步数即停；暂停续走不跳变；
     * 长时间运行不会像"时间 × 步频"那样在两套独立噪声下漂移。
     */
    internal fun calculateCurrentStepsFloat(config: JSONObject, now: Long): Double {
        if (!isSimulating(config)) return lastStepsFloat
        val sessionStart = config.optLong("start_timestamp", 0L)
        if (sessionStart != lastSessionStart) {
            // 第一次看到这个会话（或模块刚启动就撞上一个已经跑了几个小时的会话）：
            // 把基准对齐到当前位移，避免把之前累计的公里数一次性补成几万步。
            lastSessionStart = sessionStart
            lastTraveled = runCatching { RouteEngine.traveledDistance(config, now) }.getOrDefault(0.0)
            return lastStepsFloat
        }
        val traveled = runCatching { RouteEngine.traveledDistance(config, now) }.getOrDefault(lastTraveled)
        val stride = StepDistanceModel.strideMeters(config.optDouble("speed_m_s", 0.0), targetCadence(config))
        val (steps, baseline) = StepDistanceModel.advance(lastStepsFloat, traveled, lastTraveled, stride)
        lastStepsFloat = steps
        lastTraveled = baseline
        return steps
    }

    /** 配置的目标步频（自动或手动）；只用于把速度换算成步幅。 */
    internal fun targetCadence(config: JSONObject): Double =
        if (config.optBoolean("is_auto_cadence", true)) {
            calculateAutoCadence(config.optDouble("speed_m_s", 0.0)).toDouble()
        } else {
            config.optInt("step_cadence_spm", 165).coerceIn(60, 240).toDouble()
        }

    /**
     * 智能根据速度自动计算匹配的生理步频 (SPM, 步/分钟)
     */
    fun calculateAutoCadence(speedMs: Double): Int {
        return when {
            speedMs <= 0.8 -> 95 // 慢走
            speedMs <= 1.5 -> (100 + (speedMs - 0.8) / 0.7 * 25).toInt() // 快走 100~125 SPM
            speedMs <= 2.8 -> (135 + (speedMs - 1.5) / 1.3 * 25).toInt() // 慢跑 135~160 SPM
            speedMs <= 4.0 -> (160 + (speedMs - 2.8) / 1.2 * 20).toInt() // 匀速跑 160~180 SPM
            speedMs <= 5.5 -> (180 + (speedMs - 4.0) / 1.5 * 15).toInt() // 快跑 180~195 SPM
            else -> 200 // 冲刺 200 SPM
        }
    }

    /**
     * ConfigPoller keeps the clock current and supplies the existing accelerometer simulation.
     * Step sensors are driven independently at 20 ms, not by the 1-second location poller.
     */
    fun dispatchStepEvents(config: JSONObject, classLoader: ClassLoader) {
        calculateCurrentSteps(config)
        if (!isSimulating(config)) return
        for (entry in capturedListeners) {
            if (entry.sensor?.type != Sensor.TYPE_ACCELEROMETER) continue
            val values = floatArrayOf(0f, 0f, 9.8f)
            applySyntheticVibration(values, config, config.optDouble("speed_m_s", 0.0))
            entry.handler?.post {
                try { deliver(entry, values) } catch (error: Throwable) {
                    XposedBridge.log("[StepSimulation] Accelerometer callback failed: " + error)
                }
            }
        }
    }

    private fun getSensorHandle(sensor: Sensor): Int? {
        return try {
            val handleField = Sensor::class.java.getDeclaredField("mHandle")
            handleField.isAccessible = true
            handleField.getInt(sensor)
        } catch (_: Throwable) {
            null
        }
    }

    private fun getOrCreateMockSensor(type: Int, classLoader: ClassLoader): Sensor? {
        if (type == Sensor.TYPE_STEP_COUNTER && mockStepCounterSensor != null) return mockStepCounterSensor
        if (type == Sensor.TYPE_STEP_DETECTOR && mockStepDetectorSensor != null) return mockStepDetectorSensor
        if (type == Sensor.TYPE_ACCELEROMETER && mockAccelerometerSensor != null) return mockAccelerometerSensor

        try {
            val sensorClass = Class.forName("android.hardware.Sensor", false, classLoader)
            val constructor: Constructor<*> = sensorClass.getDeclaredConstructor()
            constructor.isAccessible = true
            val sensor = constructor.newInstance() as Sensor

            setSensorField(sensor, "mType", type)
            setSensorField(sensor, "mName", when(type) {
                Sensor.TYPE_STEP_COUNTER -> "Step Counter Sensor"
                Sensor.TYPE_STEP_DETECTOR -> "Step Detector Sensor"
                else -> "Accelerometer Sensor"
            })
            setSensorField(sensor, "mVendor", "Android")
            setSensorField(sensor, "mVersion", 1)
            setSensorField(sensor, "mResolution", 1.0f)
            setSensorField(sensor, "mPower", 0.05f)

            when (type) {
                Sensor.TYPE_STEP_COUNTER -> mockStepCounterSensor = sensor
                Sensor.TYPE_STEP_DETECTOR -> mockStepDetectorSensor = sensor
                Sensor.TYPE_ACCELEROMETER -> mockAccelerometerSensor = sensor
            }
            return sensor
        } catch (_: Throwable) {
            return null
        }
    }

    private fun setSensorField(sensor: Sensor, fieldName: String, value: Any) {
        try {
            val field: Field = Sensor::class.java.getDeclaredField(fieldName)
            field.isAccessible = true
            field.set(sensor, value)
        } catch (_: Throwable) {}
    }

    private fun createSensorEvent(sensor: Sensor, values: FloatArray): SensorEvent? {
        try {
            val eventConstructor = SensorEvent::class.java.getDeclaredConstructor(Int::class.javaPrimitiveType)
            eventConstructor.isAccessible = true
            val event = eventConstructor.newInstance(values.size) as SensorEvent
            System.arraycopy(values, 0, event.values, 0, values.size)

            val sensorField = SensorEvent::class.java.getDeclaredField("sensor")
            sensorField.isAccessible = true
            sensorField.set(event, sensor)

            val timestampField = SensorEvent::class.java.getDeclaredField("timestamp")
            timestampField.isAccessible = true
            timestampField.setLong(event, SystemClock.elapsedRealtimeNanos())

            val accuracyField = SensorEvent::class.java.getDeclaredField("accuracy")
            accuracyField.isAccessible = true
            accuracyField.setInt(event, 3) // SENSOR_STATUS_ACCURACY_HIGH

            return event
        } catch (_: Throwable) {
            try {
                // Fallback 1: search any constructors
                val constructors = SensorEvent::class.java.declaredConstructors
                for (ctor in constructors) {
                    ctor.isAccessible = true
                    val paramTypes = ctor.parameterTypes
                    val args = arrayOfNulls<Any>(paramTypes.size)
                    for (i in args.indices) {
                        if (paramTypes[i] == Int::class.javaPrimitiveType) args[i] = values.size
                    }
                    val event = ctor.newInstance(*args) as SensorEvent
                    System.arraycopy(values, 0, event.values, 0, values.size)
                    try {
                        val sf = SensorEvent::class.java.getDeclaredField("sensor")
                        sf.isAccessible = true
                        sf.set(event, sensor)
                        val tf = SensorEvent::class.java.getDeclaredField("timestamp")
                        tf.isAccessible = true
                        tf.setLong(event, SystemClock.elapsedRealtimeNanos())
                        val af = SensorEvent::class.java.getDeclaredField("accuracy")
                        af.isAccessible = true
                        af.setInt(event, 3)
                    } catch (_: Throwable) {}
                    return event
                }
            } catch (_: Throwable) {}

            try {
                // Fallback 2: sun.misc.Unsafe.allocateInstance
                val unsafeClass = Class.forName("sun.misc.Unsafe")
                val theUnsafeField = unsafeClass.getDeclaredField("theUnsafe")
                theUnsafeField.isAccessible = true
                val unsafe = theUnsafeField.get(null)
                val allocateMethod = unsafeClass.getMethod("allocateInstance", Class::class.java)
                val event = allocateMethod.invoke(unsafe, SensorEvent::class.java) as SensorEvent

                val valuesField = SensorEvent::class.java.getDeclaredField("values")
                valuesField.isAccessible = true
                val arr = FloatArray(values.size)
                System.arraycopy(values, 0, arr, 0, values.size)
                valuesField.set(event, arr)

                val sf = SensorEvent::class.java.getDeclaredField("sensor")
                sf.isAccessible = true
                sf.set(event, sensor)

                val tf = SensorEvent::class.java.getDeclaredField("timestamp")
                tf.isAccessible = true
                tf.setLong(event, SystemClock.elapsedRealtimeNanos())

                val af = SensorEvent::class.java.getDeclaredField("accuracy")
                af.isAccessible = true
                af.setInt(event, 3)
                return event
            } catch (_: Throwable) {}

            return null
        }
    }
}
