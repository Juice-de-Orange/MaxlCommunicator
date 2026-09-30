/*
 * Two whole nodes, talking to each other.
 *
 * sim/two_node.cpp already drives link/ end to end -- framing, crypto, ARQ,
 * budget -- against a simulated channel. This drives the layer above it: two
 * app::Node instances with their own key stores, queues, journals and counters,
 * a GATT server in front of each, and the same channel between them.
 *
 * A message is typed into node A's GATT characteristic as a phone would type it,
 * and the test asks whether it appears in node B's journal for B's phone to
 * collect. Everything in between is the shipping code: the real crypto, the real
 * replay window, the real duty cycle budget.
 *
 * What this cannot prove is anything about an SX1262. The radio is a fake and
 * the channel is arithmetic. docs/test-plan.md phase 2 wants two devices on a
 * bench, and no amount of simulation substitutes for that -- what it does is
 * make sure the bench time is spent on radio problems rather than on a bug that
 * a laptop could have found.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/node.h"
#include "ble/bridge_codec.h"
#include "ble/gatt_server.h"
#include "fakes/fake_ble.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "fakes/fake_key_store.h"
#include "fakes/fake_radio.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char *what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

const uint8_t kNetworkKey[16] = {0x8f, 0x2a, 0x11, 0xd0, 0x5c, 0x77, 0x93, 0x04,
                                 0xbe, 0x61, 0x38, 0xaa, 0x0d, 0xf5, 0x42, 0x19};
constexpr uint8_t kNetId = 0x2A;

/// One node and everything around it.
struct Peer {
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeKeyStore keys;
    fakes::FakeBleTransport ble;
    fakes::FixedJitter jitter;
    app::Node node;
    ble::GattServer gatt;
    fakes::FakeRadio *radio = nullptr;

    Peer(uint32_t *nowMs, fakes::SimChannel &channel, uint16_t nodeId)
        : node(clock, store, keys), gatt(ble, node)
    {
        clock.setUnix(1788126105);
        node.begin(nodeId);
        keys.store(0, kNetId, kNetworkKey);
        ble.isBonded = true;
        radio = new fakes::FakeRadio(channel, nowMs);
        node.attachRadio(*radio, jitter);
    }

    ~Peer() { delete radio; }

    /// Send a command exactly as a phone would: chunked, over the transport.
    void command(ble::Opcode opcode, const std::vector<uint8_t> &body, uint32_t nowMs)
    {
        uint8_t message[ble::kMaxMessageBytes];
        const size_t total = ble::encodeMessage(static_cast<uint8_t>(opcode), 1, body.data(),
                                                body.size(), message, sizeof(message));
        ble::Chunker chunker;
        chunker.begin(message, total, ble.negotiatedMtu, 1);
        uint8_t chunk[ble::kMaxMtu];
        size_t written = 0;
        while (chunker.next(chunk, sizeof(chunk), &written)) {
            gatt.onChunk(chunk, written, nowMs);
        }
    }

    void sendText(uint16_t dst, const std::string &text, uint32_t nowMs)
    {
        std::vector<uint8_t> body = {static_cast<uint8_t>(dst), static_cast<uint8_t>(dst >> 8),
                                     static_cast<uint8_t>(text.size())};
        body.insert(body.end(), text.begin(), text.end());
        command(ble::Opcode::SendText, body, nowMs);
    }
};

/// Advance both nodes and the channel together.
void run(uint32_t *nowMs, uint32_t forMs, Peer &a, Peer &b, fakes::SimChannel &channel)
{
    const uint32_t until = *nowMs + forMs;
    while (*nowMs < until) {
        *nowMs += 10;
        a.clock.advanceMs(10);
        b.clock.advanceMs(10);
        channel.pump(*nowMs);
        a.radio->pump();
        b.radio->pump();
        a.node.pumpRadio(*nowMs);
        b.node.pumpRadio(*nowMs);
    }
}

size_t textEventsIn(const app::Node &node)
{
    app::JournalEntry entries[64];
    const size_t howMany = node.journal().fetch(0, entries, 64);
    size_t texts = 0;
    for (size_t i = 0; i < howMany; ++i) {
        // EVT_FRAME_RX with a TEXT frame type at offset 6.
        if (entries[i].opcode == 0x81 && entries[i].length > 6 && entries[i].body[6] == 0x05) {
            ++texts;
        }
    }
    return texts;
}

size_t opcodeCountIn(const app::Node &node, uint8_t opcode)
{
    app::JournalEntry entries[64];
    const size_t howMany = node.journal().fetch(0, entries, 64);
    size_t hits = 0;
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode == opcode) {
            ++hits;
        }
    }
    return hits;
}

uint32_t u32At(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
           | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

/// The last EVT_FRAME_TX_RESULT in a node's journal, decoded.
struct TxResult {
    bool found = false;
    uint32_t counter = 0;
    uint8_t result = 0xFF;
    uint8_t attempts = 0;
    int16_t rssi = 0;
    int8_t snr = 0;
};

TxResult lastTxResultIn(const app::Node &node)
{
    app::JournalEntry entries[64];
    const size_t howMany = node.journal().fetch(0, entries, 64);
    TxResult out;
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode != 0x82 || entries[i].length < 12) {
            continue;
        }
        out.found = true;
        out.counter = u32At(entries[i].body);
        out.result = entries[i].body[7];
        out.attempts = entries[i].body[8];
        out.rssi = static_cast<int16_t>(entries[i].body[9]
                                        | (static_cast<uint16_t>(entries[i].body[10]) << 8));
        out.snr = static_cast<int8_t>(entries[i].body[11]);
    }
    return out;
}

/// The frame counter of the first TEXT frame a node received.
uint32_t firstTextRxCounterIn(const app::Node &node)
{
    app::JournalEntry entries[64];
    const size_t howMany = node.journal().fetch(0, entries, 64);
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode == 0x81 && entries[i].length > 6 && entries[i].body[6] == 0x05) {
            return u32At(entries[i].body);
        }
    }
    return 0xFFFFFFFFu;
}

void scenarioCleanLink()
{
    std::printf("\nscenario: a phone types a message into A, B's phone finds it\n");

    uint32_t now = 0;
    fakes::SimChannel channel(0xC0FFEE);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);

    a.sendText(2, "at the summit", now);
    check(a.node.queue().size() == 1, "A queued the message");

    run(&now, 60000, a, b, channel);

    std::printf("  A transmitted %u times, channel sent %u, dropped %u\n",
                a.radio->transmitCount, channel.framesSent, channel.framesDropped);
    std::printf("  A: MIC failures %u, replays rejected %u\n", a.node.micFailures(),
                a.node.replaysRejected());
    std::printf("  B: MIC failures %u, replays rejected %u\n", b.node.micFailures(),
                b.node.replaysRejected());

    check(a.radio->transmitCount > 0, "A actually transmitted");
    check(b.node.micFailures() == 0, "B saw no MIC failures");
    check(textEventsIn(b.node) >= 1, "the text reached B's journal");
    check(b.node.peers().find(0x0001) != nullptr, "B knows about A");

    /*
     * The whole point of 2026-08-31's rework: B ACKs, A hears it, and the
     * message reaches DELIVERED -- on the first attempt, with no retries. On
     * the first two-node contact this same exchange was 80 transmissions, one
     * acceptance and zero deliveries, because no seq was ever assigned and no
     * ACK was ever built.
     */
    check(a.node.queue().countInState(app::MessageState::Delivered) == 1,
          "A marked the message delivered");
    check(a.radio->transmitCount == 1, "first-attempt delivery, no retries");
    check(b.node.duplicatesReacked() == 0, "B never needed to re-ACK");

    const TxResult result = lastTxResultIn(a.node);
    check(result.found, "A journaled the TX result");
    check(result.found && result.result == 0, "and it says delivered");
    check(result.found && result.attempts == 1, "after exactly one attempt");
    // The ACK carries what B measured about A's frame (CLAUDE.md 2.2), and the
    // journal entry carries what the ACK said.
    check(result.found && result.rssi == -80 && result.snr == 8,
          "the TX result carries B's measurement");
    // D10's correlation: A's EVT_FRAME_TX_RESULT and B's EVT_FRAME_RX name the
    // same frame counter, so the server can join them over (src, counter).
    check(result.found && result.counter == firstTextRxCounterIn(b.node),
          "the (src, counter) correlation holds across the air");

    // Section 4 makes journaling binding for every event type: the budget
    // changed on both sides -- A spent airtime on the text, B on the ACK.
    check(opcodeCountIn(a.node, 0x87) >= 1, "A journaled its budget change");
    check(opcodeCountIn(b.node, 0x87) >= 1, "B journaled the ACK's budget change");

    /*
     * And the ACK came back on the SHORT preamble, into the continuous window A
     * opens after asking for one.
     *
     * These two have to agree or the ACK is inaudible: A listens continuously
     * for link::ackWindowMs, and B uses the short preamble only while that
     * lasts. missedTooShort counts frames that arrived at a sniffing receiver
     * with a preamble too short to latch -- the failure this coupling causes if
     * either side changes its mind alone. On the bench the long-preamble ACK
     * cost 690 ms instead of 185 and put both nodes on the same 6 s lockout,
     * where they transmitted over each other indefinitely.
     */
    check(a.radio->missedTooShort == 0, "no ACK was lost for being too short to latch");
    check(b.node.ackTxTotal() == 1, "B sent exactly one ACK");
    std::printf("  B sent %u ACK(s); A missed %u frame(s) as too short%s",
                b.node.ackTxTotal(), a.radio->missedTooShort, "\n");
}

void scenarioLostAck()
{
    std::printf("\nscenario: B's ACK is lost -- the retry is re-ACKed, not re-delivered\n");

    /*
     * Seed 0x1 with 35 %% loss drops exactly the second frame on the channel:
     * A's data arrives, B's ACK is lost, A's retry arrives, B's re-ACK arrives.
     * The retry travels under a FRESH counter with the same seq, so B's replay
     * window admits it, the seq history recognises it, and the application sees
     * the text once. This is the exchange the whole dedupe design exists for.
     */
    uint32_t now = 0;
    fakes::SimChannel channel(0x1u);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);
    channel.conditions.lossPercent = 35;

    a.sendText(2, "durch den Nebel", now);
    run(&now, 240000, a, b, channel);

    std::printf("  A transmitted %u times, channel dropped %u, B re-ACKed %u duplicates\n",
                a.radio->transmitCount, channel.framesDropped, b.node.duplicatesReacked());

    check(a.radio->transmitCount == 2, "the lost ACK forced exactly one retry");
    check(b.node.duplicatesReacked() == 1, "B recognised the retry and re-ACKed it");
    check(a.node.queue().countInState(app::MessageState::Delivered) == 1,
          "A marked the message delivered");
    check(textEventsIn(b.node) == 1, "B's application saw the text exactly once");

    const TxResult result = lastTxResultIn(a.node);
    check(result.found && result.result == 0 && result.attempts == 2,
          "the TX result says delivered on the second attempt");
}

void scenarioWrongKey()
{
    std::printf("\nscenario: B holds a different network key\n");

    uint32_t now = 0;
    fakes::SimChannel channel(0xBADBAD);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);

    uint8_t other[16];
    std::memcpy(other, kNetworkKey, sizeof(other));
    other[0] ^= 0xFF;
    b.keys.store(0, kNetId, other);
    b.node.attachRadio(*b.radio, b.jitter); // reload keys

    a.sendText(2, "is never heard", now);
    run(&now, 40000, a, b, channel);

    std::printf("  B: MIC failures %u, text events %zu\n", b.node.micFailures(),
                textEventsIn(b.node));
    check(b.node.micFailures() > 0, "B rejected the frames");
    check(textEventsIn(b.node) == 0, "nothing reached B's application");
    // And A never sees an ACK, so it gives up rather than reporting success.
    check(a.node.queue().countInState(app::MessageState::Delivered) == 0,
          "A did not report a delivery");
}

void scenarioNoKey()
{
    std::printf("\nscenario: A has no key at all\n");

    uint32_t now = 0;
    fakes::SimChannel channel(0x1234);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);

    a.keys.eraseAll();
    check(!a.node.transmitAllowed(), "A refuses to transmit without a key");

    a.sendText(2, "won't work", now);
    run(&now, 20000, a, b, channel);

    std::printf("  A transmitted %u times\n", a.radio->transmitCount);
    check(a.radio->transmitCount == 0, "A transmitted nothing");
    check(a.node.queue().empty(), "and the message was refused rather than queued");
}

void scenarioNoTime()
{
    std::printf("\nscenario: A's clock is not valid\n");

    uint32_t now = 0;
    fakes::SimChannel channel(0x5678);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);

    a.clock.invalidate();
    check(!a.node.transmitAllowed(), "A is transmit-blocked");

    a.sendText(2, "keine Zeit", now);
    run(&now, 20000, a, b, channel);

    // CLAUDE.md 1.2: no valid time means the budget cannot be reconstructed, and
    // a node that cannot account for its airtime must not spend any.
    check(a.radio->transmitCount == 0, "A transmitted nothing");
}

void scenarioReplay()
{
    std::printf("\nscenario: a frame captured off the air and replayed\n");

    uint32_t now = 0;
    fakes::SimChannel channel(0x9ABC);
    Peer a(&now, channel, 0x0001);
    Peer b(&now, channel, 0x0002);
    channel.attach(a.radio, b.radio);

    a.sendText(2, "einmal", now);
    run(&now, 30000, a, b, channel);

    const size_t before = textEventsIn(b.node);
    const uint32_t rejectedBefore = b.node.replaysRejected();
    check(before >= 1, "the first copy arrived");

    /*
     * Take what B actually received off the air and hand it to B again.
     *
     * `lastFrame` is a tap on the *receive* path, so a.radio->lastFrame is the
     * ACK that A received from B -- addressed to A, and B would drop it on the
     * destination check long before the replay window ever saw it. Replaying
     * that would have looked like a working guard and proved nothing.
     */
    const size_t captured = b.radio->lastFrameLength;
    uint8_t frame[link::kMaxFrameBytes];
    std::memcpy(frame, b.radio->lastFrame, captured);

    for (int i = 0; i < 20; ++i) {
        hal::RxInfo info{-85, 7};
        b.node.onFrameReceived(frame, captured, info);
    }

    std::printf("  B rejected %u replays, text events %zu -> %zu\n", b.node.replaysRejected(),
                before, textEventsIn(b.node));
    check(textEventsIn(b.node) == before, "no replay reached the application");
    check(b.node.replaysRejected() >= rejectedBefore + 20, "every replay was rejected");
}

/*
 * The receiver that never transmits.
 *
 * Under a real RxDutyCycle the SX1262 leaves the receive window on RxDone and
 * stays in STDBY_RC. Everything that armed the receiver did so after a
 * transmission, so a node that only listens -- a broadcast, or an ACK the duty
 * cycle budget parked -- heard exactly one frame and then went deaf. It never
 * showed on the bench because the driver had the arguments to
 * startReceiveDutyCycleAuto the wrong way round and the chip was in continuous
 * receive, where RxDone does not end the window; and it could not show in
 * simulation because FakeRadio kept listening until sleep().
 *
 * Broadcast is the isolating case: CLAUDE.md 2.4 says broadcast frames never
 * request an ACK, so B receives and transmits nothing at all.
 */
void scenarioListenerNeverTransmits()
{
    std::printf("\nscenario: a listener that never transmits stays listening\n");

    for (int variant = 0; variant < 2; ++variant) {
        const bool rearm = variant == 0;

        uint32_t now = 0;
        fakes::SimChannel channel(0x5150);
        Peer a(&now, channel, 0x0001);
        Peer b(&now, channel, 0x0002);
        channel.attach(a.radio, b.radio);
        b.radio->rearmAfterReceive = rearm;

        a.sendText(link::kBroadcastAddress, "erste", now);
        run(&now, 20000, a, b, channel);
        const size_t afterFirst = textEventsIn(b.node);

        a.sendText(link::kBroadcastAddress, "zweite", now);
        run(&now, 20000, a, b, channel);
        const size_t afterSecond = textEventsIn(b.node);

        std::printf("  re-arm %s: B heard %zu then %zu, and transmitted %u times\n",
                    rearm ? "on " : "off", afterFirst, afterSecond, b.radio->transmitCount);
        check(b.radio->transmitCount == 0, "B never transmitted, so nothing else re-armed it");
        check(afterFirst == 1, "the first broadcast arrived either way");
        if (rearm) {
            check(afterSecond == 2, "with the re-arm, the second broadcast arrived too");
        } else {
            // The point of the whole scenario: without it, this is what the
            // bench would have shown, and the simulation can now show it first.
            check(afterSecond == 1, "without the re-arm, B is deaf after one frame");
        }
    }
}

} // namespace

int main()
{
    std::printf("=== two nodes, whole stack, simulated channel ===\n");
    std::printf("Simulated. docs/test-plan.md phase 2 still wants two devices on a bench;\n");
    std::printf("this only makes sure that bench time goes on radio problems.\n");

    scenarioCleanLink();
    scenarioLostAck();
    scenarioWrongKey();
    scenarioNoKey();
    scenarioNoTime();
    scenarioReplay();
    scenarioListenerNeverTransmits();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
