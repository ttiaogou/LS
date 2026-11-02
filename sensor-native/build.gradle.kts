plugins {
    alias(libs.plugins.android.library)
}

val backend = providers.gradleProperty("nativeSensorBackend").orElse("steps").get()
val passthrough = backend in setOf("passthrough", "steps")

// JNI 导出名必须跟随承载它的 Java 类所在的包。此前它是 C++ 里的字面量，
// 改包名时不会自动更新（一旦漏改，构建照样过、运行期 UnsatisfiedLinkError），
// 所以这里从 :xposed 模块的 Gradle namespace 直接推导，杜绝该类故障。
evaluationDependsOn(":xposed")
val jniPackage = run {
    val namespace = project(":xposed")
        .extensions.getByType<com.android.build.gradle.LibraryExtension>().namespace
        ?: error(":xposed 未声明 namespace，无法推导 JNI 包名")
    // Java 类在 <namespace>.hooks 里，JNI 名用的是"类所在包"的下划线形式：
    // com.yushan.locationspoofer.xposed.hooks -> com_yushan_locationspoofer_xposed_hooks
    // 这里一次性给出完整包名，C++ 只负责接类名与方法名。上一版只给到 namespace、再由 C++
    // 拼 _xposed_hooks_，结果多出一段 xposed（..._xposed_xposed_hooks_...），运行期
    // UnsatisfiedLinkError；构建期不报错，所以由 :app:verifyJniExports 自动核对。
    namespace.replace('.', '_') + "_hooks"
}

android {
    namespace = "com.yushan.locationspoofer.nativeprobe"
    compileSdk = 37
    ndkVersion = "28.2.13676358"
    defaultConfig {
        minSdk = 26
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
                arguments += "-DSENSOR_PROBE_PASSTHROUGH=${if (passthrough) "ON" else "OFF"}"
                // 布局核对：passthrough（只挂钩不改写）构建里转储前若干条真实 step 事件
                arguments += "-DSENSOR_PROBE_EVENT_DUMP=${if (backend == "passthrough") "ON" else "OFF"}"
                arguments += "-DSENSOR_PROBE_STEPS=${if (backend == "steps") "ON" else "OFF"}"
                arguments += "-DSENSOR_PROBE_JNI_PACKAGE=$jniPackage"
                targets += "location_accel"
                if (passthrough) targets += listOf("shadowhook", "shadowhook_nothing")
            }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("../xposed/src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
