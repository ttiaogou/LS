package com.yushan.locationspoofer.utils

import org.json.JSONObject

/** Disk state is not a user request to restart a simulation in a new process. */
internal fun restoreInactiveConfig(text: String?): JSONObject? = text?.let {
    runCatching { JSONObject(it).put("active", false) }.getOrNull()
}

internal fun shouldClearStaleSimulation(inProcessActive: Boolean, serviceRunning: Boolean): Boolean =
    !inProcessActive && !serviceRunning
