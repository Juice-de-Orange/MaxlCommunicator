import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    kotlin("android")
}

android {
    namespace = "io.github.juice_de_orange.maxlcommunicator"
    compileSdk = 34

    defaultConfig {
        applicationId = "io.github.juice_de_orange.maxlcommunicator"

        /*
         * 26 is where a foreground service needs a notification channel, and
         * this app is a foreground service with a UI attached rather than the
         * other way round. Below that there is nothing to gain: the device this
         * talks to did not exist in 2017 either.
         */
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

kotlin {
    compilerOptions {
        /*
         * The same target the Java side above compiles to. Kotlin and Java must
         * agree or the Android plugin refuses the build -- and it refuses with a
         * message about toolchains, which points at the wrong fix on a machine
         * that has one JDK and needs no toolchain at all.
         */
        jvmTarget.set(JvmTarget.JVM_17)

        // The same discipline as :protocol and as the firmware.
        allWarningsAsErrors.set(true)
    }
}

dependencies {
    implementation(project(":protocol"))
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    /*
     * test-junit rather than plain kotlin("test"): kotlin-test needs a framework
     * binding, and an Android module's unit tests run on JUnit 4. Without the
     * suffix every kotlin.test import is simply unresolved, which reads like a
     * missing dependency rather than a missing binding.
     *
     * These are JVM unit tests, not instrumented ones. EventStore touches
     * java.io.File and nothing Android, which is why it can be tested at all
     * without a device -- and why it is worth the source set.
     */
    testImplementation(kotlin("test-junit"))
}
