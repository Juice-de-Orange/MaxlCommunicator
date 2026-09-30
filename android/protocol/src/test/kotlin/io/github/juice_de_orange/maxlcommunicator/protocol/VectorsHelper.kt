package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.assertNotNull

/**
 * The shared vectors, and the rule for using them.
 *
 * the JSON files under test-vectors/ is written by hand from the specification and read by all
 * three implementations: the firmware compiles it to a C++ header, the PWA parses
 * the JSON, and this module generates a Kotlin object from it. None of the three
 * generated it, which is the only reason agreeing with it means anything.
 *
 * A missing vector must fail loudly. A test that silently skips is a test that
 * passes for the wrong reason.
 *
 * (Written without a slash-star anywhere in this comment on purpose: Kotlin
 * block comments NEST, unlike C++ ones, so a stray one opens a second comment
 * that never closes and the compiler blames the end of the file.)
 */
internal fun vector(name: String): ByteArray {
    val found = Vectors.find(name)
    assertNotNull(found, "shared vector '$name' is missing -- check tools/gen_vectors.py")
    return found.bytes
}

internal fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }
