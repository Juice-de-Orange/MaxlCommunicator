/*
 * Bring-up 20 -- write which node this is on its own panel.
 *
 * Two identical T-Echos on one desk look identical, and from 2026-08-31 there
 * are two. The USB serial tells them apart in software (tools/nodes.py) and
 * nothing tells them apart in the hand: the blue and red lamps belong to the
 * charge circuit, no firmware drives them, and P0.14 is the only pin that is an
 * LED on both hardware revisions.
 *
 * E-paper holds its image with no power at all, so a letter written here is a
 * label that survives flashing, discharging and a night in a drawer.
 *
 * THE LETTER IS NOT A BUILD FLAG. The node reads its own nRF52840 DEVICEID and
 * looks itself up, so the same image on the wrong board cannot mislabel it -- it
 * says '?' instead, which is the honest answer. The ids are the ones in your
 * firmware/nodes.ini (copied from nodes.example.ini) and are factory chip ids,
 * not secrets -- but they identify your hardware, so they are not in the
 * repository. Pass them as build flags, e.g.
 *
 *   PLATFORMIO_BUILD_FLAGS="-DMAXL_NODE_A_ID_HIGH=0x0123ABCDul -DMAXL_NODE_A_ID_LOW=0x4567EF01ul"
 *
 * Without them every board says '?'.
 *
 * The panel is on SPI1; SPI0 belongs to the SX1262 and is not touched here.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 20

#include <Arduino.h>

#include "common.h"
#include "report.h"

#include "hal/canvas.h"
#include "hal/display_ssd1681.h"

namespace bringup {
namespace {

/*
 * File scope, not locals: hal::Canvas is 5000 bytes and the loop task's stack is
 * 4096. -Wframe-larger-than=1024 would refuse it anyway, and it is in the build
 * because a single 8960-byte local took node A off the USB bus on 2026-08-31.
 */
hal::DisplaySsd1681 g_display;
hal::Canvas g_canvas;

bool g_displayOk = false;
bool g_drawn = false;
char g_letter = '?';

/// The two boards, by the id their own silicon reports. Same values as your
/// firmware/nodes.ini, which is what tools/nodes.py resolves --node against.
struct KnownNode {
    uint32_t idHigh;   ///< DEVICEID[1], the first eight hex digits of the serial
    uint32_t idLow;    ///< DEVICEID[0], the last eight
    char letter;
};

#ifndef MAXL_NODE_A_ID_HIGH
#define MAXL_NODE_A_ID_HIGH 0x00000000ul
#endif
#ifndef MAXL_NODE_A_ID_LOW
#define MAXL_NODE_A_ID_LOW 0x00000000ul
#endif
#ifndef MAXL_NODE_B_ID_HIGH
#define MAXL_NODE_B_ID_HIGH 0x00000000ul
#endif
#ifndef MAXL_NODE_B_ID_LOW
#define MAXL_NODE_B_ID_LOW 0x00000000ul
#endif

constexpr KnownNode kKnownNodes[] = {
    {MAXL_NODE_A_ID_HIGH, MAXL_NODE_A_ID_LOW, 'A'},
    {MAXL_NODE_B_ID_HIGH, MAXL_NODE_B_ID_LOW, 'B'},
};

char letterForThisBoard()
{
    const uint32_t high = NRF_FICR->DEVICEID[1];
    const uint32_t low = NRF_FICR->DEVICEID[0];
    for (const KnownNode &node : kKnownNodes) {
        if (node.idHigh == high && node.idLow == low) {
            return node.letter;
        }
    }
    return '?';
}

/*
 * 'A', 'B' and '?' as 5x7 bitmaps, one bit per pixel, MSB of each row leftmost.
 *
 * hal::Canvas has a 5x7 font already, but only at scale 1 and 2 (textLarge), and
 * a letter meant to be read across a desk needs about twenty. Scaling three
 * glyphs by hand is cheaper than a second font and costs no flash worth counting.
 */
struct Glyph {
    char c;
    uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
};

void drawGlyph(char c, int16_t x, int16_t y, int16_t scale)
{
    const Glyph *glyph = &kGlyphs[2]; // '?' unless we know better
    for (const Glyph &candidate : kGlyphs) {
        if (candidate.c == c) {
            glyph = &candidate;
            break;
        }
    }
    for (int16_t row = 0; row < 7; ++row) {
        for (int16_t col = 0; col < 5; ++col) {
            if ((glyph->rows[row] >> (4 - col)) & 1) {
                g_canvas.fillRect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

void draw()
{
    g_canvas.clear();

    // A border, so a half-written panel is obvious at a glance.
    g_canvas.rect(0, 0, hal::Canvas::kWidth, hal::Canvas::kHeight, true);

    constexpr int16_t kScale = 20;
    constexpr int16_t kGlyphW = 5 * kScale;
    constexpr int16_t kGlyphH = 7 * kScale;
    drawGlyph(g_letter, (hal::Canvas::kWidth - kGlyphW) / 2, 24, kScale);

    // The serial underneath, so the label can be checked rather than believed.
    char serial[17];
    snprintf(serial, sizeof(serial), "%08lX%08lX",
             static_cast<unsigned long>(NRF_FICR->DEVICEID[1]),
             static_cast<unsigned long>(NRF_FICR->DEVICEID[0]));

    const int16_t serialY = 24 + kGlyphH + 14;
    g_canvas.text((hal::Canvas::kWidth - hal::Canvas::textWidth(serial)) / 2, serialY, serial);

    const char *hint = "tools/nodes.py --list";
    g_canvas.text((hal::Canvas::kWidth - hal::Canvas::textWidth(hint)) / 2,
                  serialY + hal::Canvas::kCharHeight + 4, hint);
}

} // namespace

void setup()
{
    commonSetup(120000);
    g_letter = letterForThisBoard();
    g_displayOk = g_display.begin();
}

void loop()
{
    if (!usbReady()) {
        delay(100);
        commonService();
        return;
    }

    if (!g_drawn && g_displayOk) {
        draw();
        // Full, not partial: this image is meant to outlive everything, and a
        // full refresh is the only one that leaves no ghost behind.
        g_display.present(g_canvas, hal::RefreshKind::Full);
        g_display.sleep();
        g_drawn = true;
    }

    report::begin(20);
    report::value("display.begin", "%d", g_displayOk ? 1 : 0);
    report::value("node.letter", "%c", g_letter);
    report::value("node.serial", "%08lX%08lX",
                  static_cast<unsigned long>(NRF_FICR->DEVICEID[1]),
                  static_cast<unsigned long>(NRF_FICR->DEVICEID[0]));
    report::value("label.drawn", "%d", g_drawn ? 1 : 0);
    report::value("display.full_ms", "%lu",
                  static_cast<unsigned long>(g_display.lastRefreshMs()));
    if (!g_displayOk) {
        report::verdict("fail", "the panel did not come up, so nothing was labelled");
    } else if (g_letter == '?') {
        report::verdict("inconclusive",
                        "this board is not in the table -- add it to nodes.ini and here");
    } else {
        report::verdict("pass", "the letter is on the panel and survives power loss");
    }
    report::end(20);

    delay(3000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 20
