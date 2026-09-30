#include "hal/external_flash.h"

// The core's SPI.h shadows two SPISettings members, and -Wshadow is an error in
// this project's own sources. Same guard as block_store_littlefs.cpp.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

namespace hal {
namespace {

/// Release from deep power-down. Datasheet command, harmless when awake.
constexpr uint8_t kCmdReleaseDeepPowerDown = 0xAB;

/*
 * The two parts LilyGO has shipped in this footprint. Kept here rather than in
 * six sketches: a list that exists once cannot disagree with itself, and
 * CLAUDE.md 0 names the ZD25WQ16B as the confirmed part on this hardware.
 */
const SPIFlash_Device_t kCandidates[] = {ZD25WQ16B, MX25R1635F};

} // namespace

bool openExternalFlash(Adafruit_FlashTransport_QSPI &transport, Adafruit_SPIFlash &flash,
                       uint32_t *jedecBeforeWake)
{
    if (jedecBeforeWake != nullptr) {
        *jedecBeforeWake = flash.getJEDECID();
    }

    // begin() on the transport first: runCommand needs the bus configured, and
    // calling it twice is harmless -- Adafruit_SPIFlash::begin() does the same.
    transport.begin();
    transport.runCommand(kCmdReleaseDeepPowerDown);

    // tRES1 in the datasheet is a few microseconds; a millisecond is free here
    // and this runs once at boot.
    delay(5);

    return flash.begin(kCandidates, sizeof(kCandidates) / sizeof(kCandidates[0]));
}

} // namespace hal
