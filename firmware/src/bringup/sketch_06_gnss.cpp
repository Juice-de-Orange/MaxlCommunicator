/*
 * Bring-up 06 -- the L76K GNSS receiver.
 *
 * UART1 on P1.09 (RX) / P1.08 (TX), reset on P1.05 (active low, held >100 ms per
 * the pin map) and wakeup on P1.02. The module sits on VDD_POWR.
 *
 * Gate 1.7 has two halves and they are not equally important. Time to first fix
 * needs a sky view and cannot be had at a desk indoors; a cold start here will
 * simply never fix, and that is not a failure of anything. The second half is
 * the one that decides the power budget: CLAUDE.md 1.5 says GNSS is powered down
 * by default and "never left running just in case", and a module that keeps
 * running because a line was not asserted will quietly eat the whole budget in
 * phase 5 -- where it will look like a radio problem.
 *
 * So what this sketch establishes is: does the module talk, and does asserting
 * reset actually stop it talking. Measuring the current is gate 0.4/1.7 and
 * needs an instrument.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 6

#include <Arduino.h>

#include "common.h"
#include "report.h"

namespace {

constexpr uint32_t kGnssBaud = 9600;
constexpr uint32_t kListenMs = 6000;

struct Tally {
    uint32_t bytes;
    uint32_t sentences;
    uint32_t gga;
    uint32_t gsv;
    uint32_t rmc;
    uint8_t satellitesInView;
    bool fixValid;
};

char g_sentence[100];
size_t g_sentenceLen = 0;

/// $GPGGA,time,lat,N,lon,E,quality,numSats,...  -- field 6 is the fix quality and
/// field 7 the satellite count. Parsed by hand: a full NMEA library would be a
/// dependency, and this needs two integers.
void parseGga(const char *sentence, Tally &tally)
{
    int field = 0;
    const char *cursor = sentence;
    while (*cursor != '\0') {
        if (*cursor == ',') {
            ++field;
            const char *value = cursor + 1;
            if (field == 6 && *value != ',') {
                tally.fixValid = (*value != '0');
            } else if (field == 7 && *value != ',') {
                tally.satellitesInView = static_cast<uint8_t>(atoi(value));
            }
        }
        ++cursor;
    }
}

void collect(Tally &tally, uint32_t forMs)
{
    const uint32_t deadline = millis() + forMs;
    g_sentenceLen = 0;
    while (millis() < deadline) {
        while (Serial1.available() > 0) {
            const int raw = Serial1.read();
            if (raw < 0) {
                break;
            }
            ++tally.bytes;
            const char c = static_cast<char>(raw);
            if (c == '\n' || c == '\r') {
                if (g_sentenceLen > 6) {
                    g_sentence[g_sentenceLen] = '\0';
                    ++tally.sentences;
                    // Talker id varies (GP, GN, BD), so match on the sentence
                    // type at offset 3 rather than on the whole prefix.
                    if (strncmp(g_sentence + 3, "GGA", 3) == 0) {
                        ++tally.gga;
                        parseGga(g_sentence, tally);
                    } else if (strncmp(g_sentence + 3, "GSV", 3) == 0) {
                        ++tally.gsv;
                    } else if (strncmp(g_sentence + 3, "RMC", 3) == 0) {
                        ++tally.rmc;
                    }
                }
                g_sentenceLen = 0;
            } else if (g_sentenceLen + 1 < sizeof(g_sentence)) {
                g_sentence[g_sentenceLen++] = c;
            }
        }
        delay(1);
    }
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(180000);

    pinMode(PIN_GPS_RESET, OUTPUT);
    pinMode(PIN_GPS_WAKEUP, OUTPUT);
    digitalWrite(PIN_GPS_WAKEUP, HIGH);

    // Out of reset: low for well over the 100 ms the pin map calls for, then
    // released. The module needs a moment before it starts emitting.
    digitalWrite(PIN_GPS_RESET, LOW);
    delay(200);
    digitalWrite(PIN_GPS_RESET, HIGH);
    delay(500);

    Serial1.begin(kGnssBaud);
}

void loop()
{
    if (!usbReady()) {
        delay(50);
        commonService();
        return;
    }

    report::begin(6);
    report::info("uart1 rx=P1.09 tx=P1.08 reset=P1.05 wakeup=P1.02, %lu baud, listening %lu ms",
                 static_cast<unsigned long>(kGnssBaud), static_cast<unsigned long>(kListenMs));

    Tally running{};
    collect(running, kListenMs);
    report::value("gnss.bytes", "%lu", static_cast<unsigned long>(running.bytes));
    report::value("gnss.sentences", "%lu", static_cast<unsigned long>(running.sentences));
    report::value("gnss.gga", "%lu", static_cast<unsigned long>(running.gga));
    report::value("gnss.gsv", "%lu", static_cast<unsigned long>(running.gsv));
    report::value("gnss.rmc", "%lu", static_cast<unsigned long>(running.rmc));
    report::value("gnss.satellites_in_view", "%u",
                  static_cast<unsigned>(running.satellitesInView));
    report::value("gnss.fix_valid", "%d", running.fixValid ? 1 : 0);

    // Now the half that matters for the power budget: assert reset and check
    // that the module actually goes quiet.
    digitalWrite(PIN_GPS_RESET, LOW);
    delay(300);
    while (Serial1.available() > 0) {
        (void)Serial1.read(); // drop whatever was already in the buffer
    }
    Tally silenced{};
    collect(silenced, 2000);
    report::value("gnss.bytes_while_reset_asserted", "%lu",
                  static_cast<unsigned long>(silenced.bytes));

    digitalWrite(PIN_GPS_RESET, HIGH);
    delay(500);
    Tally recovered{};
    collect(recovered, 3000);
    report::value("gnss.bytes_after_release", "%lu",
                  static_cast<unsigned long>(recovered.bytes));

    const bool talks = running.sentences > 0;
    const bool obeysReset = silenced.bytes == 0 && recovered.bytes > 0;
    if (talks && obeysReset) {
        report::verdict("inconclusive",
                        "module talks NMEA and goes silent on reset; time to first fix and "
                        "actual current draw (gate 1.7) need a sky view and a meter");
    } else {
        report::verdict("fail", talks ? "module did not stop on reset -- it would keep "
                                        "drawing current in DEEP_IDLE"
                                      : "no NMEA on UART1");
    }
    report::end(6);

    delay(5000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 6
