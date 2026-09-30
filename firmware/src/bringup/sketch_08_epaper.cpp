/*
 * Bring-up 08 -- the e-paper panel.
 *
 * Open decision D1 says gate 0.2 as written ("reads back SSD1681 ID") is
 * probably not achievable: the SSD1681 has no chip id register, and on an
 * e-paper FPC the MISO line is frequently not even connected. D1 proposes
 * replacing it with three criteria. This sketch settles the two that do not
 * need a human:
 *
 *   (3) partial refresh works and lands near 0.3 s   -- measured below
 *       full refresh near 2 s                        -- measured below
 *   (2) a test image renders geometrically correct   -- rendered, and left on
 *       the panel, because e-paper holds its image with no power at all
 *
 * That last point is what makes this worth doing unattended. The pattern below
 * is still on the screen in the morning, and it is drawn so that the failure
 * modes are distinguishable at a glance rather than needing a caliper:
 *
 *   - a one-pixel border, so a shifted origin or a clipped edge is obvious
 *   - filled squares in three corners only, so a mirrored or rotated frame
 *     cannot be mistaken for a correct one
 *   - both diagonals, which cross exactly at the centre if the geometry is right
 *   - a ruler of one-pixel lines at widening gaps, which is where an off-by-one
 *     in the row stride shows up first
 *
 * The panel is on SPI1 (P0.31 SCK, P0.29 MOSI, P1.06 MISO); SPI0 belongs to the
 * SX1262 and is not touched here.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 8

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <SPI.h>
#include <GxEPD2_BW.h>
#pragma GCC diagnostic pop

#include "common.h"
#include "report.h"

namespace {

/// GxEPD2_154_D67 is the SSD1681 driver named in CLAUDE.md 0.1. If the geometry
/// comes out wrong, D1 lists GxEPD2_154_GDEY0154D67 and GxEPD2_154 as the next
/// candidates -- which is a decision for whoever looks at the panel, not for
/// this sketch.
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_display(
    GxEPD2_154_D67(PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RES, PIN_EINK_BUSY));

constexpr int kWidth = 200;
constexpr int kHeight = 200;
constexpr uint32_t kPartialCount = 20;

bool g_measured = false;

/*
 * Measured once, printed for ever after.
 *
 * The first version printed its report and then set a done flag, which meant the
 * report existed for exactly one pass of the loop -- and that pass happens
 * before tools/bringup_run.py has the port open. The panel work must not repeat
 * (twenty partial refreshes every three seconds would be absurd), but the
 * report must, because a report nobody can read is not a report.
 */
struct Measurements {
    uint32_t initMs = 0;
    int width = 0;
    int height = 0;
    bool hasPartial = false;
    uint32_t fullMs = 0;
    uint32_t partialWorstMs = 0;
    uint32_t partialMeanMs = 0;
};

Measurements g_measurements;

void drawTestPattern(uint32_t fullMs)
{
    g_display.setFullWindow();
    g_display.firstPage();
    do {
        g_display.fillScreen(GxEPD_WHITE);

        // One-pixel border: a clipped edge or an off-by-one origin shows here.
        g_display.drawRect(0, 0, kWidth, kHeight, GxEPD_BLACK);

        // Three corners only. A mirrored frame puts the empty corner somewhere
        // other than bottom right, and that is visible from across the room.
        g_display.fillRect(2, 2, 10, 10, GxEPD_BLACK);
        g_display.fillRect(kWidth - 12, 2, 10, 10, GxEPD_BLACK);
        g_display.fillRect(2, kHeight - 12, 10, 10, GxEPD_BLACK);

        // Both diagonals: they cross at the centre if nothing is skewed.
        g_display.drawLine(0, 0, kWidth - 1, kHeight - 1, GxEPD_BLACK);
        g_display.drawLine(kWidth - 1, 0, 0, kHeight - 1, GxEPD_BLACK);

        // A ruler of single-pixel verticals at widening gaps. If the row stride
        // is wrong these smear or disappear before anything else does.
        for (int i = 0, x = 20; x < kWidth - 20; ++i, x += 2 + i) {
            g_display.drawFastVLine(x, kHeight - 40, 18, GxEPD_BLACK);
        }

        g_display.setTextColor(GxEPD_BLACK);
        g_display.setCursor(24, 70);
        g_display.print("MAXL bringup 08");
        g_display.setCursor(24, 86);
        g_display.print("SSD1681 / 200x200");
        g_display.setCursor(24, 110);
        g_display.printf("full %lu ms", static_cast<unsigned long>(fullMs));
    } while (g_display.nextPage());
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(180000);
}

void measure()
{
    Measurements &m = g_measurements;

    // 2 ms reset pulse, no pulldown reset mode; SPI1 explicitly, because the
    // default SPI instance is the radio's.
    const uint32_t initStart = millis();
    g_display.init(0, true, 2, false, SPI1, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    m.initMs = millis() - initStart;
    m.width = static_cast<int>(g_display.width());
    m.height = static_cast<int>(g_display.height());
    m.hasPartial = g_display.epd2.hasPartialUpdate;

    // Full refresh, timed. CLAUDE.md 1.6 says roughly 2 s.
    const uint32_t fullStart = millis();
    drawTestPattern(0);
    const uint32_t fullMs = millis() - fullStart;
    m.fullMs = fullMs;

    // Partial refreshes, timed individually. CLAUDE.md 1.6 says roughly 0.3 s
    // and gate 1.1 wants under 400 ms across 20 consecutive updates -- so all 20
    // are measured and the worst one is reported, not the average. An average
    // hides exactly the outlier the gate is asking about.
    uint32_t worst = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < kPartialCount; ++i) {
        const uint32_t start = millis();
        g_display.setPartialWindow(24, 130, 152, 24);
        g_display.firstPage();
        do {
            g_display.fillScreen(GxEPD_WHITE);
            g_display.drawRect(0, 0, 152, 24, GxEPD_BLACK);
            g_display.setTextColor(GxEPD_BLACK);
            g_display.setCursor(30, 147);
            g_display.printf("partial %lu/%lu", static_cast<unsigned long>(i + 1),
                             static_cast<unsigned long>(kPartialCount));
        } while (g_display.nextPage());
        const uint32_t took = millis() - start;
        total += took;
        if (took > worst) {
            worst = took;
        }
    }
    m.partialWorstMs = worst;
    m.partialMeanMs = total / kPartialCount;

    // Redraw the full pattern so what stays on the panel overnight is the
    // geometry test with the real timing printed into it, not the last partial.
    drawTestPattern(fullMs);

    // hibernate() powers the panel down and leaves the image standing. That is
    // the whole reason this can be judged in the morning.
    g_display.hibernate();
}

void printReport()
{
    const Measurements &m = g_measurements;

    report::begin(8);
    report::info("spi1 sck=P0.31 mosi=P0.29 miso=P1.06; cs=P0.30 dc=P0.28 rst=P0.02 busy=P0.03");
    report::value("epd.init_ms", "%lu", static_cast<unsigned long>(m.initMs));
    report::value("epd.width", "%d", m.width);
    report::value("epd.height", "%d", m.height);
    report::value("epd.has_partial_update", "%d", m.hasPartial ? 1 : 0);
    report::value("epd.full_refresh_ms", "%lu", static_cast<unsigned long>(m.fullMs));
    report::value("epd.partial_count", "%lu", static_cast<unsigned long>(kPartialCount));
    report::value("epd.partial_worst_ms", "%lu", static_cast<unsigned long>(m.partialWorstMs));
    report::value("epd.partial_mean_ms", "%lu", static_cast<unsigned long>(m.partialMeanMs));
    report::info("panel hibernated; the test pattern stays on screen with no power");

    const bool geometryPlausible = m.width == kWidth && m.height == kHeight;
    const bool partialOk = m.partialWorstMs > 0 && m.partialWorstMs < 400;
    if (geometryPlausible && partialOk) {
        report::verdict("inconclusive",
                        "timings meet gate 1.1 and D1 criterion 3; criterion 2 is a look at "
                        "the panel, and the pattern is waiting on it");
    } else {
        report::verdict("fail", geometryPlausible
                                    ? "partial refresh missed 400 ms or never ran"
                                    : "panel geometry is not 200x200");
    }
    report::end(8);
}

void loop()
{
    if (!usbReady()) {
        delay(200);
        commonService();
        return;
    }

    if (!g_measured) {
        measure();
        g_measured = true;
    }

    printReport();
    delay(3000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 8
