// SPDX-License-Identifier: MPL-2.0
// Runs the Kotlin binding's JVM tests against the host JNI build (docs/building.md, "Bindings").
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "sigil-jvm-tests"
include(":sigil")
project(":sigil").projectDir = file("../../bindings/android")
