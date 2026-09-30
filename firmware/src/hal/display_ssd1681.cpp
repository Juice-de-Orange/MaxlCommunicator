#include "hal/display_ssd1681.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <GxEPD2_BW.h>
#include <SPI.h>
#pragma GCC diagnostic pop

namespace hal {
namespace {

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_panel(
    GxEPD2_154_D67(PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RES, PIN_EINK_BUSY));

/// 4 MHz, MSB first, mode 0 -- the settings bring-up sketch 08 ran on node A.
SPISettings panelSpi() { return SPISettings(4000000, MSBFIRST, SPI_MODE0); }

/*
 * Which edge of the panel is "up".
 *
 * Adafruit_GFX rotation, applied through drawBitmap below, so it turns every
 * screen at once rather than each one having to know. The panel is square, so a
 * quarter turn costs no geometry: 200x200 either way.
 *
 * Measured, not chosen. With rotation 0 the bring-up 20 label came out lying on
 * its side on both boards -- observed on 2026-08-31 with the device held
 * the way it is meant to be held, buttons at the top and bottom edge. 3 is a
 * quarter turn anticlockwise from that.
 *
 * If a screen ever comes out upside down, this constant is the one place to
 * change, and docs/test-plan.md gate 4.2 is what would catch it.
 */
constexpr uint8_t kRotation = 3;

void paint(const Canvas &canvas)
{
    /*
     * The whole canvas is drawn every time, whatever the window is. GxEPD2 clips
     * to the window it was given, so a partial window costs a partial transfer
     * to the panel while the code above it stays simple -- and the alternative,
     * drawing only the changed part, would need every screen to know which part
     * that was.
     */
    g_panel.firstPage();
    do {
        g_panel.fillScreen(GxEPD_WHITE);
        g_panel.drawBitmap(0, 0, canvas.buffer(), Canvas::kWidth, Canvas::kHeight, GxEPD_BLACK);
    } while (g_panel.nextPage());
}

/// Clamp a region to the panel, and treat an empty one as the whole panel.
Rect clampRegion(const Rect &region)
{
    if (region.empty()) {
        return Rect{0, 0, static_cast<int16_t>(Canvas::kWidth),
                    static_cast<int16_t>(Canvas::kHeight)};
    }

    Rect out = region;
    if (out.x < 0) {
        out.w = static_cast<int16_t>(out.w + out.x);
        out.x = 0;
    }
    if (out.y < 0) {
        out.h = static_cast<int16_t>(out.h + out.y);
        out.y = 0;
    }
    if (out.x + out.w > static_cast<int16_t>(Canvas::kWidth)) {
        out.w = static_cast<int16_t>(Canvas::kWidth - out.x);
    }
    if (out.y + out.h > static_cast<int16_t>(Canvas::kHeight)) {
        out.h = static_cast<int16_t>(Canvas::kHeight - out.y);
    }
    return out;
}

} // namespace

bool DisplaySsd1681::begin()
{
    pinMode(PIN_EINK_BL, OUTPUT);
    digitalWrite(PIN_EINK_BL, LOW);

    // Arguments in order: no serial diagnostic output, initial full refresh,
    // 2 ms reset pulse, no pulldown reset mode, then the bus.
    g_panel.init(0, true, 2, false, SPI1, panelSpi());
    g_panel.setRotation(kRotation);
    g_panel.setTextColor(GxEPD_BLACK);

    ready_ = true;
    asleep_ = false;
    return true;
}

void DisplaySsd1681::present(const Canvas &canvas, RefreshKind kind, const Rect &region)
{
    if (!ready_ || kind == RefreshKind::None) {
        return;
    }

    if (asleep_) {
        // hibernate() drops the panel's own supply; anything written to it
        // afterwards goes nowhere. GxEPD2 wakes it inside init(), and re-running
        // init is the documented way back.
        g_panel.init(0, false, 2, false, SPI1, panelSpi());
        // init() is the documented way back from hibernate, and it re-runs the
        // constructor's defaults -- so the rotation has to be restated here or
        // the first screen after a sleep comes back turned.
        g_panel.setRotation(kRotation);
        asleep_ = false;
    }

    const uint32_t started = millis();

    if (kind == RefreshKind::Full) {
        g_panel.setFullWindow();
    } else {
        const Rect window = clampRegion(region);
        g_panel.setPartialWindow(window.x, window.y, window.w, window.h);
    }
    paint(canvas);

    lastRefreshMs_ = millis() - started;
    if (kind == RefreshKind::Full) {
        if (lastRefreshMs_ > worstFullMs_) {
            worstFullMs_ = lastRefreshMs_;
        }
    } else {
        if (lastRefreshMs_ > worstPartialMs_) {
            worstPartialMs_ = lastRefreshMs_;
        }
    }
}

void DisplaySsd1681::sleep()
{
    if (!ready_ || asleep_) {
        return;
    }
    setFrontLight(false);
    g_panel.hibernate();
    asleep_ = true;
}

void DisplaySsd1681::setFrontLight(bool on)
{
    pinMode(PIN_EINK_BL, OUTPUT);
    digitalWrite(PIN_EINK_BL, on ? HIGH : LOW);
}

} // namespace hal
