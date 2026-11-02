package com.yushan.locationspoofer.xposed.utils

import org.json.JSONObject
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameworkConfigBootGateTest {
    private fun snapshot(publishedAt: Long) =
        FrameworkConfigSnapshot(JSONObject(), "libxposed:remote-preferences", publishedAt)

    @Test
    fun `snapshot from a previous boot is not from this boot`() {
        val now = 1_000_000L
        val elapsed = 10_000L  // 开机于 now-elapsed = 990_000
        assertFalse(snapshot(950_000L).publishedThisBoot(now, elapsed))
        assertFalse(snapshot(989_999L).publishedThisBoot(now, elapsed))
    }

    @Test
    fun `snapshot published after boot counts`() {
        val now = 1_000_000L
        val elapsed = 10_000L
        assertTrue(snapshot(990_001L).publishedThisBoot(now, elapsed))
        assertTrue(snapshot(999_000L).publishedThisBoot(now, elapsed))
    }
}
