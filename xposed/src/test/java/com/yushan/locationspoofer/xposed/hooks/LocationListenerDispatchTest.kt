package com.yushan.locationspoofer.xposed.hooks

import org.junit.Assert.*
import org.junit.Test

class LocationListenerDispatchTest {
    class Single {
        var received: String? = null
        fun onLocationChanged(location: String) { received = location }
    }
    interface Callback
    class Batch {
        var received: List<String>? = null
        fun onLocationChanged(locations: List<String>, callback: Callback?) { received = locations }
    }
    class Broken {
        fun onLocationChanged(locations: List<String>, callback: Any?) {
            throw IllegalStateException("delivery failed")
        }
    }
    class ListOnly {
        var received: List<String>? = null
        fun onLocationChanged(locations: List<String>) { received = locations }
    }
    class BrokenSingle {
        fun onLocationChanged(location: String) { throw IllegalStateException("single failed") }
    }
    @Test fun singleDispatches() {
        val listener = Single()
        invokeLocationListener(listener, "fix") { error("unused") }
        assertEquals("fix", listener.received)
    }
    @Test fun incompatibleCallbackFallsBackToNull() {
        val listener = Batch()
        invokeLocationListener(listener, "fix") { Any() }
        assertEquals(listOf("fix"), listener.received)
    }
    @Test fun listOnlyDispatches() {
        val listener = ListOnly()
        invokeLocationListener(listener, "fix") { error("unused") }
        assertEquals(listOf("fix"), listener.received)
    }
    @Test fun singleFailureIsUnwrapped() {
        try {
            invokeLocationListener(BrokenSingle(), "fix") { Any() }
            fail("Expected delivery failure")
        } catch (error: IllegalStateException) {
            assertEquals("single failed", error.message)
        }
    }
    @Test fun failedFallbackIsNotSwallowed() {
        try {
            invokeLocationListener(Broken(), "fix") { Any() }
            fail("Expected delivery failure")
        } catch (error: IllegalStateException) {
            assertEquals("delivery failed", error.message)
            assertEquals(1, error.suppressed.size)
        }
    }
    @Test fun unsupportedListenerFails() {
        try {
            invokeLocationListener(Any(), "fix") { Any() }
            fail("Expected unsupported signature")
        } catch (_: NoSuchMethodException) { }
    }
}
