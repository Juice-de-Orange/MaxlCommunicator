package io.github.juice_de_orange.maxlcommunicator.protocol

/**
 * Little-endian reading and writing, and nothing else.
 *
 * docs/bridge-protocol.md section 1.2: "All multi-byte integers are little
 * endian. Strings are UTF-8, length-prefixed, never null-terminated."
 *
 * Kotlin's Byte is signed, which is the single most reliable source of bugs in a
 * binary protocol written in it -- 0xC0 read as a Byte is -64. Every read here
 * goes through `toInt() and 0xFF` for exactly that reason, and nothing outside
 * this file touches a raw Byte.
 */
internal class ByteReader(private val bytes: ByteArray, private var at: Int = 0) {
    val remaining: Int get() = bytes.size - at

    fun require(count: Int) {
        if (remaining < count) {
            throw BridgeFormatException("need $count more bytes, have $remaining")
        }
    }

    fun u8(): Int {
        require(1)
        return bytes[at++].toInt() and 0xFF
    }

    fun i8(): Int {
        require(1)
        return bytes[at++].toInt()
    }

    fun u16(): Int {
        require(2)
        return u8() or (u8() shl 8)
    }

    fun i16(): Int {
        val value = u16()
        return if (value >= 0x8000) value - 0x10000 else value
    }

    fun u32(): Long {
        require(4)
        return u8().toLong() or (u8().toLong() shl 8) or (u8().toLong() shl 16) or
            (u8().toLong() shl 24)
    }

    fun i32(): Int {
        require(4)
        return u8() or (u8() shl 8) or (u8() shl 16) or (u8() shl 24)
    }

    fun bytes(count: Int): ByteArray {
        require(count)
        val out = bytes.copyOfRange(at, at + count)
        at += count
        return out
    }

    fun utf8(count: Int): String = String(bytes(count), Charsets.UTF_8)

    fun rest(): ByteArray = bytes(remaining)
}

internal class ByteWriter {
    private val out = ArrayList<Byte>(64)

    fun u8(value: Int) = apply { out.add((value and 0xFF).toByte()) }

    fun u16(value: Int) = apply { u8(value); u8(value shr 8) }

    fun u32(value: Long) = apply {
        u8((value and 0xFF).toInt())
        u8(((value shr 8) and 0xFF).toInt())
        u8(((value shr 16) and 0xFF).toInt())
        u8(((value shr 24) and 0xFF).toInt())
    }

    fun i32(value: Int) = apply { u32(value.toLong() and 0xFFFFFFFFL) }

    fun bytes(value: ByteArray) = apply { value.forEach { out.add(it) } }

    fun finish(): ByteArray = out.toByteArray()
}

/** The protocol was violated by the other end, or by us. Never swallowed. */
class BridgeFormatException(message: String) : IllegalArgumentException(message)
