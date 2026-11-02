pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        maven { url = uri("https://api.xposed.info/") }
    }
}

rootProject.name = "LocationSpoofer"
include(":app")
include(":app-ui")
include(":service")
include(":xposed")
include(":core-data")
include(":core-geo")

val nativeSensorBackend = providers.gradleProperty("nativeSensorBackend").orElse("steps").get()
require(nativeSensorBackend in setOf("probe", "passthrough", "steps")) {
    "Unsupported nativeSensorBackend=$nativeSensorBackend; use steps, probe or passthrough (legacy is removed)"
}
include(":sensor-native")
if (providers.gradleProperty("includeGlobalProbe").orNull == "true") include(":global-probe")
