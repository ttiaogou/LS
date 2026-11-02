package com.yushan.locationspoofer.xposed.hooks

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 步数=位移/步幅 的换算：步频是它的导数，位移封顶即停，会话重置不回退。
 */
class StepDistanceModelTest {

    @Test fun autoCadenceAndSpeedAgreeOnWalkingStride() {
        // 1.4 m/s 的自动步频是 121 spm，对应约 0.692 m 的步幅
        val stride = StepDistanceModel.strideMeters(1.4, 121.0)
        assertEquals(0.6942, stride, 0.001)
        // 反推回步频必须一致（否则 native 秒内外推会与步数漂移）
        assertEquals(121.0, StepDistanceModel.cadenceSpm(1.4, stride), 0.2)
    }

    @Test fun manualCadenceDegeneratesToTimeTimesCadence() {
        // 手动 165 spm + 1.4 m/s：走 10 秒应当正好是 27.5 步
        val stride = StepDistanceModel.strideMeters(1.4, 165.0)
        val traveledPerSecond = 1.4
        val steps = (0 until 10).fold(0.0 to 0.0) { (steps, last), second ->
            StepDistanceModel.advance(steps, traveledPerSecond * (second + 1), last, stride)
        }.first
        assertEquals(27.5, steps, 0.0001)
    }

    @Test fun traveledDistanceDrivesStepsMonotonically() {
        val stride = StepDistanceModel.strideMeters(1.4, 121.0)
        var steps = 2350.0
        var last = 0.0
        // 每 200 ms 前进 0.28 m（1.4 m/s）
        repeat(50) { index ->
            val (next, baseline) = StepDistanceModel.advance(steps, 0.28 * (index + 1), last, stride)
            assertTrue("步数只增不减", next >= steps)
            steps = next
            last = baseline
        }
        assertEquals(2350.0 + 14.0 / stride, steps, 0.0001)
    }

    @Test fun cappedTravelStopsStepsAtDestination() {
        // 到达终点后 traveled 被 RouteEngine 封顶：位移不再增长 → 步数不再增长
        val stride = StepDistanceModel.strideMeters(1.2, 110.0)
        val atArrival = StepDistanceModel.advance(2400.0, 100.0, 99.9, stride)
        val afterArrival = StepDistanceModel.advance(atArrival.first, 100.0, atArrival.second, stride)
        assertEquals(atArrival.first, afterArrival.first, 0.0)
        assertEquals(100.0, afterArrival.second, 0.0)
    }

    @Test fun sessionResetRebasesWithoutRewinding() {
        val stride = StepDistanceModel.strideMeters(1.4, 121.0)
        // 新会话把累计位移拉回 0：步数保持，只把基准拉回
        val (steps, baseline) = StepDistanceModel.advance(3000.0, 0.0, 480.0, stride)
        assertEquals(3000.0, steps, 0.0)
        assertEquals(0.0, baseline, 0.0)
        // 之后继续按新位移增长
        val resumed = StepDistanceModel.advance(steps, 6.92, baseline, stride)
        assertEquals(3000.0 + 6.92 / stride, resumed.first, 0.0001)
    }

    @Test fun firstObservationOfARunningSessionAddsNoSteps() {
        // 模块启动时撞上一个已经跑了 5.45 小时（约 27.5 km）的会话：
        // 基准对齐到当前位置 → 一步都不补，之后只按新位移增长
        val stride = StepDistanceModel.strideMeters(1.4, 121.0)
        val (steps, baseline) = StepDistanceModel.advance(2350.0, 27_470.0, 27_470.0, stride)
        assertEquals(2350.0, steps, 0.0)
        assertEquals(27_470.0, baseline, 0.0)
        val after = StepDistanceModel.advance(steps, 27_470.0 + 6.92, baseline, stride)
        assertEquals(2350.0 + 6.92 / stride, after.first, 0.0001)
    }

    @Test fun invalidInputsAreIgnored() {
        val stride = StepDistanceModel.strideMeters(0.0, 0.0)
        assertEquals(StepDistanceModel.DEFAULT_STRIDE_M, stride, 0.0)
        assertEquals(0.0, StepDistanceModel.cadenceSpm(0.0, stride), 0.0)
        assertEquals(0.0, StepDistanceModel.cadenceSpm(Double.NaN, stride), 0.0)
        val unchanged = StepDistanceModel.advance(100.0, Double.NaN, 10.0, stride)
        assertEquals(100.0, unchanged.first, 0.0)
        assertEquals(10.0, unchanged.second, 0.0)
    }
}
