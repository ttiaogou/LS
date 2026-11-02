package com.yushan.locationspoofer.xposed.utils

import com.yushan.locationspoofer.utils.FrameworkConfigChannel
import org.json.JSONObject

internal data class FrameworkConfigSnapshot(val config: JSONObject, val source: String, val publishedAt: Long) {
    /**
     * 配置是否在本次开机之后发布。上一次运行留下的快照会在重启后被框架重新读出，里面仍是
     * active=true；如果照单全收，模块就会在用户没有打开 App、没有点"开始"的情况下开始改写
     * 定位并注入计步。是否模拟必须由 App 决定，所以跨开机的旧快照一律按未激活处理。
     */
    fun publishedThisBoot(nowWallClockMillis: Long, elapsedRealtimeMillis: Long): Boolean =
        publishedAt > nowWallClockMillis - elapsedRealtimeMillis

    companion object {
        fun decode(text: String, readFile: (String) -> String): FrameworkConfigSnapshot {
            val envelope = JSONObject(text)
            require(envelope.getInt("version") == FrameworkConfigChannel.VERSION) { "Unsupported config version" }
            require(envelope.getString("id").isNotBlank()) { "Missing snapshot identity" }
            val file = envelope.optString("file")
            val payload = if (file.isEmpty()) envelope.getString("payload") else {
                require(FrameworkConfigChannel.isConfigFile(file)) { "Invalid remote config filename" }
                readFile(file)
            }
            return FrameworkConfigSnapshot(
                JSONObject(payload),
                if (file.isEmpty()) "libxposed:remote-preferences" else "libxposed:remote-file",
                envelope.getLong("published_at")
            )
        }
    }
}
