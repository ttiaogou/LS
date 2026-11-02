package com.yushan.locationspoofer.xposed.utils

import com.yushan.locationspoofer.utils.AltitudeModel
import com.yushan.locationspoofer.utils.CoordinateUtils.LatLng
import com.yushan.locationspoofer.utils.GeoMath
import com.yushan.locationspoofer.utils.MotionRealism
import com.yushan.locationspoofer.utils.RoutePath
import org.json.JSONArray
import org.json.JSONObject

data class SpoofedMotion(
    val lat: Double,
    val lng: Double,
    val bearing: Float,
    val speed: Float
)

/**
 * 根据配置和当前时间推导模拟位置：路线模式沿路线插值，摇杆模式按方向和速度推算，其余返回配置里的固定坐标。
 * 路线几何与 App 端共用 [RoutePath]，保证两端对同一时刻算出同一位置。
 */
object RouteEngine {

    /**
     * 摇杆模式：App 约每秒写一次"当前位置 + 方向 + 速度"，两次写入之间在这里按方向和速度往前推算，
     * 让被 Hook 的 App 看到连续移动而不是每秒跳一下。推算时长有上限，防止 App 被杀后位置一直漂走。
     */
    private const val JOYSTICK_MAX_EXTRAPOLATION_MS = 3000L

    // 配置 JSON 只在文件变化时才重新解析，同一个 JSONArray 实例对应同一条路线，按实例缓存即可
    @Volatile private var cachedRouteArray: JSONArray? = null
    @Volatile private var cachedPath: RoutePath? = null

    private fun routePath(routeArray: JSONArray): RoutePath {
        val cached = cachedPath
        if (routeArray === cachedRouteArray && cached != null) return cached
        val points = ArrayList<LatLng>(routeArray.length())
        for (i in 0 until routeArray.length()) {
            val obj = routeArray.optJSONObject(i) ?: continue
            points += LatLng(obj.optDouble("lat", 0.0), obj.optDouble("lng", 0.0))
        }
        return RoutePath(points).also {
            cachedPath = it
            cachedRouteArray = routeArray
        }
    }

    private fun extrapolateJoystick(config: JSONObject, now: Long, base: LatLng, bearing: Float): SpoofedMotion {
        val speed = config.optDouble("speed_m_s", 0.0)
        val startTime = config.optLong("start_timestamp", 0L)
        if (speed <= 0.0 || startTime <= 0L) return SpoofedMotion(base.lat, base.lng, bearing, 0f)

        val elapsedSec = (now - startTime).coerceIn(0L, JOYSTICK_MAX_EXTRAPOLATION_MS) / 1000.0
        val pos = GeoMath.destination(base, bearing.toDouble(), speed * elapsedSec)
        return SpoofedMotion(pos.lat, pos.lng, bearing, speed.toFloat())
    }

    fun calculateCurrentPosition(config: JSONObject, now: Long = System.currentTimeMillis()): SpoofedMotion {
        val base = LatLng(config.optDouble("lat", 0.0), config.optDouble("lng", 0.0))
        val baseBearing = config.optDouble("sim_bearing", 0.0).toFloat()
        val routeArray = config.optJSONArray("route_points")

        if (!config.optBoolean("is_route_mode", false) || routeArray == null || routeArray.length() < 2) {
            if (config.optString("sim_mode") == "JOYSTICK") {
                return extrapolateJoystick(config, now, base, baseBearing)
            }
            return SpoofedMotion(base.lat, base.lng, baseBearing, 0f)
        }

        val path = routePath(routeArray)
        if (!path.isValid) return SpoofedMotion(base.lat, base.lng, baseBearing, 0f)

        val speed = config.optDouble("speed_m_s", 3.0).coerceAtLeast(0.1)
        val rawStartTime = config.optLong("start_timestamp", 0L)
        val startTime = if (rawStartTime > 0L) rawStartTime else now
        val elapsedSec = (now - startTime).coerceAtLeast(0L) / 1000.0
        val realism = realismSession(config, startTime)
        // 暂停后继续时，路线从暂停处的累计距离接着走（start_timestamp 重置为继续的时刻）
        val offset = config.optDouble("route_distance_offset", 0.0).coerceAtLeast(0.0)
        val traveled = offset + realism.distance(speed, elapsedSec)

        val pos = path.positionAt(traveled, config.optBoolean("stop_at_destination", false))
        val currentSpeed = if (pos.arrived) 0f else realism.speed(speed, elapsedSec).toFloat()
        return SpoofedMotion(pos.lat, pos.lng, pos.bearing, currentSpeed)
    }

    /**
     * 已沿模拟路径走过的距离（米）。与 [calculateCurrentPosition] 用同一个速度模型（同一噪声），
     * 所以步数可以由它直接推出、与位置严格一致：
     *  - 勾选"到达后停止"时封顶在路线总长（到点即停，步数不再增长）；
     *  - 折返/闭环模式返回累计走过的路程（来回往复都在累积，符合真实计步）；
     *  - 摇杆模式用同一个外推窗口；定点模式速度为 0 → 0。
     */
    fun traveledDistance(config: JSONObject, now: Long = System.currentTimeMillis()): Double {
        val routeArray = config.optJSONArray("route_points")
        val routeMode = config.optBoolean("is_route_mode", false) && routeArray != null && routeArray.length() >= 2
        if (!routeMode) {
            if (config.optString("sim_mode") != "JOYSTICK") return 0.0
            val speed = config.optDouble("speed_m_s", 0.0)
            val startTime = config.optLong("start_timestamp", 0L)
            if (speed <= 0.0 || startTime <= 0L) return 0.0
            val elapsedSec = (now - startTime).coerceIn(0L, JOYSTICK_MAX_EXTRAPOLATION_MS) / 1000.0
            return speed * elapsedSec
        }
        val path = routePath(routeArray!!)
        if (!path.isValid) return 0.0
        val speed = config.optDouble("speed_m_s", 3.0).coerceAtLeast(0.1)
        val rawStartTime = config.optLong("start_timestamp", 0L)
        val startTime = if (rawStartTime > 0L) rawStartTime else now
        val elapsedSec = (now - startTime).coerceAtLeast(0L) / 1000.0
        val offset = config.optDouble("route_distance_offset", 0.0).coerceAtLeast(0.0)
        val traveled = offset + realismSession(config, startTime).distance(speed, elapsedSec)
        return if (config.optBoolean("stop_at_destination", false)) {
            traveled.coerceAtMost(path.totalDistance)
        } else {
            traveled
        }
    }

    /**
     * 当前位置的海拔：基准海拔 ± 变化范围内随地形起伏，叠加随机强度对应的缓慢漂移（见 [AltitudeModel]），
     * 各进程对同一时刻给出一致的值。旧版配置没有变化范围字段时，按随机强度的漂移幅度处理。
     */
    fun realisticAltitude(config: JSONObject, now: Long = System.currentTimeMillis()): Double {
        val base = config.optDouble("altitude", 25.0)
        val drift = MotionRealism.Level.fromId(config.optInt("realism_level", MotionRealism.Level.OFF.id)).altitudeAmplitudeM
        val variation = config.optDouble("altitude_variation_m", drift)
        if (variation <= 0.0) return base
        val pos = calculateCurrentPosition(config, now)
        return AltitudeModel.altitude(base, variation, drift, pos.lat, pos.lng, now)
    }

    /** 配置里缺少真实度字段（旧版 App 写的配置）时按"关闭 + 不浮动"处理，行为与旧版一致 */
    fun realismSession(config: JSONObject, startTimestamp: Long = config.optLong("start_timestamp", 0L)): MotionRealism.Session =
        MotionRealism.session(
            startTimestamp,
            config.optInt("realism_level", MotionRealism.Level.OFF.id),
            config.optInt("speed_fluctuation_pct", 0)
        )
}
