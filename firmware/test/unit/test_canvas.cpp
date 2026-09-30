/*
 * The drawing surface.
 *
 * Not glamorous, but it is the substrate every screen in ui/ is checked against
 * and the packing is the panel's own -- display_ssd1681 hands buffer() straight
 * to the controller without repacking. A bit order error here would show up as a
 * mirrored display and nowhere else.
 */

#include <cstring>

#include "doctest.h"

#include "hal/canvas.h"

using namespace hal;

TEST_CASE("the buffer is the panel's geometry, not something close to it")
{
    CHECK(Canvas::kWidth == 200);
    CHECK(Canvas::kHeight == 200);
    CHECK(Canvas::kStride == 25);
    CHECK(Canvas::kBufferSize == 5000);
}

TEST_CASE("a new canvas is white and clear(true) fills it")
{
    Canvas canvas;
    CHECK(canvas.inkCount() == 0);

    canvas.clear(true);
    CHECK(canvas.inkCount() == Canvas::kWidth * Canvas::kHeight);

    canvas.clear();
    CHECK(canvas.inkCount() == 0);
}

TEST_CASE("pixels land where they are put, MSB first")
{
    Canvas canvas;
    canvas.setPixel(0, 0, true);
    CHECK(canvas.pixel(0, 0));
    CHECK(canvas.buffer()[0] == 0x80);

    canvas.clear();
    canvas.setPixel(7, 0, true);
    CHECK(canvas.buffer()[0] == 0x01);

    canvas.clear();
    canvas.setPixel(8, 0, true);
    CHECK(canvas.buffer()[0] == 0x00);
    CHECK(canvas.buffer()[1] == 0x80);

    // Second row starts one stride in.
    canvas.clear();
    canvas.setPixel(0, 1, true);
    CHECK(canvas.buffer()[Canvas::kStride] == 0x80);
}

TEST_CASE("drawing outside the surface is a no-op, not a fault")
{
    Canvas canvas;
    canvas.setPixel(-1, 0, true);
    canvas.setPixel(0, -1, true);
    canvas.setPixel(Canvas::kWidth, 0, true);
    canvas.setPixel(0, Canvas::kHeight, true);
    canvas.setPixel(30000, 30000, true);
    CHECK(canvas.inkCount() == 0);

    CHECK_FALSE(canvas.pixel(-5, -5));
    CHECK_FALSE(canvas.pixel(1000, 1000));
}

TEST_CASE("lines and rectangles cover exactly what they claim to")
{
    Canvas canvas;
    canvas.hLine(10, 20, 30);
    CHECK(canvas.inkCount() == 30);
    CHECK(canvas.pixel(10, 20));
    CHECK(canvas.pixel(39, 20));
    CHECK_FALSE(canvas.pixel(40, 20));

    canvas.clear();
    canvas.vLine(10, 20, 30);
    CHECK(canvas.inkCount() == 30);

    canvas.clear();
    canvas.fillRect(0, 0, 10, 10);
    CHECK(canvas.inkCount() == 100);

    canvas.clear();
    canvas.rect(0, 0, 10, 10);
    CHECK(canvas.inkCount() == 36);  // 4 * 10 - 4 corners counted twice
}

TEST_CASE("a rectangle clipped at the edge draws only what fits")
{
    Canvas canvas;
    canvas.fillRect(195, 195, 20, 20);
    CHECK(canvas.inkCount() == 25);  // 5 x 5 survives
}

TEST_CASE("text advances by the character cell and reports the same width")
{
    Canvas canvas;
    const int16_t end = canvas.text(0, 0, "ABC");
    CHECK(end == 3 * Canvas::kCharWidth);
    CHECK(Canvas::textWidth("ABC") == 3 * Canvas::kCharWidth);
    CHECK(Canvas::textWidth("ABC", 2) == 6 * Canvas::kCharWidth);
    CHECK(canvas.inkCount() > 0);

    // 33 characters is what a 200 px line holds; the 34th would fall off and the
    // screens in ui/ are laid out against that number.
    CHECK(Canvas::textWidth("012345678901234567890123456789012") <= Canvas::kWidth);
}

TEST_CASE("a space draws nothing and an unknown character draws something")
{
    Canvas canvas;
    canvas.text(0, 0, "   ");
    CHECK(canvas.inkCount() == 0);

    // A UTF-8 lead byte, which a TEXT payload may well carry.
    const char umlaut[] = {static_cast<char>(0xC3), static_cast<char>(0xA4), '\0'};
    canvas.text(0, 0, umlaut);
    CHECK(canvas.inkCount() > 0);   // rendered as '?', not as a wild index
}

TEST_CASE("textLarge is the same glyphs at twice the size")
{
    Canvas small;
    Canvas large;
    small.text(0, 0, "8");
    large.textLarge(0, 0, "8");
    CHECK(large.inkCount() == 4 * small.inkCount());
}

TEST_CASE("sameAs is the byte-for-byte comparison the refresh policy relies on")
{
    Canvas a;
    Canvas b;
    CHECK(a.sameAs(b));

    a.setPixel(100, 100, true);
    CHECK_FALSE(a.sameAs(b));

    b.copyFrom(a);
    CHECK(a.sameAs(b));

    // The difference the policy must not miss: one pixel, in the last byte.
    b.setPixel(199, 199, true);
    CHECK_FALSE(a.sameAs(b));
}

TEST_CASE("identical canvases have no changed region")
{
    Canvas a;
    Canvas b;
    Rect region;
    CHECK_FALSE(a.diffBounds(b, region));

    a.text(10, 10, "hello");
    b.copyFrom(a);
    CHECK_FALSE(a.diffBounds(b, region));
}

TEST_CASE("one changed pixel gives a byte-aligned box around it")
{
    Canvas a;
    Canvas b;
    b.setPixel(100, 50, true);

    Rect region;
    REQUIRE(b.diffBounds(a, region));

    // x = 100 sits in byte column 12, so the window starts at 96 and is 8 wide.
    CHECK(region.x == 96);
    CHECK(region.w == 8);
    CHECK(region.y == 50);
    CHECK(region.h == 1);

    // Byte alignment is the point: the SSD1681 addresses its RAM a byte at a
    // time, and a window starting mid-byte is one the controller widens anyway.
    CHECK(region.x % 8 == 0);
    CHECK(region.w % 8 == 0);
}

TEST_CASE("the box covers everything that moved and nothing more")
{
    Canvas a;
    Canvas b;
    b.setPixel(20, 30, true);
    b.setPixel(60, 90, true);

    Rect region;
    REQUIRE(b.diffBounds(a, region));

    CHECK(region.x == 16);                  // byte column 2
    CHECK(region.x + region.w == 64);       // through byte column 7
    CHECK(region.y == 30);
    CHECK(region.y + region.h == 91);
}

TEST_CASE("a full redraw gives the whole panel and stays inside it")
{
    Canvas a;
    Canvas b;
    b.clear(true);

    Rect region;
    REQUIRE(b.diffBounds(a, region));
    CHECK(region.x == 0);
    CHECK(region.y == 0);
    CHECK(region.w == Canvas::kWidth);
    CHECK(region.h == Canvas::kHeight);
}

TEST_CASE("a change in the last pixel is not lost at the edge")
{
    Canvas a;
    Canvas b;
    b.setPixel(199, 199, true);

    Rect region;
    REQUIRE(b.diffBounds(a, region));
    CHECK(region.x == 192);
    CHECK(region.x + region.w == Canvas::kWidth);
    CHECK(region.y == 199);
    CHECK(region.h == 1);
}

TEST_CASE("a realistic screen update touches a small fraction of the panel")
{
    // The measurement that made this exist: a partial refresh of the whole panel
    // took 471 ms on node A against gate 1.1's 400 ms, while a 152x24 window
    // took 323 ms. A one-line change must produce a window of that order, not
    // the whole screen.
    Canvas before;
    Canvas after;
    before.text(4, 100, "Uptime  2h");
    after.text(4, 100, "Uptime  3h");

    Rect region;
    REQUIRE(after.diffBounds(before, region));
    CHECK(region.h <= 8);
    CHECK(region.w <= 48);
}
