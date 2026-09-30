plugins {
    kotlin("jvm") version "1.9.22" apply false
    kotlin("android") version "1.9.22" apply false
    id("com.android.application") version "8.2.2" apply false
}

// Repositories are declared in settings.gradle.kts, for both plugins and
// dependencies. Declaring them here as well would be a second source of truth
// and Gradle 8 warns about it.
