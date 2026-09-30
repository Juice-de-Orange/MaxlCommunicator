/*
 * The five screens, rendered into a canvas and inspected.
 *
 * This is what the layer policy bought. ui/ may only see hal/, so a screen is a
 * function from a plain struct to a byte array -- no radio, no flash, no clock,
 * nothing to stand up. Two of the phase 4 gates become properties that can be
 * checked here rather than watched for an hour:
 *
 *   4.1  the same model renders the same pixels, every time
 *   4.4  the three waiting states are visibly different from each other
 *
 * 4.2 and 4.3 are not here and cannot be. Whether a screen is *correct* is a
 * question about what a person sees, and it needs the panel and a person.
 */

#include <cstring>

#include "doctest.h"

#include "hal/canvas.h"
#include "hal/refresh_policy.h"
#include "ui/screens.h"

using namespace ui;

namespace {

ViewModel populatedModel()
{
    ViewModel model;
    model.batteryMv = 4021;
    model.timeValid = true;
    model.uptimeS = 7200;
    model.unreadCount = 2;
    model.budgetRemainingS = 220;
    model.budgetTotalS = 360;
    model.nextTxInS = 0;
    model.band = 'P';
    model.currentSf = 9;

    model.haveLocalSensor = true;
    model.localHumidityValid = true;
    model.localTempCentiC = 2134;
    model.localHumidityCentiPct = 4820;
    model.localPressurePa = 98123;

    model.havePeerTelemetry = true;
    model.peerTelemetryNodeId = 2;
    model.peerTempCentiC = 1980;
    model.peerHumidityCentiPct = 5510;
    model.peerPressurePa = 97880;
    model.peerBatteryMv = 3890;
    model.peerTelemetryAgeS = 300;

    model.haveFix = true;
    model.latE7 = 481173000;
    model.lonE7 = 115166667;
    model.altM = 545;
    model.fixAgeS = 12;
    model.hdopTenths = 9;
    model.satellites = 8;

    model.peerCount = 1;
    model.peers[0].nodeId = 2;
    model.peers[0].everHeard = true;
    model.peers[0].rssi = -97;
    model.peers[0].snr = 7;
    model.peers[0].sf = 9;
    model.peers[0].lastSeenS = 45;
    model.peers[0].hasPosition = true;
    model.peers[0].latE7 = 482082000;
    model.peers[0].lonE7 = 163738000;

    model.messageCount = 3;
    std::strcpy(model.messages[0].preview, "on my way");
    model.messages[0].state = DeliveryState::Queued;
    model.messages[0].releaseInMinutes = 4;
    model.messages[1].state = DeliveryState::InFlight;
    std::strcpy(model.messages[1].preview, "still here");
    model.messages[2].state = DeliveryState::Undelivered;
    std::strcpy(model.messages[2].preview, "no answer");
    return model;
}

void forEachScreen(void (*body)(Screen))
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(Screen::Count); ++i) {
        body(static_cast<Screen>(i));
    }
}

} // namespace

TEST_CASE("every screen draws something and none of them is blank")
{
    const ViewModel model = populatedModel();
    for (uint8_t i = 0; i < static_cast<uint8_t>(Screen::Count); ++i) {
        const Screen screen = static_cast<Screen>(i);
        CAPTURE(screenTitle(screen));

        hal::Canvas canvas;
        renderScreen(screen, model, canvas);
        CHECK(canvas.inkCount() > 200);
    }
}

TEST_CASE("an empty device renders every screen without drawing off the edge")
{
    // The state a device is in for the first minutes of its life: no peers, no
    // messages, no fix, no clock. Every screen must still be readable.
    ViewModel model;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Screen::Count); ++i) {
        const Screen screen = static_cast<Screen>(i);
        CAPTURE(screenTitle(screen));

        hal::Canvas canvas;
        renderScreen(screen, model, canvas);
        CHECK(canvas.inkCount() > 0);
    }
}

TEST_CASE("the same model renders the same pixels -- gate 4.1's premise")
{
    const ViewModel model = populatedModel();

    for (uint8_t i = 0; i < static_cast<uint8_t>(Screen::Count); ++i) {
        const Screen screen = static_cast<Screen>(i);
        CAPTURE(screenTitle(screen));

        hal::Canvas first;
        hal::Canvas second;
        renderScreen(screen, model, first);
        renderScreen(screen, model, second);
        CHECK(first.sameAs(second));
    }
}

TEST_CASE("an idle hour produces no refresh at all -- gate 4.1")
{
    const ViewModel model = populatedModel();
    hal::RefreshPolicy policy;
    hal::Canvas shown;
    hal::Canvas next;

    renderScreen(Screen::Status, model, shown);
    REQUIRE(policy.decide(true, false) == hal::RefreshKind::Full);

    /*
     * 3600 passes of the loop with nothing changing. The model carries an uptime
     * and a fix age, and those are exactly the fields that tempt a screen into
     * redrawing on their own -- which is the failure gate 4.1's note describes.
     * They do not change here because app/ only recomputes them when something
     * else did.
     */
    for (int second = 0; second < 3600; ++second) {
        renderScreen(Screen::Status, model, next);
        REQUIRE(policy.decide(!next.sameAs(shown), false) == hal::RefreshKind::None);
    }

    CHECK(policy.partialCount() == 0);
    CHECK(policy.fullCount() == 1);
}

TEST_CASE("a changed value does reach the panel")
{
    ViewModel model = populatedModel();
    hal::Canvas before;
    hal::Canvas after;

    renderScreen(Screen::Status, model, before);
    model.budgetRemainingS = 40;
    renderScreen(Screen::Status, model, after);

    CHECK_FALSE(before.sameAs(after));
}

TEST_CASE("the three waiting states look different from one another -- gate 4.4")
{
    ViewModel queued = populatedModel();
    queued.messageCount = 1;
    queued.messages[0].state = DeliveryState::Queued;

    ViewModel inFlight = queued;
    inFlight.messages[0].state = DeliveryState::InFlight;

    ViewModel undelivered = queued;
    undelivered.messages[0].state = DeliveryState::Undelivered;

    hal::Canvas a;
    hal::Canvas b;
    hal::Canvas c;
    renderScreen(Screen::Messages, queued, a);
    renderScreen(Screen::Messages, inFlight, b);
    renderScreen(Screen::Messages, undelivered, c);

    CHECK_FALSE(a.sameAs(b));
    CHECK_FALSE(b.sameAs(c));
    CHECK_FALSE(a.sameAs(c));
}

TEST_CASE("a queued message shows the wait, not merely the word")
{
    ViewModel four = populatedModel();
    four.messageCount = 1;
    four.messages[0].state = DeliveryState::Queued;
    four.messages[0].releaseInMinutes = 4;

    ViewModel forty = four;
    forty.messages[0].releaseInMinutes = 40;

    hal::Canvas a;
    hal::Canvas b;
    renderScreen(Screen::Messages, four, a);
    renderScreen(Screen::Messages, forty, b);
    CHECK_FALSE(a.sameAs(b));
}

TEST_CASE("no valid time is announced, because it means the radio is blocked")
{
    ViewModel valid = populatedModel();
    ViewModel invalid = valid;
    invalid.timeValid = false;

    hal::Canvas a;
    hal::Canvas b;
    renderScreen(Screen::Status, valid, a);
    renderScreen(Screen::Status, invalid, b);

    CHECK_FALSE(a.sameAs(b));
    // The banner is inverted, so it adds a great deal of ink.
    CHECK(b.inkCount() > a.inkCount() + 500);
}

TEST_CASE("a missing humidity sensor says so rather than printing zero")
{
    ViewModel fitted = populatedModel();
    ViewModel notFitted = fitted;
    notFitted.localHumidityValid = false;
    notFitted.localHumidityCentiPct = 0;

    ViewModel zeroButFitted = fitted;
    zeroButFitted.localHumidityCentiPct = 0;

    hal::Canvas a;
    hal::Canvas b;
    renderScreen(Screen::Telemetry, notFitted, a);
    renderScreen(Screen::Telemetry, zeroButFitted, b);
    CHECK_FALSE(a.sameAs(b));
}

TEST_CASE("the three GNSS states are told apart on POSITION")
{
    ViewModel off;
    ViewModel searching;
    searching.gnssPowered = true;
    ViewModel timedOut;
    timedOut.gnssTimedOut = true;

    hal::Canvas a;
    hal::Canvas b;
    hal::Canvas c;
    renderScreen(Screen::Position, off, a);
    renderScreen(Screen::Position, searching, b);
    renderScreen(Screen::Position, timedOut, c);

    CHECK_FALSE(a.sameAs(b));
    CHECK_FALSE(b.sameAs(c));
    CHECK_FALSE(a.sameAs(c));
}

TEST_CASE("a peer that moves changes the arrow")
{
    ViewModel east = populatedModel();
    ViewModel north = east;
    north.peers[0].latE7 = 490000000;
    north.peers[0].lonE7 = east.lonE7;

    hal::Canvas a;
    hal::Canvas b;
    renderScreen(Screen::Position, east, a);
    renderScreen(Screen::Position, north, b);
    CHECK_FALSE(a.sameAs(b));
}

TEST_CASE("the screen cycle visits all five and comes back")
{
    Screen screen = Screen::Status;
    for (int i = 0; i < static_cast<int>(Screen::Count); ++i) {
        screen = nextScreen(screen);
    }
    CHECK(screen == Screen::Status);

    CHECK(nextScreen(Screen::Status) == Screen::Messages);
    CHECK(nextScreen(Screen::Messages) == Screen::Telemetry);
    CHECK(nextScreen(Screen::Telemetry) == Screen::Peers);
    CHECK(nextScreen(Screen::Peers) == Screen::Position);
    CHECK(nextScreen(Screen::Position) == Screen::Status);
}

TEST_CASE("every screen has a title and none of them is the fallback")
{
    forEachScreen([](Screen screen) {
        const char *title = screenTitle(screen);
        REQUIRE(title != nullptr);
        CHECK(std::strlen(title) > 2);
        CHECK(std::strcmp(title, "?") != 0);
    });
}

TEST_CASE("long-press feedback grows, and is complete before the action fires")
{
    hal::Canvas empty;
    hal::Canvas quarter;
    hal::Canvas full;
    hal::Canvas past;

    renderLongPressFeedback(quarter, 250, 1000, "GNSS fix");
    renderLongPressFeedback(full, 1000, 1000, "GNSS fix");
    renderLongPressFeedback(past, 5000, 1000, "GNSS fix");

    CHECK(quarter.inkCount() > empty.inkCount());
    CHECK(full.inkCount() > quarter.inkCount());

    // Held past the threshold the bar cannot grow any further -- the action is
    // already armed and the feedback is already complete.
    CHECK(past.sameAs(full));
}

TEST_CASE("the feedback overlay does not erase the screen underneath it")
{
    const ViewModel model = populatedModel();
    hal::Canvas canvas;
    renderScreen(Screen::Position, model, canvas);
    const uint32_t before = canvas.inkCount();

    renderLongPressFeedback(canvas, 500, 1000, "GNSS fix");

    // The title is above the overlay and must survive it.
    CHECK(canvas.inkCount() > 0);
    hal::Canvas titleOnly;
    renderScreen(Screen::Position, model, titleOnly);
    bool topRowsIntact = true;
    for (int16_t y = 0; y < 14 && topRowsIntact; ++y) {
        for (int16_t x = 0; x < hal::Canvas::kWidth; ++x) {
            if (canvas.pixel(x, y) != titleOnly.pixel(x, y)) {
                topRowsIntact = false;
                break;
            }
        }
    }
    CHECK(topRowsIntact);
    CHECK(before > 0);
}

TEST_CASE("nothing on STATUS moves on its own -- gate 4.1")
{
    ViewModel model = populatedModel();
    hal::Canvas first;
    hal::Canvas second;

    /*
     * The measurements that made this test exist, both on node A with sketch 12:
     * 7 partial refreshes in the first 10 seconds (the uptime advancing by a
     * second), and then one a minute once that was coarsened. CLAUDE.md 3.3 does
     * not ask STATUS for an uptime at all, so it no longer has one.
     *
     * Two hours of the uptime advancing must produce one identical image.
     *
     * The peer's last-contact age is held still here on purpose. That one DOES
     * change the screen when it crosses a coarse boundary, and it should: a peer
     * that has gone quiet is exactly what this screen is for. That is the next
     * test, not a violation of this one.
     */
    model.uptimeS = 3600;
    model.peers[0].lastSeenS = 45;
    renderScreen(Screen::Status, model, first);

    for (uint32_t extra = 1; extra < 7200; extra += 7) {
        model.uptimeS = 3600 + extra;
        renderScreen(Screen::Status, model, second);
        CAPTURE(extra);
        REQUIRE(first.sameAs(second));
    }
}

TEST_CASE("a device with nothing to say redraws nothing at all -- gate 4.1")
{
    /*
     * The literal gate: an hour of loop passes with the model frozen, through
     * the same policy the device runs.
     *
     * Every screen, not only STATUS -- POSITION is the one that could hide a
     * per-second value, and it is checked here with the GNSS off, which is the
     * state CLAUDE.md 1.5 says it spends nearly all its life in.
     */
    const ViewModel model = populatedModel();

    for (uint8_t i = 0; i < static_cast<uint8_t>(Screen::Count); ++i) {
        const Screen screen = static_cast<Screen>(i);
        CAPTURE(screenTitle(screen));

        hal::RefreshPolicy policy;
        hal::Canvas shown;
        hal::Canvas next;

        renderScreen(screen, model, shown);
        REQUIRE(policy.decide(true, false) == hal::RefreshKind::Full);

        // One pass a second for an hour.
        for (int second = 0; second < 3600; ++second) {
            renderScreen(screen, model, next);
            REQUIRE(policy.decide(!next.sameAs(shown), false) == hal::RefreshKind::None);
        }

        CHECK(policy.partialCount() == 0);
        CHECK(policy.fullCount() == 1);
    }
}

TEST_CASE("but a peer that has just gone quiet does show on STATUS")
{
    // Coarsening is not the same as ignoring: crossing from under a minute to
    // minutes, and from minutes to hours, is a change worth a refresh.
    ViewModel model = populatedModel();
    hal::Canvas under;
    hal::Canvas minutes;
    hal::Canvas hours;

    model.peers[0].lastSeenS = 30;
    renderScreen(Screen::Status, model, under);
    model.peers[0].lastSeenS = 300;
    renderScreen(Screen::Status, model, minutes);
    model.peers[0].lastSeenS = 7200;
    renderScreen(Screen::Status, model, hours);

    CHECK_FALSE(under.sameAs(minutes));
    CHECK_FALSE(minutes.sameAs(hours));
}

TEST_CASE("no screen draws into the footer's row, however full it is")
{
    /*
     * The footer marks live at y = 190..193 and are how the user knows where in
     * the cycle they are. A list that grew into them would not look broken -- it
     * would look like a different position in the cycle, which is worse.
     *
     * The model here is as full as the view model can be: every peer slot used,
     * every message slot used, the longest previews that fit.
     */
    ViewModel model = populatedModel();

    model.peerCount = kMaxPeers;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        model.peers[i].nodeId = static_cast<uint16_t>(100 + i);
        model.peers[i].everHeard = true;
        model.peers[i].rssi = -128;
        model.peers[i].snr = -20;
        model.peers[i].sf = 12;
        model.peers[i].lastSeenS = 99999;
        model.peers[i].hasPosition = true;
        model.peers[i].latE7 = 482082000;
        model.peers[i].lonE7 = 163738000;
    }

    model.messageCount = kMaxMessages;
    for (size_t i = 0; i < kMaxMessages; ++i) {
        for (size_t c = 0; c + 1 < kMessagePreviewChars; ++c) {
            model.messages[i].preview[c] = 'W';
        }
        model.messages[i].preview[kMessagePreviewChars - 1] = '\0';
        model.messages[i].state = DeliveryState::Undelivered;
        model.messages[i].peerId = 65535;
        model.messages[i].ageS = 99999;
    }

    for (uint8_t s = 0; s < static_cast<uint8_t>(Screen::Count); ++s) {
        const Screen screen = static_cast<Screen>(s);
        CAPTURE(screenTitle(screen));

        hal::Canvas canvas;
        renderScreen(screen, model, canvas);

        // The gap between the content area and the footer marks.
        for (int16_t y = 185; y < 190; ++y) {
            for (int16_t x = 0; x < hal::Canvas::kWidth; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                REQUIRE_FALSE(canvas.pixel(x, y));
            }
        }
    }
}

TEST_CASE("the footer marks are actually there, one filled and four not")
{
    const ViewModel model = populatedModel();
    for (uint8_t s = 0; s < static_cast<uint8_t>(Screen::Count); ++s) {
        hal::Canvas canvas;
        renderScreen(static_cast<Screen>(s), model, canvas);

        uint32_t footerInk = 0;
        for (int16_t y = 190; y < 195; ++y) {
            for (int16_t x = 0; x < hal::Canvas::kWidth; ++x) {
                if (canvas.pixel(x, y)) {
                    ++footerInk;
                }
            }
        }
        CAPTURE(s);
        // One filled 8x4 block plus four 8-pixel rules.
        CHECK(footerInk == 8u * 4u + 4u * 8u);
    }
}
