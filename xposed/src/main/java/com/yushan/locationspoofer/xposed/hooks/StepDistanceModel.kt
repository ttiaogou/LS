package com.yushan.locationspoofer.xposed.hooks

/**
 * 步数与位移的唯一换算：**步数由"沿模拟路径走过的距离"推出，步频是它的导数**。
 *
 * 旧实现是"时间 × 步频"积分（[StepCadenceClock]），与位置引擎各用一套独立噪声，因此会出现
 * 到点不停、暂停续走对不上、长时间漂移；现在改成同一个速度模型：
 *
 * ```
 * traveled(t) = route_distance_offset + ∫ realism.speed(speed, τ)dτ   // 与位置引擎同一个量
 * stride      = speed / (cadenceTarget/60)                            // 1.4 m/s → 0.692 m/步
 * steps(t)    = 会话起点步数 + traveled(t) / stride
 * cadence(t)  = realism.speed(speed, t) / stride × 60                 // 交给 native 做秒内插值
 * ```
 *
 * 这样：到达终点（traveled 被封顶）步数就停；暂停续走不跳变；步频与步数同源，不再漂移；
 * 手动步频模式下 speed 与 cadence 都给定时，行为与旧的"时间 × 步频"完全一致。
 */
internal object StepDistanceModel {

    const val DEFAULT_STRIDE_M = 0.70
    const val MIN_STRIDE_M = 0.35
    const val MAX_STRIDE_M = 1.20

    /** 由速度与目标步频反推步幅（米/步）；任一参数无效时退回 [DEFAULT_STRIDE_M]。 */
    fun strideMeters(speedMps: Double, cadenceSpm: Double): Double {
        if (!speedMps.isFinite() || !cadenceSpm.isFinite() || speedMps <= 0.0 || cadenceSpm <= 0.0) {
            return DEFAULT_STRIDE_M
        }
        return (speedMps / (cadenceSpm / 60.0)).coerceIn(MIN_STRIDE_M, MAX_STRIDE_M)
    }

    /** 瞬时步频：位移对时间的导数 ÷ 步幅。速度为 0（已到达或暂停）时为 0。 */
    fun cadenceSpm(speedMps: Double, strideMeters: Double): Double {
        if (!speedMps.isFinite() || speedMps <= 0.0) return 0.0
        if (!strideMeters.isFinite() || strideMeters <= 0.0) return 0.0
        return speedMps / strideMeters * 60.0
    }

    /**
     * 用位移增量推进累计步数：只增不减。
     *
     * `traveled < lastTraveled` 表示会话重置（新的 start_timestamp / 折返回到更小的累计距离），
     * 此时只把基准拉回当前位移，不回退步数——真机计步器同样是单调的。
     * 返回 (新步数, 新位移基准)。
     */
    fun advance(
        steps: Double,
        traveledMeters: Double,
        lastTraveledMeters: Double,
        strideMeters: Double
    ): Pair<Double, Double> {
        if (!traveledMeters.isFinite() || !strideMeters.isFinite() || strideMeters <= 0.0) {
            return steps to lastTraveledMeters
        }
        val delta = traveledMeters - lastTraveledMeters
        if (delta <= 0.0) return steps to traveledMeters
        return (steps + delta / strideMeters) to traveledMeters
    }
}
