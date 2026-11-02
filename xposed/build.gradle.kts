import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.jetbrains.kotlin.android)
}

val nativeSensorBackend = providers.gradleProperty("nativeSensorBackend").orElse("steps").get()

android {
    namespace = "com.yushan.locationspoofer.xposed"
    compileSdk = 37

    defaultConfig {
        minSdk = 26
        buildConfigField("String", "NATIVE_SENSOR_BACKEND", "\"none\"")
    }

    // 与 app/build.gradle.kts 中的 scheme 维度保持一致。
    // META-INF/xposed/scope.list 按 flavor 放在 src/scoped/resources 与 src/global/resources 下。
    flavorDimensions += "scheme"
    productFlavors {
        create("scoped") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "false")
        }
        create("global") {
            dimension = "scheme"
            buildConfigField("boolean", "GLOBAL_SCHEME", "true")
            buildConfigField("String", "NATIVE_SENSOR_BACKEND", "\"$nativeSensorBackend\"")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        buildConfig = true
    }
}

run {
    // AGP's legacy DSL needs its own source-set interface when newDsl=false.
    val sets: org.gradle.api.NamedDomainObjectContainer<*> =
        extensions.getByType(com.android.build.gradle.LibraryExtension::class.java).sourceSets
    (sets.getByName("global") as com.android.build.gradle.api.AndroidSourceSet)
        .jniLibs.setSrcDirs(emptyList<String>())
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":core-geo"))
    compileOnly(libs.xposed.api)

    testImplementation(libs.junit)
    testImplementation(libs.org.json)
    add("globalImplementation", project(":sensor-native"))
}
