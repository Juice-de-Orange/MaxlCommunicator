#include "app/view.h"

#include "ble/bridge_codec.h"
#include "link/band.h"
#include "link/frame.h"

namespace app {
namespace {

/*
 * EVT_FRAME_RX, from docs/bridge-protocol.md section 4:
 *
 *   counter:u32, src:u16, type:u8, rssi:i16, snr:i8, len:u8, payload[len]
 *
 * Eleven bytes of header and then the payload. The MESSAGES screen reads these
 * out of the journal because there is nowhere else for a received message to
 * live: app::MessageQueue is the OUTBOUND queue, and until now the screen showed
 * only what this device had sent. A communicator whose message list does not
 * contain what arrived is not finished.
 */
constexpr size_t kFrameRxHeaderBytes = 11;
constexpr size_t kFrameRxSrcOffset = 4;
constexpr size_t kFrameRxTypeOffset = 6;
constexpr size_t kFrameRxLenOffset = 10;

bool isReceivedText(const JournalEntry &entry, uint16_t *src, const uint8_t **payload,
                    size_t *length)
{
    if (entry.opcode != static_cast<uint8_t>(ble::EventCode::FrameRx)) {
        return false;
    }
    if (entry.length < kFrameRxHeaderBytes) {
        return false;
    }
    if (entry.body[kFrameRxTypeOffset] != static_cast<uint8_t>(link::FrameType::Text)) {
        return false;
    }

    const uint8_t declared = entry.body[kFrameRxLenOffset];
    if (kFrameRxHeaderBytes + declared > entry.length) {
        return false;
    }

    *src = static_cast<uint16_t>(entry.body[kFrameRxSrcOffset] |
                                 (entry.body[kFrameRxSrcOffset + 1] << 8));
    *payload = entry.body + kFrameRxHeaderBytes;
    *length = declared;
    return true;
}

/// Ages are unsigned differences and the clock can go backwards -- SET_TIME and
/// a GNSS fix both jump it. A negative age would wrap to seventy years.
uint32_t ageSeconds(uint32_t nowUnix, uint32_t thenUnix)
{
    if (thenUnix == 0 || nowUnix < thenUnix) {
        return 0;
    }
    return nowUnix - thenUnix;
}

ui::DeliveryState mapState(MessageState state)
{
    switch (state) {
    case MessageState::Pending: return ui::DeliveryState::InFlight;
    case MessageState::Queued: return ui::DeliveryState::Queued;
    case MessageState::InFlight: return ui::DeliveryState::InFlight;
    case MessageState::Delivered: return ui::DeliveryState::Delivered;
    case MessageState::Undelivered: return ui::DeliveryState::Undelivered;
    }
    return ui::DeliveryState::InFlight;
}

void copyPreview(char *out, size_t outSize, const char *text, size_t textLen)
{
    size_t i = 0;
    for (; i + 1 < outSize && i < textLen && text[i] != '\0'; ++i) {
        const char c = text[i];
        // The font covers printable ASCII and renders anything else as '?'.
        // Turning a newline into a space here keeps a two-line message from
        // drawing a stray glyph mid-line.
        out[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    out[i] = '\0';
}

} // namespace

uint32_t newestReceivedCounter(const Node &node)
{
    const Journal &journal = node.journal();
    for (size_t remaining = journal.size(); remaining > 0; --remaining) {
        const JournalEntry &entry = journal.at(remaining - 1);
        uint16_t src = 0;
        const uint8_t *payload = nullptr;
        size_t length = 0;
        if (isReceivedText(entry, &src, &payload, &length)) {
            return entry.counter;
        }
    }
    return 0;
}

void buildViewModel(const Node &node, const Peripherals &peripherals, uint32_t nowUnix,
                    uint32_t uptimeS, uint32_t lastReadCounter, ui::ViewModel &out)
{
    out = ui::ViewModel{};

    /*
     * Quantised to 50 mV before it reaches the screen, and the raw value still
     * goes on the wire.
     *
     * Two reasons, and the second is the one that costs power. docs/test-plan.md
     * gate 1.6 allows the ADC 50 mV of error, so a display in centivolts claims a
     * precision the instrument does not have. And measured on node A, sketch 12:
     * two consecutive samples 30 s apart read 4910 and 4903 mV, which is noise --
     * but it crossed a centivolt boundary and cost a 325 ms partial refresh. Over
     * an hour that is 120 refreshes of a number that never really moved, which is
     * precisely what gate 4.1's note calls "a value that technically changed".
     *
     * A real discharge crosses a 50 mV step every twenty minutes or so. That is
     * the rate at which this number deserves to be redrawn.
     */
    out.batteryMv = static_cast<uint16_t>(((peripherals.batteryMv + 25u) / 50u) * 50u);
    out.batteryLow = peripherals.batteryLow;
    out.uptimeS = uptimeS;

    /*
     * CLAUDE.md 1.2: with no valid time the node is fully transmit-blocked. The
     * UI does not decide that and must not appear to -- it asks the node, which
     * asks the budget and the counter and the clock.
     */
    out.timeValid = node.transmitAllowed();

    const NodeConfig &config = node.config();
    out.band = config.band == 0 ? 'P' : 'M';
    // What the modem is actually set to -- with adaptive SF wired (2026-08-31),
    // the configured fixedSf is only the truth when sfMode says fixed.
    out.currentSf = node.currentSf();

    // --- sensors ----------------------------------------------------------
    out.haveLocalSensor = peripherals.sensor.valid;
    out.localHumidityValid = peripherals.sensor.humidityValid;
    out.localTempCentiC = peripherals.sensor.tempCentiC;
    out.localHumidityCentiPct = peripherals.sensor.humidityCentiPct;
    out.localPressurePa = peripherals.sensor.pressurePa;

    // --- position ---------------------------------------------------------
    out.gnssPowered = peripherals.gnssPowered;
    out.gnssTimedOut = peripherals.gnssTimedOut;
    out.haveFix = peripherals.fix.hasPosition;
    out.latE7 = peripherals.fix.latitudeE7;
    out.lonE7 = peripherals.fix.longitudeE7;
    out.altM = peripherals.fix.altitudeM;
    out.hdopTenths = peripherals.fix.hdopTenths;
    out.satellites = peripherals.fix.satellites;
    out.fixAgeS = ageSeconds(nowUnix, peripherals.fixTakenAtUnix);

    // --- peers ------------------------------------------------------------
    const PeerTable &peers = node.peers();
    for (size_t i = 0; i < PeerTable::slots() && out.peerCount < ui::kMaxPeers; ++i) {
        const Peer &peer = peers.at(i);
        if (!peer.used) {
            continue;
        }
        ui::PeerView &view = out.peers[out.peerCount++];
        view.nodeId = peer.nodeId;
        view.rssi = peer.lastRssi;
        view.snr = peer.lastSnr;
        view.sf = peer.lastSf;
        view.everHeard = peer.framesReceived > 0;
        view.lastSeenS = ageSeconds(nowUnix, peer.lastSeenUnix);
        view.hasPosition = peer.hasPosition;
        view.latE7 = peer.latE7;
        view.lonE7 = peer.lonE7;
    }

    /*
     * --- messages ---------------------------------------------------------
     *
     * Two sources, and no way to interleave them by time. Received messages live
     * in the journal, which carries a counter and no timestamp; sent ones live
     * in the queue, which carries createdAtUnix and an id from a different
     * supply entirely (decision D10). There is no common ordering key, and
     * inventing one would mean putting a clock reading into the journal -- a
     * change to the bridge protocol, and not one to make in passing.
     *
     * So: newest received first, then newest sent in whatever room is left.
     * That order is also the right one to read. What arrived is what you want to
     * see; what you sent, you already know about.
     */
    uint32_t unread = 0;
    const Journal &journal = node.journal();
    for (size_t remaining = journal.size(); remaining > 0; --remaining) {
        const JournalEntry &entry = journal.at(remaining - 1);
        uint16_t src = 0;
        const uint8_t *payload = nullptr;
        size_t length = 0;
        if (!isReceivedText(entry, &src, &payload, &length)) {
            continue;
        }

        const bool isUnread = entry.counter > lastReadCounter;
        if (isUnread) {
            ++unread;
        }
        if (out.messageCount >= ui::kMaxMessages) {
            // Keep counting the unread ones even once the screen is full --
            // STATUS shows the total, and a total that stopped at five would be
            // a worse answer than no answer.
            continue;
        }

        ui::MessageView &view = out.messages[out.messageCount++];
        copyPreview(view.preview, sizeof(view.preview),
                    reinterpret_cast<const char *>(payload), length);
        view.peerId = src;
        view.state = ui::DeliveryState::Received;
        view.unread = isUnread;
        // No age: a journal entry has no timestamp, and "0s" against a message
        // from yesterday is a lie the screen would tell every time.
    }

    const MessageQueue &queue = node.queue();
    for (size_t remaining = queue.size();
         remaining > 0 && out.messageCount < ui::kMaxMessages; --remaining) {
        const QueuedMessage &message = queue.at(remaining - 1);
        ui::MessageView &view = out.messages[out.messageCount++];

        copyPreview(view.preview, sizeof(view.preview), message.text, message.textLen);
        view.peerId = message.dst;
        view.state = mapState(message.state);
        view.ageS = ageSeconds(nowUnix, message.createdAtUnix);

        if (message.state == MessageState::Queued && message.releaseAtUnix > nowUnix) {
            // Rounded up: "0m" against a message that has not gone yet reads as
            // a bug, and one minute early is a lie in the harmless direction.
            const uint32_t seconds = message.releaseAtUnix - nowUnix;
            const uint32_t minutes = (seconds + 59u) / 60u;
            view.releaseInMinutes = minutes > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(minutes);
        }
    }

    /*
     * D18: the heads of undelivered messages whose text was given up. The
     * failure stays on the screen after the entry itself was reduced --
     * CLAUDE.md 2.4 demands the failure be surfaced, not the prose kept.
     */
    static const char kTruncatedLabel[] = "(text dropped)";
    for (size_t remaining = queue.stubCount();
         remaining > 0 && out.messageCount < ui::kMaxMessages; --remaining) {
        const UndeliveredStub &stub = queue.stubAt(remaining - 1);
        ui::MessageView &view = out.messages[out.messageCount++];
        copyPreview(view.preview, sizeof(view.preview), kTruncatedLabel,
                    sizeof(kTruncatedLabel) - 1);
        view.peerId = stub.dst;
        view.state = ui::DeliveryState::Undelivered;
        view.truncated = true;
        view.ageS = ageSeconds(nowUnix, stub.createdAtUnix);
    }

    // Unread means received and not yet seen. An undelivered message of our own
    // is a different problem and the MESSAGES screen boxes it; counting it here
    // would make the STATUS number mean two things at once.
    out.unreadCount = unread > 255u ? 255u : static_cast<uint8_t>(unread);
}

} // namespace app
