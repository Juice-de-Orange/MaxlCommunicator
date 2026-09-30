#include "hal/canvas.h"

namespace hal {
namespace {

/*
 * 5x7 glyphs for ASCII 0x20..0x7E, five columns each, one byte per column with
 * bit 0 at the top. 95 glyphs, 475 bytes -- small enough that a second font
 * would cost more than the scale factor in textLarge() does.
 *
 * The panel is 200 px wide, so a line holds 33 characters at scale 1 and 16 at
 * scale 2. Every screen in ui/ is laid out against those two numbers.
 */
const uint8_t kFont[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x41, 0x22, 0x14, 0x08, 0x00}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x01, 0x01}, // F
    {0x3E, 0x41, 0x41, 0x51, 0x32}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x00, 0x7F, 0x41, 0x41}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x41, 0x41, 0x7F, 0x00, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // ~
};

constexpr char kFirstGlyph = 0x20;
constexpr char kLastGlyph = 0x7E;

/// Anything outside the font -- including every byte of a UTF-8 sequence, which
/// TEXT payloads may well carry -- renders as '?' rather than as a wild index.
const uint8_t *glyphFor(char c)
{
    if (c < kFirstGlyph || c > kLastGlyph) {
        c = '?';
    }
    return kFont[static_cast<uint8_t>(c) - static_cast<uint8_t>(kFirstGlyph)];
}

} // namespace

void Canvas::clear(bool black)
{
    const uint8_t fill = black ? 0xFF : 0x00;
    for (size_t i = 0; i < kBufferSize; ++i) {
        buf_[i] = fill;
    }
}

void Canvas::setPixel(int16_t x, int16_t y, bool black)
{
    if (x < 0 || y < 0 || x >= static_cast<int16_t>(kWidth) || y >= static_cast<int16_t>(kHeight)) {
        return;
    }
    const size_t index = static_cast<size_t>(y) * kStride + static_cast<size_t>(x) / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (static_cast<unsigned>(x) & 7u));
    if (black) {
        buf_[index] = static_cast<uint8_t>(buf_[index] | mask);
    } else {
        buf_[index] = static_cast<uint8_t>(buf_[index] & static_cast<uint8_t>(~mask));
    }
}

bool Canvas::pixel(int16_t x, int16_t y) const
{
    if (x < 0 || y < 0 || x >= static_cast<int16_t>(kWidth) || y >= static_cast<int16_t>(kHeight)) {
        return false;
    }
    const size_t index = static_cast<size_t>(y) * kStride + static_cast<size_t>(x) / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (static_cast<unsigned>(x) & 7u));
    return (buf_[index] & mask) != 0;
}

void Canvas::hLine(int16_t x, int16_t y, int16_t w, bool black)
{
    for (int16_t i = 0; i < w; ++i) {
        setPixel(static_cast<int16_t>(x + i), y, black);
    }
}

void Canvas::vLine(int16_t x, int16_t y, int16_t h, bool black)
{
    for (int16_t i = 0; i < h; ++i) {
        setPixel(x, static_cast<int16_t>(y + i), black);
    }
}

void Canvas::rect(int16_t x, int16_t y, int16_t w, int16_t h, bool black)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    hLine(x, y, w, black);
    hLine(x, static_cast<int16_t>(y + h - 1), w, black);
    vLine(x, y, h, black);
    vLine(static_cast<int16_t>(x + w - 1), y, h, black);
}

void Canvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, bool black)
{
    for (int16_t row = 0; row < h; ++row) {
        hLine(x, static_cast<int16_t>(y + row), w, black);
    }
}

int16_t Canvas::drawGlyph(int16_t x, int16_t y, char c, bool black, uint8_t scale)
{
    const uint8_t *glyph = glyphFor(c);
    for (uint8_t column = 0; column < 5; ++column) {
        const uint8_t bits = glyph[column];
        for (uint8_t row = 0; row < 7; ++row) {
            if ((bits & (1u << row)) == 0) {
                continue;
            }
            for (uint8_t sx = 0; sx < scale; ++sx) {
                for (uint8_t sy = 0; sy < scale; ++sy) {
                    setPixel(static_cast<int16_t>(x + column * scale + sx),
                             static_cast<int16_t>(y + row * scale + sy),
                             black);
                }
            }
        }
    }
    return static_cast<int16_t>(x + kCharWidth * scale);
}

int16_t Canvas::text(int16_t x, int16_t y, const char *s, bool black)
{
    if (s == nullptr) {
        return x;
    }
    int16_t cursor = x;
    for (const char *p = s; *p != '\0'; ++p) {
        cursor = drawGlyph(cursor, y, *p, black, 1);
    }
    return cursor;
}

int16_t Canvas::textLarge(int16_t x, int16_t y, const char *s, bool black)
{
    if (s == nullptr) {
        return x;
    }
    int16_t cursor = x;
    for (const char *p = s; *p != '\0'; ++p) {
        cursor = drawGlyph(cursor, y, *p, black, 2);
    }
    return cursor;
}

int16_t Canvas::textWidth(const char *s, uint8_t scale)
{
    if (s == nullptr) {
        return 0;
    }
    int16_t count = 0;
    for (const char *p = s; *p != '\0'; ++p) {
        ++count;
    }
    return static_cast<int16_t>(count * kCharWidth * scale);
}

bool Canvas::sameAs(const Canvas &other) const
{
    for (size_t i = 0; i < kBufferSize; ++i) {
        if (buf_[i] != other.buf_[i]) {
            return false;
        }
    }
    return true;
}

bool Canvas::diffBounds(const Canvas &other, Rect &out) const
{
    int16_t minByte = static_cast<int16_t>(kStride);
    int16_t maxByte = -1;
    int16_t minRow = static_cast<int16_t>(kHeight);
    int16_t maxRow = -1;

    for (uint16_t row = 0; row < kHeight; ++row) {
        const size_t base = static_cast<size_t>(row) * kStride;
        for (uint16_t byte = 0; byte < kStride; ++byte) {
            if (buf_[base + byte] == other.buf_[base + byte]) {
                continue;
            }
            if (static_cast<int16_t>(byte) < minByte) {
                minByte = static_cast<int16_t>(byte);
            }
            if (static_cast<int16_t>(byte) > maxByte) {
                maxByte = static_cast<int16_t>(byte);
            }
            if (static_cast<int16_t>(row) < minRow) {
                minRow = static_cast<int16_t>(row);
            }
            if (static_cast<int16_t>(row) > maxRow) {
                maxRow = static_cast<int16_t>(row);
            }
        }
    }

    if (maxByte < 0) {
        return false;
    }

    // Byte columns are already eight pixels wide, so working in them is what
    // gives the eight-pixel alignment the controller wants.
    out.x = static_cast<int16_t>(minByte * 8);
    out.w = static_cast<int16_t>((maxByte - minByte + 1) * 8);
    out.y = minRow;
    out.h = static_cast<int16_t>(maxRow - minRow + 1);
    return true;
}

void Canvas::copyFrom(const Canvas &other)
{
    for (size_t i = 0; i < kBufferSize; ++i) {
        buf_[i] = other.buf_[i];
    }
}

uint32_t Canvas::inkCount() const
{
    uint32_t total = 0;
    for (size_t i = 0; i < kBufferSize; ++i) {
        uint8_t byte = buf_[i];
        while (byte != 0) {
            total += (byte & 1u);
            byte = static_cast<uint8_t>(byte >> 1);
        }
    }
    return total;
}

} // namespace hal
