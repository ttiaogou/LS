package com.yushan.locationspoofer.vendor

/**
 * 已知的 ROM 家族，由 [RomRules.familyOf] 从设备画像识别得出。
 * xposed 模块用它选择适配器，App 用它显示"本机系统"。
 */
enum class RomFamily(
    /** 界面上显示的系统名（HyperOS / MIUI 由 [RomRules.romName] 进一步区分） */
    val displayName: String
) {
    /** 原生 / 接近原生（Pixel、AOSP、LineageOS 等），也是所有机型的兜底基线 */
    AOSP("Android"),

    /** 小米 / 红米 / POCO：HyperOS 与旧版 MIUI */
    HYPEROS_MIUI("HyperOS"),

    /** OPPO / OnePlus / realme：ColorOS / OxygenOS（新版已同源） */
    COLOROS("ColorOS"),

    /** 三星：One UI */
    ONEUI("One UI"),

    /** vivo / iQOO：OriginOS / Funtouch OS */
    ORIGINOS_FUNTOUCH("OriginOS"),

    /** 华为 / 荣耀：EMUI / MagicOS */
    MAGICOS_EMUI("MagicOS"),

    /** 魅族：Flyme */
    FLYME("Flyme"),
}
