# Android binding for argosy-sigil

Reference Android library wrapping sigil's C ABI for JVM consumers
(Kotlin / Java). Used by argosy-launcher; suitable for any Android
emulator launcher that wants the same RomM-canonical title-ID extraction.

## Integrating into a Gradle build

There are two integration paths. Pick whichever fits your repo layout.

### Option 1: vendor sigil as a git submodule

Add sigil under your project root:

```sh
git submodule add https://github.com/rommapp/argosy-sigil.git sigil
```

Reference the binding module in `settings.gradle.kts`:

```kotlin
include(":sigil")
project(":sigil").projectDir = file("sigil/bindings/android")
```

Then depend on `:sigil` from your app module:

```kotlin
// app/build.gradle.kts
dependencies {
    implementation(project(":sigil"))
}
```

### Option 2: Gradle composite build

If you want to develop sigil and your launcher in tandem, use Gradle's
`includeBuild`. Add to `settings.gradle.kts`:

```kotlin
includeBuild("../argosy-sigil/bindings/android") {
    dependencySubstitution {
        substitute(module("com.nendo:sigil")).using(project(":"))
    }
}
```

## Usage

[docs/kotlin.md](../../docs/kotlin.md): identify, locate, hash, with
what each call requires and returns.

## ABI

Default build targets `arm64-v8a`. Pass `-PallAbis` to also build
`armeabi-v7a` (for CI or distribution).

## How it works

The CMake build at `src/main/cpp/CMakeLists.txt` adds the sigil C
library as a subdirectory build (`add_subdirectory`) and links it into
`libsigil-jni.so`. The Kotlin facade in
`src/main/kotlin/com/nendo/sigil/Sigil.kt` calls into the JNI shim,
which in turn calls sigil's public C API.

This means each consuming app builds sigil from source — there's no
prebuilt AAR to vendor or version-pin. That's deliberate: sigil is a
small enough C library that building from source is cheap, and it
keeps consumers on the same revision their submodule pin specifies.
