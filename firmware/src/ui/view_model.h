/*
 * Everything the screens are allowed to know.
 *
 * This exists because of the layer policy in firmware/layering.toml: ui/ may
 * include hal/ and nothing else. It may not reach into app/ for a PeerTable or a
 * message queue. That is not an inconvenience to work around -- it is what makes
 * every screen renderable from a struct literal in a host test, with no radio,
 * no flash and no clock anywhere near it.
 *
 * So app/ fills this in and hands it over. Two consequences worth stating,
 * because they shape the fields below:
 *
 *   - Ages are seconds, already computed. A screen never sees a clock and can
 *     therefore never render a different picture from the same state, which is
 *     precisely the property docs/test-plan.md gate 4.1 depends on: "zero
 *     partial refreshes in an hour with no state change". A screen that read
 *     millis() would change every millisecond and refresh for ever.
 *   - Everything is fixed size. No pointers into app/'s storage, no lifetimes to
 *     reason about, and the whole model is copyable, which is how the controller
 *     can hold the last one it drew.
 */

#ifndef MAXL_UI_VIEW_MODEL_H
#define MAXL_UI_VIEW_MODEL_H

#include <stddef.h>
#include <stdint.h>

namespace ui {

inline constexpr size_t kMaxPeers = 8;      ///< matches app::kMaxPeers
inline constexpr size_t kMaxMessages = 5;   ///< as many as the MESSAGES screen shows
inline constexpr size_t kMessagePreviewChars = 26;

/// CLAUDE.md 2.4: "The UI distinguishes three states, because they mean
/// different things to the user." Delivered is the fourth, and it is the only
/// one that is good news.
enum class DeliveryState : uint8_t {
    Queued = 0,   ///< blocked by the duty cycle budget, with a release time
    InFlight,     ///< transmitted, waiting for an ACK, possibly retrying
    Delivered,
    Undelivered,  ///< three attempts, gave up. Never silently dropped.
    Received,     ///< inbound
};

struct PeerView {
    uint16_t nodeId = 0;
    int16_t rssi = 0;
    int8_t snr = 0;
    uint8_t sf = 0;
    uint32_t lastSeenS = 0;   ///< seconds since last heard
    bool everHeard = false;
    bool hasPosition = false;
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
};

struct MessageView {
    char preview[kMessagePreviewChars] = {0}; ///< NUL-terminated, already truncated
    uint16_t peerId = 0;
    uint32_t ageS = 0;
    DeliveryState state = DeliveryState::Received;

    /// Minutes until the budget releases this frame. Only meaningful for Queued,
    /// and it is what turns "queued" into "queued until HH:MM" on the screen.
    uint16_t releaseInMinutes = 0;
    bool unread = false;

    /// D18: an undelivered message reduced to its head. The text is gone; the
    /// preview says so and the failure itself stays on the screen.
    bool truncated = false;
};

struct ViewModel {
    // --- STATUS -----------------------------------------------------------
    uint16_t batteryMv = 0;
    bool batteryLow = false;
    bool timeValid = false;     ///< false means transmit-blocked (CLAUDE.md 1.2)
    uint32_t uptimeS = 0;
    uint8_t unreadCount = 0;

    /// Duty cycle budget, in seconds of airtime. `budgetTotalS` is 360 in g3 and
    /// 36 in g1 (CLAUDE.md 1.3), so the ratio is what the bar shows.
    uint16_t budgetRemainingS = 0;
    uint16_t budgetTotalS = 0;

    /// Seconds until the next legal transmission, 0 if one may go now. This is
    /// the later of the hourly total and the per-frame lockout (CLAUDE.md 1.2).
    uint16_t nextTxInS = 0;

    /// 'P' for the primary sub-band g3, 'M' for the g1 fallback.
    char band = 'P';
    uint8_t currentSf = 9;

    // --- TELEMETRY --------------------------------------------------------
    bool haveLocalSensor = false;
    bool localHumidityValid = false;
    int16_t localTempCentiC = 0;
    uint16_t localHumidityCentiPct = 0;
    uint32_t localPressurePa = 0;

    bool havePeerTelemetry = false;
    uint16_t peerTelemetryNodeId = 0;
    int16_t peerTempCentiC = 0;
    uint16_t peerHumidityCentiPct = 0;
    uint32_t peerPressurePa = 0;
    uint16_t peerBatteryMv = 0;
    uint32_t peerTelemetryAgeS = 0;

    // --- POSITION ---------------------------------------------------------
    bool haveFix = false;
    bool gnssPowered = false;
    bool gnssTimedOut = false;
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    int16_t altM = 0;
    uint32_t fixAgeS = 0;
    uint8_t hdopTenths = 0;   ///< decision D11
    uint8_t satellites = 0;

    // --- lists ------------------------------------------------------------
    PeerView peers[kMaxPeers];
    size_t peerCount = 0;

    MessageView messages[kMaxMessages];
    size_t messageCount = 0;
};

} // namespace ui

#endif // MAXL_UI_VIEW_MODEL_H
