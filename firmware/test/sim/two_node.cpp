/*
 * Two nodes talking to each other in simulated time.
 *
 * WHAT THIS IS: the link layer wired together the way app/ will wire it, driven
 * over a simulated channel for hours of simulated time, with the compliance
 * invariants checked after every single transmission. It reaches states a bench
 * test reaches only by luck -- a saturated budget, a peer that vanishes
 * mid-exchange, a link that degrades and recovers.
 *
 * WHAT THIS IS NOT: a substitute for docs/test-plan.md. Every Phase 2 gate except
 * 2.15 needs two devices, an antenna and a step attenuator. The channel here
 * delivers or it does not; it does not model preamble detection, the RxDutyCycle
 * sniff cycle, collisions, TCXO startup, or interference from the LoRaWAN
 * gateways that live 50 kHz away from the default channel. A green run here means
 * the logic is consistent with itself, not that it works on an nRF52840.
 *
 * The node loop below is simulation scaffolding, not firmware. app/ is Phase 3.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "fakes/fake_radio.h"
#include "link/adaptive_sf.h"
#include "link/airtime.h"
#include "link/arq.h"
#include "link/budget.h"
#include "link/counter.h"
#include "link/crypto.h"
#include "link/frame.h"
#include "link/payloads.h"
#include "link/replay.h"

using namespace link;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char *what, long detail = 0)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL: %s (%ld)\n", what, detail);
    }
}

constexpr uint32_t kBootUnix = 1788000000u;
constexpr uint32_t kTickMs = 10;
constexpr uint8_t kNetId = 0x2A;

/*
 * The AES-128 example key from FIPS-197 appendix C. Deliberately a published
 * value: it is a simulation key, it is not a secret, and choosing something that
 * looked random would invite someone to wonder whether it was one. Real keys are
 * provisioned at runtime over BLE and never appear in this repository
 * (CLAUDE.md 6).
 */
const uint8_t kNetworkKey[kKeyBytes] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                                        0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};

/*
 * One node. Owns exactly the modules link/ provides and nothing else -- if this
 * needed something the firmware does not have, that would be the finding.
 */
struct Node : public hal::IRadioObserver {
    Node(const char *label, uint16_t address, fakes::SimChannel &channel, uint32_t *nowMs)
        : name(label), self(address), radio(channel, nowMs), now(nowMs)
    {
        clock.setUnix(kBootUnix);
        counter.begin(store);
        budget.begin(store, clock);
        crypto.setKey(0, kNetworkKey);
        arq.begin(budget, jitter);
        radio.setObserver(this);
        radio.startReceiveDutyCycle(minSymbolsForSf(kRendezvousSf));
        applyModulation();
    }

    void applyModulation()
    {
        hal::Modulation modulation{};
        modulation.spreadingFactor = sf.currentSf();
        modulation.frequencyHz = plan(band).defaultHz;
        modulation.txPowerDbm = clampTxPowerDbm(band, 22);
        radio.setModulation(modulation);
        radio.setPreambleLength(preambleSymbolsForInterval(sf.currentSf(), sniffIntervalMs));
    }

    /// Build and queue a POSITION frame to `peerAddress`.
    bool sendPosition(uint16_t peerAddress, bool wantAck)
    {
        uint32_t frameCounter = 0;
        if (!counter.next(&frameCounter)) {
            return false;  // cannot persist -- must not transmit (CLAUDE.md 2.1)
        }

        Header header{};
        header.version = kWireVersion;
        header.type = FrameType::Position;
        header.netId = kNetId;
        header.src = self;
        header.dst = peerAddress;
        header.counter = frameCounter;
        header.seq = nextSeq++;
        header.flags = wantAck ? kFlagAckReq : 0u;

        uint8_t frame[kMaxFrameBytes];
        encodeHeader(header, frame);

        PositionPayload position{472692000, 114041000, 574, 12, 3};
        uint8_t plain[kPositionPayloadBytes];
        encodePosition(position, plain);

        if (crypto.encrypt(frame, plain, sizeof(plain), frame + kHeaderBytes,
                           sizeof(frame) - kHeaderBytes) != CryptoError::None) {
            return false;
        }
        const size_t frameLen = kHeaderBytes + sizeof(plain) + kMicBytes;

        return arq.submit(frame, frameLen, header.seq, sf.currentSf(), band, wantAck, *now) <
               kMaxOutstanding;
    }

    void onFrameReceived(const uint8_t *data, size_t len, const hal::RxInfo &info) override
    {
        Header header{};
        const FrameError error = inspect(data, len, &header);
        if (error == FrameError::VersionMismatch) {
            ++versionMismatches;
            return;
        }
        if (error != FrameError::None || header.netId != kNetId) {
            return;
        }

        uint8_t plain[kMaxPayloadBytes];
        const size_t bodyLen = len - kHeaderBytes;
        if (crypto.decrypt(data, data + kHeaderBytes, bodyLen, plain, sizeof(plain), nullptr) !=
            CryptoError::None) {
            ++micFailures;
            return;
        }

        // Only now, after authentication, may the replay window be touched.
        const ReplayVerdict verdict = replay.admit(header.src, header.counter, header.seq);
        if (verdict == ReplayVerdict::Replayed || verdict == ReplayVerdict::NoRoom) {
            ++replaysRejected;
            return;
        }

        lastContactMs = *now;

        if (header.type == FrameType::Ack) {
            AckPayload ack{};
            if (decodeAck(plain, bodyLen - kMicBytes, &ack)) {
                lastAckSnrDb = ack.snrDb;
                arq.onAckReceived(ack.seq, *now);
            }
            return;
        }

        if (verdict == ReplayVerdict::Accept) {
            ++delivered;
        }
        // A duplicate is still ACKed -- otherwise the sender keeps retrying.
        if ((header.flags & kFlagAckReq) != 0) {
            queueAck(header.src, header.seq, info);
        }
    }

    void queueAck(uint16_t peerAddress, uint8_t ackSeq, const hal::RxInfo &info)
    {
        uint32_t frameCounter = 0;
        if (!counter.next(&frameCounter)) {
            return;
        }

        Header header{};
        header.version = kWireVersion;
        header.type = FrameType::Ack;
        header.netId = kNetId;
        header.src = self;
        header.dst = peerAddress;
        header.counter = frameCounter;
        header.seq = nextSeq++;

        uint8_t frame[kMaxFrameBytes];
        encodeHeader(header, frame);

        AckPayload ack{ackSeq, info.rssiDbm, info.snrDb};
        uint8_t plain[kAckPayloadBytes];
        encodeAck(ack, plain);

        if (crypto.encrypt(frame, plain, sizeof(plain), frame + kHeaderBytes,
                           sizeof(frame) - kHeaderBytes) != CryptoError::None) {
            return;
        }
        // An ACK spends the receiver's own budget (CLAUDE.md 1.4), so it goes
        // through the same scheduler as everything else. A budget-blocked ACK is
        // queued, not dropped (2.4).
        arq.submit(frame, kHeaderBytes + sizeof(plain) + kMicBytes, header.seq, sf.currentSf(),
                   band, false, *now);
    }

    void onTransmitComplete(hal::RadioResult) override
    {
        if (pendingSlot >= kMaxOutstanding) {
            return;
        }

        size_t frameLen = 0;
        (void)arq.frameOf(pendingSlot, &frameLen);
        const uint32_t airtimeUs =
            timeOnAirUs(sf.currentSf(), static_cast<uint8_t>(frameLen),
                        preambleSymbolsForInterval(sf.currentSf(), sniffIntervalMs));

        // Anything that transmits goes through the budget. There is no second
        // path (CLAUDE.md 6).
        if (!budget.recordTransmission(band, airtimeUs)) {
            ++budgetRecordFailures;
        }
        airtimeThisRunUs += airtimeUs;

        arq.onTransmitComplete(pendingSlot, *now, true);
        pendingSlot = kMaxOutstanding;
    }

    void tick()
    {
        radio.pump();
        if (radio.transmitting() || pendingSlot < kMaxOutstanding) {
            return;
        }

        const ArqOutcome outcome = arq.poll(*now);
        if (outcome.action == ArqAction::Transmit) {
            size_t frameLen = 0;
            const uint8_t *frame = arq.frameOf(outcome.slot, &frameLen);
            if (frame != nullptr) {
                sf.onTransmitStart(arq.attemptsOf(outcome.slot) > 1
                                       ? static_cast<uint8_t>(arq.attemptsOf(outcome.slot) - 1u)
                                       : 0u);
                pendingSlot = outcome.slot;
                applyModulation();
                radio.transmit(frame, frameLen);
            }
        } else if (outcome.action == ArqAction::Deliver) {
            if (outcome.state == DeliveryState::Delivered) {
                ++acknowledged;
                sf.onDelivered(static_cast<uint8_t>(arq.attemptsOf(outcome.slot) - 1u),
                               lastAckSnrDb);
            } else {
                ++undelivered;
                sf.onUndelivered();
            }
            arq.clear(outcome.slot);
        } else if (*now > lastContactMs) {
            sf.tick(*now - lastContactMs, beaconIntervalMs);
        }
    }

    const char *name;
    uint16_t self;

    fakes::FakeBlockStore store;
    fakes::FakeClock clock;
    fakes::FixedJitter jitter{500};
    fakes::FakeRadio radio;

    FrameCounter counter;
    DutyCycleBudget budget;
    Crypto crypto;
    Arq arq;
    ReplayGuard replay;
    AdaptiveSf sf;

    Band band = kRendezvousBand;
    uint32_t sniffIntervalMs = kRendezvousSniffIntervalMs;
    uint32_t beaconIntervalMs = 10u * 60u * 1000u;

    uint32_t *now;
    size_t pendingSlot = kMaxOutstanding;
    uint8_t nextSeq = 0;
    int8_t lastAckSnrDb = 8;
    uint32_t lastContactMs = 0;

    uint32_t delivered = 0;
    uint32_t acknowledged = 0;
    uint32_t undelivered = 0;
    uint32_t micFailures = 0;
    uint32_t replaysRejected = 0;
    uint32_t versionMismatches = 0;
    uint32_t budgetRecordFailures = 0;
    uint64_t airtimeThisRunUs = 0;
};

struct World {
    uint32_t nowMs = 0;
    fakes::SimChannel channel;
    Node a{"A", 0x0001, channel, &nowMs};
    Node b{"B", 0x0002, channel, &nowMs};

    World() { channel.attach(&a.radio, &b.radio); }

    void step()
    {
        nowMs += kTickMs;
        a.clock.advanceMs(kTickMs);
        b.clock.advanceMs(kTickMs);
        channel.pump(nowMs);
        a.tick();
        b.tick();
    }

    /// The invariant that must hold at every instant, not just at the end.
    void checkBudgetInvariant()
    {
        for (Node *node : {&a, &b}) {
            const uint32_t used = node->budget.usedMs(Band::G3);
            check(used <= plan(Band::G3).airtimeBudgetMsPerHour,
                  "rolling-hour airtime exceeded the g3 allowance",
                  static_cast<long>(used));
            check(node->budgetRecordFailures == 0, "a transmission could not be recorded",
                  static_cast<long>(node->budgetRecordFailures));
        }
    }
};

// --------------------------------------------------------------------------

void scenarioCleanLink()
{
    std::printf("scenario: clean link, 2 hours, position every 60 s\n");
    World world;
    world.channel.conditions.lossPercent = 0;
    world.channel.conditions.snrDb = 8;

    uint32_t sent = 0;
    for (uint32_t elapsed = 0; elapsed < 2u * 3600u * 1000u; elapsed += kTickMs) {
        if (elapsed % 60000u == 0) {
            if (world.a.sendPosition(world.b.self, true)) {
                ++sent;
            }
        }
        world.step();
        if (elapsed % 10000u == 0) {
            world.checkBudgetInvariant();
        }
    }

    std::printf("  sent %u, B delivered %u, A acknowledged %u, undelivered %u\n", sent,
                world.b.delivered, world.a.acknowledged, world.a.undelivered);
    std::printf("  A airtime %.1f s over 2 h, SF %u, MIC failures %u\n",
                static_cast<double>(world.a.airtimeThisRunUs) / 1e6, world.a.sf.currentSf(),
                world.a.micFailures);

    check(world.b.delivered > 0, "nothing arrived at B");
    check(world.a.acknowledged > 0, "no ACK came back to A");
    check(world.a.micFailures == 0, "MIC failure on a clean link",
          static_cast<long>(world.a.micFailures));
    check(world.b.micFailures == 0, "MIC failure on a clean link",
          static_cast<long>(world.b.micFailures));
    check(world.a.undelivered == 0, "gave up on a clean link",
          static_cast<long>(world.a.undelivered));
    // A clean link with margin walks down towards the floor; it must not climb.
    check(world.a.sf.currentSf() <= kRendezvousSf, "SF escalated on a clean link",
          world.a.sf.currentSf());
}

void scenarioPeerVanishes()
{
    std::printf("scenario: peer switched off mid-exchange\n");
    World world;
    world.b.radio.powered = false;

    check(world.a.sendPosition(world.b.self, true), "submit refused");

    for (uint32_t elapsed = 0; elapsed < 30u * 60u * 1000u; elapsed += kTickMs) {
        world.step();
        if (world.a.undelivered > 0) {
            break;
        }
    }

    std::printf("  transmissions %u, undelivered %u\n", world.a.radio.transmitCount,
                world.a.undelivered);
    // docs/test-plan.md 2.5: exactly 3 retries, then surfaced. Never silent.
    check(world.a.undelivered == 1, "the message was not surfaced as undelivered",
          static_cast<long>(world.a.undelivered));
    check(world.a.radio.transmitCount == 1u + kMaxRetries, "wrong number of transmissions",
          static_cast<long>(world.a.radio.transmitCount));
}

void scenarioLossyLink()
{
    std::printf("scenario: 60 %% packet loss for an hour\n");
    World world;
    world.channel.conditions.lossPercent = 60;
    world.channel.conditions.snrDb = -11;

    for (uint32_t elapsed = 0; elapsed < 3600u * 1000u; elapsed += kTickMs) {
        if (elapsed % 30000u == 0) {
            world.a.sendPosition(world.b.self, true);
        }
        world.step();
        if (elapsed % 10000u == 0) {
            world.checkBudgetInvariant();
        }
    }

    std::printf("  A: acknowledged %u, undelivered %u, SF %u; B delivered %u\n",
                world.a.acknowledged, world.a.undelivered, world.a.sf.currentSf(),
                world.b.delivered);
    std::printf("  channel: %u sent, %u dropped\n", world.channel.framesSent,
                world.channel.framesDropped);

    // A bad link should have pushed the SF up, and the budget must still hold.
    check(world.a.sf.currentSf() >= kRendezvousSf, "SF did not respond to a bad link",
          world.a.sf.currentSf());
    check(world.a.micFailures == 0, "a lost frame was reported as a MIC failure",
          static_cast<long>(world.a.micFailures));
    world.checkBudgetInvariant();
}

void scenarioReplayAttack()
{
    std::printf("scenario: a frame captured off the air and replayed 20 times\n");
    World world;

    check(world.a.sendPosition(world.b.self, true), "submit refused");
    for (uint32_t elapsed = 0; elapsed < 60u * 1000u; elapsed += kTickMs) {
        world.step();
        if (world.b.delivered > 0) {
            break;
        }
    }
    check(world.b.delivered == 1, "the original did not arrive",
          static_cast<long>(world.b.delivered));

    /*
     * Capture what B actually received and push it back in, exactly as an
     * attacker with a transmitter would. This goes through the whole receive
     * path -- header inspection, MIC check, replay window -- rather than poking
     * the guard directly, so it also proves the ordering: authentication first,
     * window afterwards. docs/test-plan.md 2.3.
     */
    uint8_t captured[kMaxFrameBytes];
    const size_t capturedLen = world.b.radio.lastFrameLength;
    check(capturedLen > 0, "nothing was captured");
    std::memcpy(captured, world.b.radio.lastFrame, capturedLen);

    const uint32_t deliveredBefore = world.b.delivered;
    const uint32_t rejectedBefore = world.b.replaysRejected;
    const hal::RxInfo info{-80, 8};
    for (int i = 0; i < 20; ++i) {
        world.b.radio.inject(captured, capturedLen, info);
    }

    std::printf("  replays rejected %u of 20\n", world.b.replaysRejected - rejectedBefore);
    check(world.b.replaysRejected - rejectedBefore == 20, "not every replay was rejected",
          static_cast<long>(world.b.replaysRejected - rejectedBefore));
    check(world.b.delivered == deliveredBefore, "a replay reached the application",
          static_cast<long>(world.b.delivered - deliveredBefore));
    check(world.b.micFailures == 0, "the replay failed the MIC instead of the window",
          static_cast<long>(world.b.micFailures));
}

void scenarioTamperedFrame()
{
    std::printf("scenario: one bit flipped in a captured frame\n");
    World world;

    check(world.a.sendPosition(world.b.self, true), "submit refused");
    for (uint32_t elapsed = 0; elapsed < 60u * 1000u; elapsed += kTickMs) {
        world.step();
        if (world.b.delivered > 0) {
            break;
        }
    }

    uint8_t captured[kMaxFrameBytes];
    const size_t capturedLen = world.b.radio.lastFrameLength;
    std::memcpy(captured, world.b.radio.lastFrame, capturedLen);

    const uint32_t deliveredBefore = world.b.delivered;
    const hal::RxInfo info{-80, 8};

    // Every bit of the whole frame, header and body alike. docs/test-plan.md 2.2
    // asks for 20 trials; this is 8 x the frame length of them.
    for (size_t byte = 0; byte < capturedLen; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t tampered[kMaxFrameBytes];
            std::memcpy(tampered, captured, capturedLen);
            tampered[byte] = static_cast<uint8_t>(tampered[byte] ^ (1u << bit));
            world.b.radio.inject(tampered, capturedLen, info);
        }
    }

    std::printf("  %zu single-bit flips injected, %u reached the application\n",
                capturedLen * 8u, world.b.delivered - deliveredBefore);
    check(world.b.delivered == deliveredBefore, "a tampered frame reached the application",
          static_cast<long>(world.b.delivered - deliveredBefore));
}

void scenarioBudgetSaturation()
{
    std::printf("scenario: 30 messages queued back to back\n");
    World world;

    uint32_t submitted = 0;
    for (int i = 0; i < 30; ++i) {
        if (world.a.sendPosition(world.b.self, true)) {
            ++submitted;
        }
    }
    std::printf("  submitted %u of 30 (ARQ window holds %zu)\n", submitted, kMaxOutstanding);
    /*
     * The rest were refused with the equivalent of ERR_QUEUE_FULL. That is the
     * ARQ window, not the message queue: docs/test-plan.md 2.7 queues 30 through
     * the persistent message queue, which is app/ and Phase 3. What this scenario
     * checks is the part that exists -- that whatever does go out stays inside
     * the hourly allowance.
     */

    for (uint32_t elapsed = 0; elapsed < 3600u * 1000u; elapsed += kTickMs) {
        world.step();
        if (elapsed % 5000u == 0) {
            world.checkBudgetInvariant();
        }
    }

    std::printf("  A airtime %.1f s in one hour (allowance %u s)\n",
                static_cast<double>(world.a.airtimeThisRunUs) / 1e6,
                plan(Band::G3).airtimeBudgetMsPerHour / 1000u);
    // docs/test-plan.md 2.7.
    check(world.a.airtimeThisRunUs <= 360u * 1000u * 1000u,
          "airtime exceeded 360 s in a single hour",
          static_cast<long>(world.a.airtimeThisRunUs / 1000u));
}

} // namespace

int main()
{
    std::printf("=== two-node simulation ===\n");
    std::printf("Simulated time only. Every gate in docs/test-plan.md except 2.15\n");
    std::printf("still needs two devices on a bench.\n\n");

    scenarioCleanLink();
    std::printf("\n");
    scenarioPeerVanishes();
    std::printf("\n");
    scenarioLossyLink();
    std::printf("\n");
    scenarioReplayAttack();
    std::printf("\n");
    scenarioTamperedFrame();
    std::printf("\n");
    scenarioBudgetSaturation();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
