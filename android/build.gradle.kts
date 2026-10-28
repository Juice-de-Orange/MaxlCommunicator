plugins {
    kotlin("jvm") version "2.4.21" apply false
    kotlin("android") version "2.4.21" apply false
    id("com.android.application") version "8.5.2" apply false
}

// Repositories are declared in settings.gradle.kts, for both plugins and
// dependencies. Declaring them here as well would be a second source of truth
// and Gradle 8 warns about it.
