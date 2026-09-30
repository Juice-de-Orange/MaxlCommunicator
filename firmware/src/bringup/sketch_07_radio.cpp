/*
 * Bring-up 07 -- the SX1262, read only. Nothing is transmitted.
 *
 * CLAUDE.md and docs/test-plan.md both open with "never power the node without
 * an antenna". An antenna is fitted and transmission has been cleared, but phase
 * 0 has no business radiating: the point here is only that the pins are right
 * and the chip is alive. Not one command below puts the radio into TX.
 *
 * Talking to the chip directly rather than through RadioLib is deliberate. This
 * sketch has to be able to say "the BUSY line never went low" or "MISO is stuck
 * high", and a driver that retries and abstracts is exactly what stands between
 * that observation and the pin that caused it. RadioLib arrives in
 * hal/radio_sx1262 with the pin map already proven.
 *
 * The evidence is the LoRa sync word register at 0x0740. It powers up as
 * 0x1424, a value that cannot be produced by a floating bus, a stuck line or a
 * chip that is not there -- unlike 0x00 or 0xFF, which any of those give you.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 7

#include <Arduino.h>

// The core's SPI.h has a constructor whose parameters shadow its own members.
// Vendor header, not ours, and -Wshadow is worth keeping everywhere else.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <SPI.h>
#pragma GCC diagnostic pop

#include "common.h"
#include "report.h"

namespace {

constexpr uint8_t kCmdGetStatus = 0xC0;
constexpr uint8_t kCmdReadRegister = 0x1D;
constexpr uint8_t kCmdSetStandby = 0x80;
constexpr uint8_t kStandbyRc = 0x00;

constexpr uint16_t kRegLoRaSyncWord = 0x0740;
constexpr uint16_t kSyncWordDefault = 0x1424;

SPISettings g_spi(2000000, MSBFIRST, SPI_MODE0);

/// The SX1262 holds BUSY high while it is not ready for a command. Every
/// transaction has to wait for it; skipping the wait is the classic way to get
/// an intermittent driver that mostly works.
bool waitBusyLow(uint32_t timeoutMs)
{
    const uint32_t deadline = millis() + timeoutMs;
    while (digitalRead(SX126X_BUSY) == HIGH) {
        if (millis() > deadline) {
            return false;
        }
    }
    return true;
}

bool command(uint8_t opcode, const uint8_t *out, uint8_t *in, size_t len)
{
    if (!waitBusyLow(100)) {
        return false;
    }
    SPI.beginTransaction(g_spi);
    digitalWrite(SX126X_CS, LOW);
    SPI.transfer(opcode);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t sent = out != nullptr ? out[i] : 0x00;
        const uint8_t received = SPI.transfer(sent);
        if (in != nullptr) {
            in[i] = received;
        }
    }
    digitalWrite(SX126X_CS, HIGH);
    SPI.endTransaction();
    return true;
}

bool readRegister(uint16_t address, uint8_t *out, size_t len)
{
    if (!waitBusyLow(100)) {
        return false;
    }
    SPI.beginTransaction(g_spi);
    digitalWrite(SX126X_CS, LOW);
    SPI.transfer(kCmdReadRegister);
    SPI.transfer(static_cast<uint8_t>(address >> 8));
    SPI.transfer(static_cast<uint8_t>(address & 0xFF));
    SPI.transfer(0x00); // one status byte before the data
    for (size_t i = 0; i < len; ++i) {
        out[i] = SPI.transfer(0x00);
    }
    digitalWrite(SX126X_CS, HIGH);
    SPI.endTransaction();
    return true;
}

void hardReset()
{
    pinMode(SX126X_RESET, OUTPUT);
    digitalWrite(SX126X_RESET, LOW);
    delay(2);
    digitalWrite(SX126X_RESET, HIGH);
    delay(20);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(120000);

    pinMode(SX126X_CS, OUTPUT);
    digitalWrite(SX126X_CS, HIGH);
    pinMode(SX126X_BUSY, INPUT);
    pinMode(SX126X_DIO1, INPUT);
    SPI.begin();
}

void loop()
{
    if (!usbReady()) {
        delay(50);
        commonService();
        return;
    }

    report::begin(7);
    report::info("sck=P0.19 miso=P0.23 mosi=P0.22 cs=P0.24 rst=P0.25 busy=P0.17 dio1=P0.20");
    report::info("read only -- no command below transmits");

    const int busyBeforeReset = digitalRead(SX126X_BUSY);
    hardReset();
    const bool busyWentLow = waitBusyLow(1000);
    report::value("busy.before_reset", "%d", busyBeforeReset);
    report::value("busy.low_after_reset", "%d", busyWentLow ? 1 : 0);

    uint8_t status = 0;
    const bool statusOk = command(kCmdGetStatus, nullptr, &status, 1);
    // Bits 6:4 chip mode, 3:1 command status. 0x00 and 0xFF are what a dead bus
    // gives, so they are called out rather than decoded.
    report::value("status.raw", "0x%02X", status);
    report::value("status.chip_mode", "%u", static_cast<unsigned>((status >> 4) & 0x07));
    report::value("status.cmd_status", "%u", static_cast<unsigned>((status >> 1) & 0x07));

    const uint8_t standby[1] = {kStandbyRc};
    command(kCmdSetStandby, standby, nullptr, 1);
    delay(2);

    uint8_t syncWord[2] = {0, 0};
    const bool regOk = readRegister(kRegLoRaSyncWord, syncWord, sizeof(syncWord));
    const uint16_t sync = static_cast<uint16_t>((syncWord[0] << 8) | syncWord[1]);
    report::value("reg.0x0740.sync_word", "0x%04X", sync);
    report::value("reg.expected", "0x%04X", kSyncWordDefault);

    const bool alive = statusOk && regOk && busyWentLow && sync == kSyncWordDefault;
    report::verdict(alive ? "pass" : "fail",
                    alive ? "BUSY handshakes and the sync word register reads its reset value"
                          : "the SX1262 did not answer as itself on these pins");
    report::end(7);

    delay(4000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 7
