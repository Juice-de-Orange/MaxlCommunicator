/*
 * A 200x200 one-bit drawing surface, and the seam that makes the UI testable.
 *
 * CLAUDE.md 1.6 fixes the panel at 200x200 monochrome. Everything the UI draws
 * lands here first and is only then pushed to the panel by an IDisplay, which is
 * the whole point of the split: screens render against a plain byte array with
 * no SSD1681, no SPI and no Arduino anywhere in sight, so docs/test-plan.md gate
 * 4.1 -- "zero partial refreshes in an hour with no state change" -- becomes a
 * counter a host test reads rather than something somebody has to sit and watch.
 *
 * One bit per pixel, MSB first, eight pixels to a byte, rows packed left to
 * right and top to bottom. 200/8 = 25 bytes per row, 5000 bytes for the buffer.
 * That layout is not arbitrary -- it is exactly what the SSD1681 wants in its
 * RAM, so display_ssd1681 hands the buffer over without repacking it.
 *
 * `true` means black. E-paper is black on white and the panel's own convention
 * is inverted (a set bit is white), which display_ssd1681 deals with in the one
 * place that knows about the panel.
 *
 * No allocation, no exceptions, every coordinate clipped. Drawing outside the
 * surface is a no-op rather than a fault: a screen that draws one pixel too far
 * should look wrong, not reboot the device in the field.
 */

#ifndef MAXL_HAL_CANVAS_H
#define MAXL_HAL_CANVAS_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

/// A rectangle on the panel. Used for partial-refresh windows.
struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
};

class Canvas {
public:
    static constexpr uint16_t kWidth = 200;
    static constexpr uint16_t kHeight = 200;
    static constexpr uint16_t kStride = kWidth / 8;          ///< 25 bytes per row
    static constexpr size_t kBufferSize = kStride * kHeight; ///< 5000 bytes

    /// Character cell of the built-in font, glyph plus its one-pixel gaps.
    static constexpr uint8_t kCharWidth = 6;
    static constexpr uint8_t kCharHeight = 8;

    Canvas() { clear(); }

    /// Fill the whole surface. Default is white, which is what a screen starts from.
    void clear(bool black = false);

    void setPixel(int16_t x, int16_t y, bool black);
    bool pixel(int16_t x, int16_t y) const;

    void hLine(int16_t x, int16_t y, int16_t w, bool black = true);
    void vLine(int16_t x, int16_t y, int16_t h, bool black = true);
    void rect(int16_t x, int16_t y, int16_t w, int16_t h, bool black = true);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, bool black = true);

    /// One line of text in the built-in 5x7 font. Returns the x just past the
    /// last glyph, so callers can chain without recomputing widths.
    int16_t text(int16_t x, int16_t y, const char *s, bool black = true);

    /// Same, doubled in both directions. The STATUS screen needs one big number
    /// and a second font would cost more flash than a scale factor.
    int16_t textLarge(int16_t x, int16_t y, const char *s, bool black = true);

    /// Width in pixels a string will occupy, without drawing it. Used for
    /// right-aligning values against the left-aligned labels beside them.
    static int16_t textWidth(const char *s, uint8_t scale = 1);

    /// Raw buffer, in the panel's own packing. display_ssd1681 pushes this
    /// straight out; nothing else should need it.
    const uint8_t *buffer() const { return buf_; }
    uint8_t *buffer() { return buf_; }

    /// Byte-for-byte comparison. This is what turns "did the screen change?"
    /// into a decision the refresh policy can make (CLAUDE.md 1.6: "Redraw on
    /// state change only"), instead of a redraw on every pass of the loop.
    bool sameAs(const Canvas &other) const;

    /*
     * The rectangle two canvases differ in, and the reason it is worth computing.
     *
     * Measured on node A, 2026-08-31, bring-up sketch 11 against sketch 08:
     *
     *   partial refresh of the whole 200x200 panel   471 ms
     *   partial refresh of a 152x24 window           323 ms
     *
     * docs/test-plan.md gate 1.1 allows 400 ms. Refreshing the whole panel
     * because one digit changed misses the gate AND spends the difference out of
     * the power budget on every single update. So the display is told what
     * actually moved.
     *
     * x and width are rounded outwards to a multiple of eight, because the
     * SSD1681 addresses its RAM a byte at a time and a window that starts
     * mid-byte is a window the controller widens for you, silently.
     *
     * Returns false when the two are identical, in which case the rectangle is
     * left untouched.
     */
    bool diffBounds(const Canvas &other, Rect &out) const;

    void copyFrom(const Canvas &other);

    /// Count of black pixels. Only the tests use it, and they use it a lot --
    /// it is the cheapest way to assert that something was actually drawn.
    uint32_t inkCount() const;

private:
    int16_t drawGlyph(int16_t x, int16_t y, char c, bool black, uint8_t scale);

    uint8_t buf_[kBufferSize];
};

} // namespace hal

#endif // MAXL_HAL_CANVAS_H
