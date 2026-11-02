@file:Suppress("DEPRECATION")

import java.util.Properties
import java.util.zip.ZipFile
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.jetbrains.kotlin.android)
    alias(libs.plugins.jetbrains.kotlin.compose)
}

val nativeSensorBackend = providers.gradleProperty("nativeSensorBackend").orElse("steps").get()

android {
    namespace = "com.yushan.locationspoofer"
    compileSdk = 37

    fun getLocalConfig(key: String): String? {
        val localYml = file("../local.yml")
        if (localYml.exists()) {
            val line = localYml.readLines().find { it.startsWith("$key:") }
            if (line != null) {
                return line.substringAfter(":").trim().removeSurrounding("\"")
                    .removeSurrounding("'")
            }
        }
        return null
    }

    val googleMapsApiKey =
        System.getenv("GOOGLE_MAPS_API_KEY") ?: getLocalConfig("GOOGLE_MAPS_API_KEY") ?: ""

    // 地图 key 一律不内置（原作者预留的高德/百度 key 已移除）：默认留空，
    // 由环境变量或根目录 local.yml（已被 gitignore）注入，应用内「设置 → 地图引擎」
    // 填写的 key 会在运行期覆盖；留空时对应引擎鉴权失败、其他引擎不受影响。
    val amapApiKey = System.getenv("AMAP_API_KEY") ?: getLocalConfig("AMAP_API_KEY") ?: ""
    val baiduApiKey = System.getenv("BAIDU_API_KEY") ?: getLocalConfig("BAIDU_API_KEY") ?: ""

    defaultConfig {
        applicationId = "com.yushan.locationspoofer"
        testInstrumentationRunner = "com.yushan.locationspoofer.SystemFilesInstrumentation"
        minSdk = 26
        targetSdk = 37
        versionCode = providers.gradleProperty("APP_VERSION_CODE").get().toInt()
        versionName = providers.gradleProperty("APP_VERSION_NAME").get()

        vectorDrawables {
            useSupportLibrary = true
        }

        manifestPlaceholders["googleMapsApiKey"] = googleMapsApiKey
        manifestPlaceholders["amapApiKey"] = amapApiKey
        manifestPlaceholders["baiduApiKey"] = baiduApiKey

        splits {
            abi {
                isEnable = true
                reset()
                include("arm64-v8a")
                isUniversalApk = false
            }
        }
    }

    // 模拟方案维度：scoped = 在 LSPosed 作用域里的目标 App 进程内 Hook；global = Hook system_server 等系统进程，对全设备生效。
    // app / app-ui / core-data / xposed 四个模块必须声明完全相同的维度与 flavor，否则变体无法对齐。
    flavorDimensions += "scheme"
    productFlavors {
        create("scoped") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "false")
        }
        create("global") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "true")
            versionNameSuffix = "-native-$nativeSensorBackend"
        }
    }

    // CI 用环境变量（见 .github/workflows/release.yml），本地用仓库根目录的 keystore.properties（已被 gitignore）
    val keystoreProps = Properties().apply {
        val f = rootProject.file("keystore.properties")
        if (f.exists()) f.inputStream().use { load(it) }
    }
    fun signingValue(env: String, prop: String): String? =
        System.getenv(env)?.takeIf { it.isNotBlank() } ?: keystoreProps.getProperty(prop)?.takeIf { it.isNotBlank() }

    val keystorePath = signingValue("KEYSTORE_FILE_PATH", "storeFile")
    val hasReleaseKeystore = keystorePath != null && file(keystorePath).exists()

    signingConfigs {
        create("release") {
            if (hasReleaseKeystore) {
                storeFile = file(keystorePath!!)
                storePassword = signingValue("KEYSTORE_PASSWORD", "storePassword")
                keyAlias = signingValue("KEY_ALIAS", "keyAlias")
                keyPassword = signingValue("KEY_PASSWORD", "keyPassword")
            }
            // v3 为以后的密钥轮换（lineage）预留；API 26–27 设备仍按 v2 校验，所以 v2 保持开启
            enableV2Signing = true
            enableV3Signing = true
        }
    }

    buildTypes {
        debug {
            // 没配置发布密钥时回退到 debug 签名，保证新克隆仓库的人也能直接编译调试
            signingConfig = signingConfigs.getByName(if (hasReleaseKeystore) "release" else "debug")
            buildConfigField("String", "GOOGLE_MAPS_API_KEY", "\"$googleMapsApiKey\"")
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
            buildConfigField("String", "GOOGLE_MAPS_API_KEY", "\"$googleMapsApiKey\"")
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
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
        freeCompilerArgs.addAll(
            "-Xskip-metadata-version-check"
        )
    }
}

dependencies {
    implementation(project(":core-geo"))
    implementation(project(":core-data"))
    implementation(project(":service"))
    implementation(project(":app-ui"))
    // 纯打包依赖：:app 不直接调用 :xposed 的代码，只是需要把它的产物
    // （LocationHooker 及 META-INF/xposed/* 资源）一起打进最终 APK，供 LSPosed 扫描加载。
    implementation(project(":xposed"))

    implementation(libs.xposed.service)
    implementation(libs.koin.androidx.compose)
    implementation(libs.amap.map)
    implementation(libs.amap.search)
    implementation(libs.baidu.map)
    implementation(libs.google.places)
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
    androidTestImplementation(libs.room.runtime)
    androidTestImplementation(libs.room.ktx)
}

/**
 * JNI 导出名核对：SystemAccelNative.java 的 native 声明（类所在包 + 方法名）必须与 global 包内
 * liblocation_accel.so 的动态符号完全一致。
 *
 * 这类错配构建期不报错，只在运行期表现为 UnsatisfiedLinkError：2026-10-16 就因为包名多拼了一段
 * xposed（..._xposed_xposed_hooks_...）而中招，且当时只核对了方法名、没核对前缀。所以固化成检查：
 * 默认取最新的 global arm64 APK，取不到再退回 sensor-native 中间产物，也可用
 * -PjniCheckApk=<path> 指定要核对的安装包。
 */
val androidSdkDir: File = androidComponents.sdkComponents.sdkDirectory.get().asFile

val verifyJniExports = tasks.register("verifyJniExports") {
    group = "verification"
    description = "核对 JNI 导出名与 Java 侧 native 声明是否一致"
    outputs.upToDateWhen { false }
    doLast {
        fun newest(root: File, predicate: (File) -> Boolean): File? =
            if (!root.isDirectory) null
            else root.walkTopDown().filter { it.isFile && predicate(it) }.maxByOrNull { it.lastModified() }

        fun nmSymbols(library: File): String {
            val candidates = mutableListOf("nm", "llvm-nm")
            listOf("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT").forEach { key ->
                System.getenv(key)?.let { candidates += "$it/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" }
            }
            File(androidSdkDir, "ndk").listFiles()?.sortedBy { it.name }?.forEach {
                candidates += "${it.absolutePath}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
            }
            var lastError = "无候选"
            for (candidate in candidates) {
                try {
                    val process = ProcessBuilder(candidate, "-D", "--defined-only", library.absolutePath)
                        .redirectErrorStream(true).start()
                    val output = process.inputStream.bufferedReader().readText()
                    if (process.waitFor() == 0) return output
                    lastError = "$candidate 退出码 ${process.exitValue()}: ${output.take(160)}"
                } catch (e: Exception) {
                    lastError = "$candidate 不可用: ${e.message}"
                }
            }
            throw GradleException("找不到可用的 nm/llvm-nm：$lastError")
        }

        val javaFile = rootProject.file("xposed/src/main/java").walkTopDown()
            .firstOrNull { it.isFile && it.name == "SystemAccelNative.java" }
            ?: throw GradleException("找不到 SystemAccelNative.java")
        val source = javaFile.readText()
        val packageName = Regex("""(?m)^\s*package\s+([\w.]+)\s*;""").find(source)?.groupValues?.get(1)
            ?: throw GradleException("${javaFile.name} 没有 package 声明")
        val methods = Regex("""\bnative\s+[\w\[\].<>]+\s+(\w+)\s*\(""")
            .findAll(source).map { it.groupValues[1] }.toSortedSet()
        if (methods.isEmpty()) throw GradleException("${javaFile.name} 里没有 native 声明")
        val expected = methods.map { "Java_" + packageName.replace('.', '_') + "_SystemAccelNative_" + it }.toSortedSet()

        val explicit = providers.gradleProperty("jniCheckApk").orNull?.let { rootProject.file(it) }
        val intermediate = newest(rootProject.file("sensor-native/build/intermediates")) { it.name == "liblocation_accel.so" }
        val apkCandidate = explicit ?: newest(rootProject.file("app/build/outputs/apk")) {
            it.extension == "apk" && it.name.contains("arm64-v8a") && it.name.contains("global")
        }
        // 同一次 Gradle 调用里核对可能先于打包执行，直接读 APK 会拿到过期库；因此只在
        // APK 不旧于中间产物时才用它，否则退回刚构建出的中间产物。
        val apk = apkCandidate?.takeIf { candidate ->
            candidate.isFile && (intermediate == null || candidate.lastModified() >= intermediate.lastModified())
        }
        val bytes = if (apk != null) {
            ZipFile(apk).use { zip ->
                val entry = zip.entries().asSequence().firstOrNull { it.name.endsWith("liblocation_accel.so") }
                    ?: throw GradleException("${apk.name} 内没有 liblocation_accel.so（scoped 变体不含原生层）")
                zip.getInputStream(entry).readBytes()
            }
        } else {
            newest(rootProject.file("sensor-native/build/intermediates")) { it.name == "liblocation_accel.so" }?.readBytes()
                ?: throw GradleException("找不到 liblocation_accel.so：先构建原生库或 global APK")
        }

        val scratch = File.createTempFile("liblocation_accel", ".so")
        try {
            scratch.writeBytes(bytes)
            val actual = Regex("""Java_[A-Za-z0-9_]+SystemAccelNative_\w+""")
                .findAll(nmSymbols(scratch)).map { it.value }.toSortedSet()
            if (actual != expected) {
                val origin = apk?.name ?: "sensor-native 中间产物"
                val missing = (expected - actual).joinToString()
                val extra = (actual - expected).joinToString()
                throw GradleException(
                    "JNI 导出名与 Java 声明不一致（核对自 $origin）\n" +
                        "  Java 声明 ${packageName}: ${expected.size} 个\n" +
                        "  产物导出: ${actual.size} 个\n" +
                        "  缺失: ${missing.ifEmpty { "无" }}\n" +
                        "  多余: ${extra.ifEmpty { "无" }}"
                )
            }
            logger.lifecycle("JNI 导出名一致：${expected.size} 个方法，核对自 ${apk?.name ?: "sensor-native 中间产物"}")
        } finally {
            scratch.delete()
        }
    }
}

tasks.named("check") { dependsOn(verifyJniExports) }
