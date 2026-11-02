package com.yushan.locationspoofer.utils

import org.junit.Assert.*
import org.junit.Test

class SimulationRecoveryTest {
    @Test fun diskSessionIsInactiveEvenWhenItStartedDuringThisBoot() {
        val restored = restoreInactiveConfig("""{"active":true,"start_timestamp":12345,"lat":23.0}""")!!
        assertFalse(restored.getBoolean("active"))
        assertEquals(12345L, restored.getLong("start_timestamp"))
        assertEquals(23.0, restored.getDouble("lat"), 0.0)
        // Settings updates copy the sanitized snapshot, not the old active payload.
        val patched = org.json.JSONObject(restored.toString()).put("enable_jitter", true)
        assertFalse(patched.getBoolean("active"))
    }

    @Test fun malformedOrMissingDiskStateIsNotRestored() {
        assertNull(restoreInactiveConfig(null))
        assertNull(restoreInactiveConfig("not json"))
    }

    @Test fun viewModelRecreationKeepsALiveSession() {
        assertFalse(shouldClearStaleSimulation(true, true))
        assertFalse(shouldClearStaleSimulation(true, false))
        assertFalse(shouldClearStaleSimulation(false, true))
    }

    @Test fun processRestartClearsOnlyTheStaleFlag() {
        assertTrue(shouldClearStaleSimulation(false, false))
    }
}
