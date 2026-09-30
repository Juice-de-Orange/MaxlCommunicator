/*
 * The native Android bridge, CLAUDE.md phase 9.
 *
 * Two modules, and the split is the whole point:
 *
 *   :protocol   pure Kotlin/JVM. No Android, no Bluetooth, no coroutines --
 *               docs/bridge-protocol.md and nothing else. It builds and its
 *               tests run on any JDK, which is what lets the protocol be checked
 *               against the same shared vectors the firmware and the PWA use.
 *
 *   :app        the Android application: BLE, the foreground service that is the
 *               entire reason phase 9 exists, and a UI. Not written yet.
 *
 * CLAUDE.md 4.2: "Nothing in the firmware or the server may assume the client is
 * a browser." The mirror of that is that nothing in :protocol may assume it is
 * an Android app.
 */
/*
 * Plugin resolution looks here and nowhere else.
 *
 * The Android Gradle Plugin lives in Google's repository, and allprojects
 * { repositories } does not apply to plugin resolution -- it applies to
 * dependencies. Leaving google() out of this block produces "plugin was not
 * found in any of the following sources", which names Gradle's own portal and
 * nothing else and does not hint at why.
 */
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

rootProject.name = "maxl-android"

include(":protocol")
include(":app")
