/*
 * IDisplay on the T-Echo's 1.54" panel, through GxEPD2.
 *
 * The driver class is GxEPD2_154_D67 (SSD1681), pinned by CLAUDE.md 0.1. Open
 * decision D1 records that gate 0.2's "reads back SSD1681 ID" is not achievable
 * on this hardware -- the controller has no chip id register and the FPC's MISO
 * is frequently not connected -- so the panel is identified by rendering
 * correctly, not by answering a question. If the geometry comes out wrong, D1
 * names GxEPD2_154_GDEY0154D67 and GxEPD2_154 as the next candidates.
 *
 * The init() arguments are not guesses. They are what bring-up sketch 08 ran on
 * node A: SPI1 explicitly, because SPI0 belongs to the SX1262 and sharing a bus
 * between a radio that must answer an interrupt and a panel that holds the bus
 * for 300 ms is not a trade worth making.
 *
 * Drawing goes through Adafruit_GFX's drawBitmap rather than GxEPD2's
 * writeImage, and that is deliberate. drawBitmap's contract is unambiguous -- a
 * set bit is drawn in the colour you pass -- whereas writeImage inherits the
 * panel's own convention, in which a set bit is white. Canvas says a set bit is
 * black (see canvas.h). Going through drawBitmap means the polarity is fixed by
 * an interface contract instead of by somebody remembering which way round this
 * particular controller stores its RAM. It costs a per-pixel loop over 40000
 * pixels, which is a few milliseconds against a panel update measured in
 * hundreds.
 */

#ifndef MAXL_HAL_DISPLAY_SSD1681_H
#define MAXL_HAL_DISPLAY_SSD1681_H

#include "hal/i_display.h"

#include <stdint.h>

namespace hal {

class DisplaySsd1681 : public IDisplay {
public:
    bool begin() override;
    void present(const Canvas &canvas, RefreshKind kind, const Rect &region) override;
    using IDisplay::present;
    void sleep() override;
    uint32_t lastRefreshMs() const override { return lastRefreshMs_; }

    /// Longest present() seen since begin(). Gate 1.1 asks for twenty partials
    /// each under 400 ms, so the worst one is the number that decides it.
    uint32_t worstPartialMs() const { return worstPartialMs_; }
    uint32_t worstFullMs() const { return worstFullMs_; }

    /// The front light on P1.11. Off at boot and off after every use -- it is
    /// tens of milliamps and CLAUDE.md 3.1 budgets nothing for it.
    void setFrontLight(bool on);

private:
    bool ready_ = false;
    bool asleep_ = false;
    uint32_t lastRefreshMs_ = 0;
    uint32_t worstPartialMs_ = 0;
    uint32_t worstFullMs_ = 0;
};

} // namespace hal

#endif // MAXL_HAL_DISPLAY_SSD1681_H
