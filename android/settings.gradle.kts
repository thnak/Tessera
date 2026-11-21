// SPDX-License-Identifier: MIT

// Gradle settings for the Android app shell (issue #189). One module: the app.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "tessera-android"
include(":app")
