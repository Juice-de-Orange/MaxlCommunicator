#include "ui/screens.h"

#include "ui/format.h"
#include "ui/geo.h"

namespace ui {
namespace {

constexpr int16_t kMargin = 3;
constexpr int16_t kTitleY = 2;
constexpr int16_t kRuleY = 13;
constexpr int16_t kBodyY = 18;
constexpr int16_t kLine = 11;
constexpr int16_t kFooterY = 190;

int16_t lineY(int index) { return static_cast<int16_t>(kBodyY + index * kLine); }

/// Millivolts as "4.02V". Two decimals of a volt is all the ADC is worth, and
/// rounding to centivolts first means the string is built rather than patched.
void formatVolts(char *out, size_t size, uint16_t millivolts)
{
    formatScaled(out, size, static_cast<int32_t>((millivolts + 5u) / 10u), 2);
    size_t len = 0;
    while (len + 1 < size && out[len] != '\0') {
        ++len;
    }
    if (len + 1 < size) {
        out[len] = 'V';
        out[len + 1] = '\0';
    }
}

void drawRightAligned(hal::Canvas &canvas, int16_t right, int16_t y, const char *text)
{
    canvas.text(static_cast<int16_t>(right - hal::Canvas::textWidth(text)), y, text);
}

/// Title, the battery in the corner and a rule underneath. Every screen has it,
/// so the user always knows where they are and what the battery is doing without
/// walking back to STATUS.
void drawChrome(hal::Canvas &canvas, Screen screen, const ViewModel &model)
{
    canvas.text(kMargin, kTitleY, screenTitle(screen));

    char buf[16];
    if (model.batteryMv > 0) {
        formatVolts(buf, sizeof(buf), model.batteryMv);
        drawRightAligned(canvas, hal::Canvas::kWidth - kMargin, kTitleY, buf);
    }

    canvas.hLine(0, kRuleY, hal::Canvas::kWidth);

    if (model.batteryLow) {
        // A filled block beside the voltage, so low battery is visible at a
        // glance rather than by reading a number.
        canvas.fillRect(hal::Canvas::kWidth - kMargin - 44, kTitleY - 1, 6, 9);
    }
}

/// Position in the cycle, as five marks along the bottom. Cheap, and it answers
/// "how many more presses to get back" without a menu.
void drawFooter(hal::Canvas &canvas, Screen screen)
{
    const int16_t count = static_cast<int16_t>(Screen::Count);
    const int16_t spacing = 14;
    const int16_t total = static_cast<int16_t>(count * spacing);
    const int16_t start = static_cast<int16_t>((hal::Canvas::kWidth - total) / 2);

    for (int16_t i = 0; i < count; ++i) {
        const int16_t x = static_cast<int16_t>(start + i * spacing);
        if (i == static_cast<int16_t>(screen)) {
            canvas.fillRect(x, kFooterY, 8, 4);
        } else {
            canvas.hLine(x, static_cast<int16_t>(kFooterY + 3), 8);
        }
    }
}

void drawStatus(hal::Canvas &canvas, const ViewModel &model)
{
    char buf[24];
    int line = 0;

    if (!model.timeValid) {
        /*
         * CLAUDE.md 1.2: with no valid time the device is fully transmit-blocked,
         * because the duty cycle budget is reconstructed against the RTC and
         * cannot be reconstructed without it. That is not a footnote on this
         * screen -- it is the single most important thing about the device's
         * state, so it is inverted and at the top.
         */
        canvas.fillRect(0, lineY(line) - 2, hal::Canvas::kWidth, kLine);
        canvas.text(kMargin, lineY(line), "NO TIME - TX BLOCKED", false);
        ++line;
    }

    // The budget bar. CLAUDE.md 1.3: 360 s per hour in g3, 36 s in g1.
    canvas.text(kMargin, lineY(line), "Airtime");
    if (model.budgetTotalS > 0) {
        const int16_t barX = 60;
        const int16_t barW = static_cast<int16_t>(hal::Canvas::kWidth - barX - kMargin);
        const int16_t y = static_cast<int16_t>(lineY(line) - 1);
        canvas.rect(barX, y, barW, 9);

        const uint32_t filled =
            (static_cast<uint32_t>(barW - 2) * model.budgetRemainingS) / model.budgetTotalS;
        canvas.fillRect(static_cast<int16_t>(barX + 1), static_cast<int16_t>(y + 1),
                        static_cast<int16_t>(filled), 7);
    }
    ++line;

    canvas.text(kMargin, lineY(line), "seconds");
    int16_t bx =
        canvas.text(kMargin + 60, lineY(line), formatInt(buf, sizeof(buf), model.budgetRemainingS));
    bx = canvas.text(bx, lineY(line), " of ");
    canvas.text(bx, lineY(line), formatInt(buf, sizeof(buf), model.budgetTotalS));
    ++line;

    canvas.text(kMargin, lineY(line), "Next TX");
    if (!model.timeValid) {
        canvas.text(kMargin + 60, lineY(line), "blocked");
    } else if (model.nextTxInS == 0) {
        canvas.text(kMargin + 60, lineY(line), "now");
    } else {
        canvas.text(kMargin + 60, lineY(line), formatAgeCoarse(buf, sizeof(buf), model.nextTxInS));
    }
    ++line;

    canvas.text(kMargin, lineY(line), "Radio");
    // CLAUDE.md 1.3: g3 is the working band at 869.575, g1 the 1 % fallback.
    canvas.text(kMargin + 60, lineY(line), model.band == 'P' ? "g3 869.575" : "g1 868.100");
    ++line;

    canvas.text(kMargin, lineY(line), "Unread");
    formatInt(buf, sizeof(buf), model.unreadCount);
    canvas.text(kMargin + 60, lineY(line), buf);
    ++line;

    canvas.hLine(0, static_cast<int16_t>(lineY(line) + 2), hal::Canvas::kWidth);
    ++line;

    /*
     * "Link quality per peer" is what CLAUDE.md 3.3 asks this screen for, so it
     * is here rather than only on PEERS -- one line each, enough to answer "can
     * I reach them" without walking the cycle.
     *
     * There is deliberately NO uptime on this screen. It is not in 3.3's list,
     * and it changed every minute, which cost a 325 ms partial refresh every
     * minute on a device nobody was looking at -- 60 an hour for a number the
     * spec never asked for. Uptime goes on the wire in the TELEMETRY payload
     * (2.2), which is where it is actually read.
     */
    if (model.peerCount == 0) {
        canvas.text(kMargin, lineY(line), "No peers known.");
        return;
    }

    for (size_t i = 0; i < model.peerCount && line < 14; ++i) {
        const PeerView &peer = model.peers[i];

        int16_t x = canvas.text(kMargin, lineY(line), "Node ");
        formatInt(buf, sizeof(buf), peer.nodeId);
        canvas.text(x, lineY(line), buf);

        if (!peer.everHeard) {
            canvas.text(kMargin + 56, lineY(line), "never heard");
            ++line;
            continue;
        }

        x = canvas.text(kMargin + 56, lineY(line), formatInt(buf, sizeof(buf), peer.rssi));
        x = canvas.text(x, lineY(line), "dBm SF");
        x = canvas.text(x, lineY(line), formatInt(buf, sizeof(buf), peer.sf));
        // Coarse: an age counting seconds redraws the panel every second.
        drawRightAligned(canvas, hal::Canvas::kWidth - kMargin, lineY(line),
                         formatAgeCoarse(buf, sizeof(buf), peer.lastSeenS));
        ++line;
    }
}

const char *deliveryLabel(DeliveryState state)
{
    switch (state) {
    case DeliveryState::Queued: return "queued until";
    case DeliveryState::InFlight: return "in flight";
    case DeliveryState::Delivered: return "delivered";
    case DeliveryState::Undelivered: return "UNDELIVERED";
    case DeliveryState::Received: return "received";
    }
    return "?";
}

void drawMessages(hal::Canvas &canvas, const ViewModel &model)
{
    if (model.messageCount == 0) {
        canvas.text(kMargin, lineY(1), "No messages.");
        return;
    }

    char buf[24];
    int line = 0;
    /*
     * Thirteen rather than fifteen.
     *
     * Not a live fix: ui::kMaxMessages is 5 and two lines each never reaches
     * either bound. But lineY(15) is 183, and seven pixels of glyph from there
     * reach 190 -- exactly where the footer marks begin. A list that grew into
     * them would not look broken, it would look like a different position in the
     * cycle, which is worse. The bound now matches the geometry instead of
     * happening to be safe because a constant elsewhere is small.
     */
    for (size_t i = 0; i < model.messageCount && line < 13; ++i) {
        const MessageView &message = model.messages[i];

        if (message.unread) {
            canvas.fillRect(0, static_cast<int16_t>(lineY(line) - 1), 3, 18);
        }
        canvas.text(kMargin + 5, lineY(line), message.preview);
        ++line;

        /*
         * CLAUDE.md 2.4 wants the three waiting states told apart, and gate 4.4
         * checks it. "queued until" without the time is the same word as
         * "queued", so the minutes are part of the label, not an extra.
         */
        int16_t x = canvas.text(kMargin + 12, lineY(line), deliveryLabel(message.state));
        if (message.state == DeliveryState::Queued) {
            x = canvas.text(x, lineY(line), " ");
            formatInt(buf, sizeof(buf), message.releaseInMinutes);
            x = canvas.text(x, lineY(line), buf);
            canvas.text(x, lineY(line), "m");
        } else if (message.state == DeliveryState::Received) {
            // Who it came from, not how old it is: a journal entry carries no
            // timestamp, so an age here would be invented (see app/view.cpp).
            x = canvas.text(static_cast<int16_t>(x + 6), lineY(line), "from ");
            formatInt(buf, sizeof(buf), message.peerId);
            canvas.text(x, lineY(line), buf);
        } else {
            // Coarse: a message under a minute old would otherwise redraw this
            // screen once a second for its first minute.
            canvas.text(static_cast<int16_t>(x + 6), lineY(line),
                        formatAgeCoarse(buf, sizeof(buf), message.ageS));
        }

        if (message.state == DeliveryState::Undelivered) {
            // Boxed. It is the one state that needs the user to do something.
            canvas.rect(kMargin + 9, static_cast<int16_t>(lineY(line) - 2),
                        static_cast<int16_t>(hal::Canvas::textWidth("UNDELIVERED") + 6), 12);
        }
        ++line;
    }
}

void drawTelemetry(hal::Canvas &canvas, const ViewModel &model)
{
    char buf[24];
    int line = 0;

    canvas.text(kMargin, lineY(line), "Here");
    ++line;

    if (!model.haveLocalSensor) {
        canvas.text(kMargin + 8, lineY(line), "sensor not answering");
        ++line;
    } else {
        canvas.text(kMargin + 8, lineY(line), "temp");
        formatScaled(buf, sizeof(buf), model.localTempCentiC, 2);
        canvas.text(kMargin + 70, lineY(line), buf);
        canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " C");
        ++line;

        // A BMP280 has no humidity sensor. Saying so beats printing a zero that
        // looks like a measurement (see hal/i_sensor.h).
        canvas.text(kMargin + 8, lineY(line), "hum");
        if (model.localHumidityValid) {
            formatScaled(buf, sizeof(buf), model.localHumidityCentiPct, 2);
            canvas.text(kMargin + 70, lineY(line), buf);
            canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " %");
        } else {
            canvas.text(kMargin + 70, lineY(line), "not fitted");
        }
        ++line;

        canvas.text(kMargin + 8, lineY(line), "press");
        formatScaled(buf, sizeof(buf), static_cast<int32_t>(model.localPressurePa / 10u), 1);
        canvas.text(kMargin + 70, lineY(line), buf);
        canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " hPa");
        ++line;
    }
    ++line;

    if (!model.havePeerTelemetry) {
        canvas.text(kMargin, lineY(line), "No peer telemetry yet.");
        return;
    }

    int16_t x = canvas.text(kMargin, lineY(line), "Node ");
    formatInt(buf, sizeof(buf), model.peerTelemetryNodeId);
    x = canvas.text(x, lineY(line), buf);
    x = canvas.text(x, lineY(line), "  ");
    canvas.text(x, lineY(line), formatAgeCoarse(buf, sizeof(buf), model.peerTelemetryAgeS));
    ++line;

    canvas.text(kMargin + 8, lineY(line), "temp");
    formatScaled(buf, sizeof(buf), model.peerTempCentiC, 2);
    canvas.text(kMargin + 70, lineY(line), buf);
    canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " C");
    ++line;

    canvas.text(kMargin + 8, lineY(line), "hum");
    formatScaled(buf, sizeof(buf), model.peerHumidityCentiPct, 2);
    canvas.text(kMargin + 70, lineY(line), buf);
    canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " %");
    ++line;

    canvas.text(kMargin + 8, lineY(line), "press");
    formatScaled(buf, sizeof(buf), static_cast<int32_t>(model.peerPressurePa / 10u), 1);
    canvas.text(kMargin + 70, lineY(line), buf);
    canvas.text(kMargin + 70 + hal::Canvas::textWidth(buf), lineY(line), " hPa");
    ++line;

    canvas.text(kMargin + 8, lineY(line), "batt");
    formatVolts(buf, sizeof(buf), model.peerBatteryMv);
    canvas.text(kMargin + 70, lineY(line), buf);
}

void drawPeers(hal::Canvas &canvas, const ViewModel &model)
{
    if (model.peerCount == 0) {
        canvas.text(kMargin, lineY(1), "No peers known.");
        return;
    }

    char buf[24];
    int line = 0;
    for (size_t i = 0; i < model.peerCount && line < 14; ++i) {
        const PeerView &peer = model.peers[i];

        int16_t x = canvas.text(kMargin, lineY(line), "Node ");
        formatInt(buf, sizeof(buf), peer.nodeId);
        x = canvas.text(x, lineY(line), buf);

        if (!peer.everHeard) {
            canvas.text(static_cast<int16_t>(x + 12), lineY(line), "never heard");
            line += 2;
            continue;
        }

        // Coarse for the same reason as on STATUS: seconds here would redraw the
        // panel every second for as long as this screen is up.
        drawRightAligned(canvas, hal::Canvas::kWidth - kMargin, lineY(line),
                         formatAgeCoarse(buf, sizeof(buf), peer.lastSeenS));
        ++line;

        x = canvas.text(kMargin + 8, lineY(line), "RSSI ");
        formatInt(buf, sizeof(buf), peer.rssi);
        x = canvas.text(x, lineY(line), buf);
        x = canvas.text(x, lineY(line), "  SNR ");
        formatInt(buf, sizeof(buf), peer.snr);
        x = canvas.text(x, lineY(line), buf);
        x = canvas.text(x, lineY(line), "  SF");
        formatInt(buf, sizeof(buf), peer.sf);
        canvas.text(x, lineY(line), buf);
        ++line;
    }
}

/// An arrow of length `radius` from (cx, cy), pointing at `bearing` degrees
/// clockwise from up. Drawn from integer lookup rather than trigonometry: the
/// eight-point resolution is all a 200x200 panel can show anyway, and it keeps
/// the maths out of the drawing code.
void drawArrow(hal::Canvas &canvas, int16_t cx, int16_t cy, int16_t radius, uint16_t bearing)
{
    // sin and cos of 0, 45, 90 ... in sixteenths.
    static const int8_t kSin[8] = {0, 11, 16, 11, 0, -11, -16, -11};
    static const int8_t kCos[8] = {16, 11, 0, -11, -16, -11, 0, 11};

    const uint8_t index = static_cast<uint8_t>((((bearing % 360u) + 22u) / 45u) % 8u);
    const int16_t dx = static_cast<int16_t>(radius * kSin[index] / 16);
    const int16_t dy = static_cast<int16_t>(-radius * kCos[index] / 16);

    const int16_t tipX = static_cast<int16_t>(cx + dx);
    const int16_t tipY = static_cast<int16_t>(cy + dy);

    // Shaft, drawn as a run of dots along the vector.
    for (int16_t step = 0; step <= radius; ++step) {
        canvas.setPixel(static_cast<int16_t>(cx + dx * step / radius),
                        static_cast<int16_t>(cy + dy * step / radius), true);
    }

    // Head: a small filled square at the tip. A proper triangle would need the
    // perpendicular, and at this size it would be four pixels either way.
    canvas.fillRect(static_cast<int16_t>(tipX - 2), static_cast<int16_t>(tipY - 2), 5, 5);
    canvas.rect(static_cast<int16_t>(cx - 2), static_cast<int16_t>(cy - 2), 5, 5);
}

void drawPosition(hal::Canvas &canvas, const ViewModel &model)
{
    char buf[24];
    int line = 0;

    if (!model.haveFix) {
        if (model.gnssPowered) {
            canvas.text(kMargin, lineY(line), "Searching...");
            ++line;
            int16_t x = canvas.text(kMargin, lineY(line), "sats ");
            formatInt(buf, sizeof(buf), model.satellites);
            canvas.text(x, lineY(line), buf);
        } else if (model.gnssTimedOut) {
            canvas.text(kMargin, lineY(line), "No fix - timed out.");
            ++line;
            canvas.text(kMargin, lineY(line), "Hold touch to retry.");
        } else {
            canvas.text(kMargin, lineY(line), "GNSS is off.");
            ++line;
            // CLAUDE.md 1.5: off is the resting state and turning it on costs
            // tens of milliamps. The screen says how, and says nothing about it
            // happening on its own, because it does not.
            canvas.text(kMargin, lineY(line), "Hold touch for a fix.");
        }
        return;
    }

    canvas.text(kMargin, lineY(line), formatCoordinate(buf, sizeof(buf), model.latE7, true));
    ++line;
    canvas.text(kMargin, lineY(line), formatCoordinate(buf, sizeof(buf), model.lonE7, false));
    ++line;

    int16_t x = canvas.text(kMargin, lineY(line), "alt ");
    formatInt(buf, sizeof(buf), model.altM);
    x = canvas.text(x, lineY(line), buf);
    x = canvas.text(x, lineY(line), "m  hdop ");
    // Decision D11: the byte is tenths.
    formatScaled(buf, sizeof(buf), model.hdopTenths, 1);
    canvas.text(x, lineY(line), buf);
    ++line;

    /*
     * The one age still counted in seconds, and deliberately.
     *
     * How old a fix is decides whether it is worth reading, and this screen is
     * only ever in front of somebody who is asking that. It does redraw once a
     * second while a fix is fresh -- which is a real cost, and it is the cheapest
     * thing happening: the GNSS itself draws tens of milliamps (CLAUDE.md 1.5)
     * against a 325 ms partial refresh. Once the module is off, the fix stops
     * ageing here because fixTakenAtUnix stops moving.
     */
    x = canvas.text(kMargin, lineY(line), "fix ");
    x = canvas.text(x, lineY(line), formatAge(buf, sizeof(buf), model.fixAgeS));
    x = canvas.text(x, lineY(line), " ago  sats ");
    formatInt(buf, sizeof(buf), model.satellites);
    canvas.text(x, lineY(line), buf);
    ++line;

    canvas.hLine(0, static_cast<int16_t>(lineY(line) + 2), hal::Canvas::kWidth);
    ++line;

    bool drewAny = false;
    for (size_t i = 0; i < model.peerCount && line < 13; ++i) {
        const PeerView &peer = model.peers[i];
        if (!peer.hasPosition) {
            continue;
        }
        drewAny = true;

        const uint32_t metres = distanceMetres(model.latE7, model.lonE7, peer.latE7, peer.lonE7);
        const uint16_t bearing = bearingDegrees(model.latE7, model.lonE7, peer.latE7, peer.lonE7);

        x = canvas.text(kMargin, lineY(line), "Node ");
        formatInt(buf, sizeof(buf), peer.nodeId);
        canvas.text(x, lineY(line), buf);

        x = canvas.text(kMargin + 62, lineY(line), formatDistance(buf, sizeof(buf), metres));
        x = canvas.text(static_cast<int16_t>(x + 8), lineY(line), compassPoint(bearing));
        x = canvas.text(x, lineY(line), " ");
        formatInt(buf, sizeof(buf), bearing);
        canvas.text(x, lineY(line), buf);

        drawArrow(canvas, static_cast<int16_t>(hal::Canvas::kWidth - 18),
                  static_cast<int16_t>(lineY(line) + 3), 12, bearing);
        line += 2;
    }

    if (!drewAny) {
        canvas.text(kMargin, lineY(line), "No peer position yet.");
    }
}

} // namespace

Screen nextScreen(Screen current)
{
    const uint8_t next = static_cast<uint8_t>(
        (static_cast<uint8_t>(current) + 1) % static_cast<uint8_t>(Screen::Count));
    return static_cast<Screen>(next);
}

const char *screenTitle(Screen screen)
{
    switch (screen) {
    case Screen::Status: return "STATUS";
    case Screen::Messages: return "MESSAGES";
    case Screen::Telemetry: return "TELEMETRY";
    case Screen::Peers: return "PEERS";
    case Screen::Position: return "POSITION";
    case Screen::Count: break;
    }
    return "?";
}

void renderScreen(Screen screen, const ViewModel &model, hal::Canvas &canvas)
{
    canvas.clear();
    drawChrome(canvas, screen, model);

    switch (screen) {
    case Screen::Status: drawStatus(canvas, model); break;
    case Screen::Messages: drawMessages(canvas, model); break;
    case Screen::Telemetry: drawTelemetry(canvas, model); break;
    case Screen::Peers: drawPeers(canvas, model); break;
    case Screen::Position: drawPosition(canvas, model); break;
    case Screen::Count: break;
    }

    drawFooter(canvas, screen);
}

PowerMenuItem nextMenuItem(PowerMenuItem current)
{
    const uint8_t next = static_cast<uint8_t>(
        (static_cast<uint8_t>(current) + 1) % static_cast<uint8_t>(PowerMenuItem::Count));
    return static_cast<PowerMenuItem>(next);
}

const char *powerMenuLabel(PowerMenuItem item)
{
    switch (item) {
    case PowerMenuItem::Cancel: return "Cancel";
    case PowerMenuItem::Sleep: return "Sleep";
    case PowerMenuItem::Shutdown: return "Shut down";
    case PowerMenuItem::Count: break;
    }
    return "?";
}

void renderPowerMenu(hal::Canvas &canvas, PowerMenuItem selected)
{
    constexpr int16_t kBoxX = 16;
    constexpr int16_t kBoxW = hal::Canvas::kWidth - 2 * kBoxX;
    constexpr int16_t kRow = 20;
    const int16_t boxH = static_cast<int16_t>(30 + static_cast<int16_t>(PowerMenuItem::Count) * kRow);
    const int16_t boxY = static_cast<int16_t>((hal::Canvas::kHeight - boxH) / 2);

    // Cleared to white first: this is modal, and the screen underneath showing
    // through would make it look like part of that screen.
    canvas.fillRect(kBoxX, boxY, kBoxW, boxH, false);
    canvas.rect(kBoxX, boxY, kBoxW, boxH);
    canvas.rect(static_cast<int16_t>(kBoxX + 1), static_cast<int16_t>(boxY + 1),
                static_cast<int16_t>(kBoxW - 2), static_cast<int16_t>(boxH - 2));

    canvas.text(static_cast<int16_t>(kBoxX + 10), static_cast<int16_t>(boxY + 8), "POWER");
    canvas.hLine(static_cast<int16_t>(kBoxX + 4), static_cast<int16_t>(boxY + 20),
                 static_cast<int16_t>(kBoxW - 8));

    for (uint8_t i = 0; i < static_cast<uint8_t>(PowerMenuItem::Count); ++i) {
        const int16_t y = static_cast<int16_t>(boxY + 26 + i * kRow);
        const bool isSelected = i == static_cast<uint8_t>(selected);

        if (isSelected) {
            // Inverted rather than a caret: at this size a caret is four pixels
            // and reads as dirt on the glass.
            canvas.fillRect(static_cast<int16_t>(kBoxX + 4), static_cast<int16_t>(y - 3),
                            static_cast<int16_t>(kBoxW - 8), 15);
        }
        canvas.text(static_cast<int16_t>(kBoxX + 12), y,
                    powerMenuLabel(static_cast<PowerMenuItem>(i)), !isSelected);
    }

    canvas.text(static_cast<int16_t>(kBoxX + 6),
                static_cast<int16_t>(boxY + boxH - 12), "touch=next  hold=pick");
}

void renderLongPressFeedback(hal::Canvas &canvas, uint32_t heldMs, uint32_t thresholdMs,
                             const char *label)
{
    if (thresholdMs == 0) {
        return;
    }

    constexpr int16_t kBoxH = 26;
    const int16_t y = static_cast<int16_t>(hal::Canvas::kHeight - kBoxH - 14);

    canvas.fillRect(0, y, hal::Canvas::kWidth, kBoxH, false);
    canvas.rect(0, y, hal::Canvas::kWidth, kBoxH);

    if (label != nullptr) {
        canvas.text(kMargin + 3, static_cast<int16_t>(y + 3), label);
    }

    const int16_t barY = static_cast<int16_t>(y + 14);
    const int16_t barW = static_cast<int16_t>(hal::Canvas::kWidth - 2 * (kMargin + 3));
    canvas.rect(static_cast<int16_t>(kMargin + 3), barY, barW, 8);

    const uint32_t held = heldMs > thresholdMs ? thresholdMs : heldMs;
    const int16_t filled = static_cast<int16_t>((static_cast<uint32_t>(barW - 2) * held) / thresholdMs);
    canvas.fillRect(static_cast<int16_t>(kMargin + 4), static_cast<int16_t>(barY + 1), filled, 6);
}

} // namespace ui
