import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.jetbrains.kotlin.android)
    alias(libs.plugins.jetbrains.kotlin.compose)
    alias(libs.plugins.jetbrains.kotlin.serialization)
}

/** 根目录 local.yml（已被 gitignore）里的本地私有配置，按 `KEY: value` 读取 */
fun localConfig(key: String): String? {
    val file = rootProject.file("local.yml")
    if (!file.exists()) return null
    return file.readLines().firstOrNull { it.startsWith("$key:") }
        ?.substringAfter(":")?.trim()?.removeSurrounding("\"")?.removeSurrounding("'")
}

/** 项目主页指向的仓库（fork 可改）：环境变量或 local.yml 的 REPO_SLUG */
val repoSlug = System.getenv("REPO_SLUG") ?: localConfig("REPO_SLUG") ?: "yushan1337/LocationSpoofer"

/** 百度地图暗色自定义样式 ID 绑定申请者账号：仓库不内置，留空则用百度默认样式 */
val baiduMapStyleId =
    System.getenv("BAIDU_MAP_STYLE_ID") ?: localConfig("BAIDU_MAP_STYLE_ID") ?: ""

android {
    namespace = "com.yushan.locationspoofer.ui"
    compileSdk = 37

    defaultConfig {
        minSdk = 26

        buildConfigField("String", "BAIDU_MAP_STYLE_ID", "\"$baiduMapStyleId\"")
        buildConfigField("String", "REPO_SLUG", "\"$repoSlug\"")

        vectorDrawables {
            useSupportLibrary = true
        }
    }

    // 与 app/build.gradle.kts 中的 scheme 维度保持一致
    flavorDimensions += "scheme"
    productFlavors {
        create("scoped") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "false")
        }
        create("global") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "true")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
        freeCompilerArgs.addAll(
            "-Xskip-metadata-version-check",
            "-opt-in=kotlinx.serialization.InternalSerializationApi"
        )
    }
}

dependencies {
    implementation(project(":core-data"))
    implementation(project(":core-geo"))

    implementation(libs.koin.androidx.compose)
    implementation(libs.amap.map)
    implementation(libs.amap.search)
    implementation(libs.baidu.map)
    implementation(libs.baidu.location)
    implementation(libs.baidu.search)
    implementation(libs.google.maps)
    implementation(libs.google.places)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.play.services.location)
    implementation(libs.okhttp)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)

    implementation(libs.miuix.ui)
    implementation(libs.miuix.blur)

    debugImplementation(libs.androidx.ui.tooling)

    testImplementation(libs.junit)
}
