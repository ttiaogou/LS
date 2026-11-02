plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "dev.stepfaker.globalprobe"
    compileSdk = 37
    defaultConfig {
        applicationId = "dev.stepfaker.globalprobe"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
