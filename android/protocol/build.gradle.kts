import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm")
}

/*
 * Target 17, compile with whatever JDK is present.
 *
 * Not jvmToolchain(17): that demands a JDK 17 specifically and fails on a
 * machine that has 21, which is what android/tools/test.sh runs in. What the
 * Android module will need is 17 *bytecode*, and that is a target, not a
 * compiler.
 */
kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)

        // The same discipline as the firmware's -Wall -Wextra -Werror: a warning
        // nobody has to look at is a warning nobody looks at.
        allWarningsAsErrors.set(true)
    }
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

dependencies {
    testImplementation(kotlin("test"))
}

/*
 * The shared test vectors, emitted as Kotlin.
 *
 * the JSON files under test-vectors/ is read by three implementations now: the TypeScript
 * bridge parses it directly, the firmware compiles a generated C++ header, and
 * this generates a Kotlin object. Written by hand from the specification, never
 * generated from any implementation -- a generator would bake one side's
 * mistakes into the other side's test and make the cross-check worthless.
 */
val vectorsDir = layout.buildDirectory.dir("generated/vectors")

val generateVectors by tasks.registering(Exec::class) {
    val out = vectorsDir.get().file("io/github/juice_de_orange/maxlcommunicator/protocol/Vectors.kt")
    inputs.dir(rootProject.file("../test-vectors"))
    inputs.file(rootProject.file("../tools/gen_vectors.py"))
    outputs.file(out)

    doFirst { out.asFile.parentFile.mkdirs() }
    commandLine(
        "python3",
        rootProject.file("../tools/gen_vectors.py").absolutePath,
        "--lang", "kotlin",
        "--group", "bridge_protocol",
        "--group", "radio_payloads",
        "--out", out.asFile.absolutePath,
    )
}

sourceSets {
    test {
        kotlin.srcDir(vectorsDir)
    }
}

tasks.named("compileTestKotlin") {
    dependsOn(generateVectors)
}
