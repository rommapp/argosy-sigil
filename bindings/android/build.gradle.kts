// SPDX-License-Identifier: MPL-2.0
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.nendo.sigil"
    compileSdk = 35

    defaultConfig {
        minSdk = 26

        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                arguments("-DANDROID_STL=none")
            }
        }

        ndk {
            // Match argosy-launcher's ABI conventions: arm64-v8a default; the rest in CI /
            // when the consumer asks for it. x86 covers Android-x86 and emulator hosts.
            if (project.hasProperty("allAbis") || project.hasProperty("ciOnly")) {
                abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
            } else {
                abiFilters += "arm64-v8a"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            version = "3.22.1"
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}
