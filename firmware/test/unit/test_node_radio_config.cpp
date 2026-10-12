/*
 * The budget is charged for what the radio actually sent (issue #39).
 *
 * The band and the sniff interval are settings a phone can change over BLE
 * (docs/bridge-protocol.md 3). Until issue #39 a change reached the budget at
 * once but the radio only at the next spreading-factor change: a node switched
 * from g1 back to g3 went on transmitting on 868.1 MHz while spending g3's 10 %,
 * and the first frame after a sniff-interval change was charged for a preamble
 * it did not carry. CLAUDE.md 1.2 makes the duty cycle legally binding, and it
 * is binding for the band the frame is actually in.
 *
 * The fake radio records the settings each transmission really used, so every
 * test compares the charge against the air, not against the configuration.
 */

#include "doctest.h"

#include <cstring>
#include <vector>

#include "app/node.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "fakes/fake_key_store.h"
#include "fakes/fake_radio.h"
#include "link/airtime.h"
#include "link/band.h"
#include "link/budget.h"

namespace {

const uint8_t kKey[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                          0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
constexpr uint8_t kNetId = 0x2A;
constexpr uint32_t kBootUnix = 1788126105u;
const char kText[] = "twelve bytes";

std::vector<uint8_t> bandTlv(uint8_t band) { return {0x03, 0x01, band}; }

std::vector<uint8_t> sniffTlv(uint16_t ms)
{
    return {0x02, 0x02, static_cast<uint8_t>(ms & 0xFF), static_cast<uint8_t>(ms >> 8)};
}

struct Rig {
    uint32_t now = 0;
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeKeyStore keys;
    fakes::FixedJitter jitter;
    fakes::SimChannel channel;
    fakes::FakeRadio radio{channel, &now};
    app::Node node{clock, store, keys};
    uint32_t configVersion = 1;

    /// `before` is applied before the radio is attached -- the way bring-up 19
    /// configures a node, and the way a node comes up on g1.
    explicit Rig(const std::vector<uint8_t> &before = {})
    {
        clock.setUnix(kBootUnix);
        REQUIRE(node.begin(0x0001));
        keys.store(0, kNetId, kKey);
        if (!before.empty()) {
            configure(before);
        }
        channel.attach(&radio, nullptr);
        REQUIRE(node.attachRadio(radio, jitter));
    }

    void configure(const std::vector<uint8_t> &tlv)
    {
        uint32_t applied = 0;
        uint8_t unapplied[16];
        size_t unappliedCount = 0;
        REQUIRE(node.setConfig(configVersion++, tlv.data(), tlv.size(), &applied, unapplied,
                               &unappliedCount) == 0);
        REQUIRE(unappliedCount == 0);
    }

    void sendText()
    {
        REQUIRE(node.sendText(0x0002, reinterpret_cast<const uint8_t *>(kText),
                              sizeof(kText) - 1) == 0);
    }

    void step()
    {
        now += 10;
        clock.advanceMonotonicMs(10);
        if (now % 1000u == 0) {
            clock.setUnix(clock.unixSeconds() + 1u);
        }
        channel.pump(now);
        radio.pump();
        node.pumpRadio(now);
    }

    void run(uint32_t forMs)
    {
        const uint32_t until = now + forMs;
        while (now < until) {
            step();
        }
    }

    /// What the flash ring says was spent in a band -- the record a reboot
    /// reconstructs the budget from.
    uint64_t usedUs(link::Band band)
    {
        link::DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));
        return static_cast<uint64_t>(budget.usedMs(band)) * 1000u;
    }

    /// The airtime of the last frame as the radio really sent it.
    uint32_t lastTxAirtimeUs() const
    {
        return link::timeOnAirUs(radio.lastTxSf, static_cast<uint8_t>(radio.lastTxLength),
                                 radio.lastTxPreambleSymbols);
    }
};

/// The band whose channel holds this frequency. Fails the test if none does.
link::Band bandOf(uint32_t frequencyHz)
{
    if (link::channelFitsInBand(link::Band::G3, frequencyHz)) {
        return link::Band::G3;
    }
    REQUIRE(link::channelFitsInBand(link::Band::G1, frequencyHz));
    return link::Band::G1;
}

} // namespace

TEST_CASE("a sniff-interval change reaches the radio before the next frame")
{
    Rig rig;
    REQUIRE(rig.radio.preambleSymbols() == link::preambleSymbolsForInterval(9, 2000));

    rig.configure(sniffTlv(500));
    rig.run(100);
    CHECK(rig.radio.preambleSymbols() == link::preambleSymbolsForInterval(9, 500));

    rig.sendText();
    rig.run(3000);
    REQUIRE(rig.radio.transmitCount == 1);
    CHECK(rig.radio.lastTxPreambleSymbols == link::preambleSymbolsForInterval(9, 500));
    // Charged what went on the air, to the millisecond the ring records.
    CHECK(rig.usedUs(link::Band::G3) / 1000u == rig.lastTxAirtimeUs() / 1000u);
}

TEST_CASE("a band change reaches the radio, and frames are charged to the band they are in")
{
    // Up on g1, as a node configured before its radio starts would be.
    Rig rig(bandTlv(1));
    REQUIRE(bandOf(rig.radio.frequencyHz()) == link::Band::G1);

    SUBCASE("g1 to g3")
    {
        rig.configure(bandTlv(0));
        rig.run(100);
        CHECK(bandOf(rig.radio.frequencyHz()) == link::Band::G3);
        CHECK(rig.radio.txPowerDbm() == link::clampTxPowerDbm(link::Band::G3, 22));

        rig.sendText();
        rig.run(3000);
        REQUIRE(rig.radio.transmitCount == 1);
        const link::Band onAir = bandOf(rig.radio.lastTxFrequencyHz);
        CHECK(onAir == link::Band::G3);
        CHECK(rig.usedUs(onAir) / 1000u == rig.lastTxAirtimeUs() / 1000u);
    }

    SUBCASE("g1, then g3, then g1: power falls back to g1's ceiling")
    {
        rig.configure(bandTlv(0));
        rig.run(100);
        rig.configure(bandTlv(1));
        rig.run(100);
        CHECK(bandOf(rig.radio.frequencyHz()) == link::Band::G1);
        CHECK(rig.radio.txPowerDbm() <= link::plan(link::Band::G1).maxTxPowerDbm);

        rig.sendText();
        rig.run(3000);
        REQUIRE(rig.radio.transmitCount == 1);
        CHECK(bandOf(rig.radio.lastTxFrequencyHz) == link::Band::G1);
        CHECK(rig.radio.lastTxPowerDbm <= link::plan(link::Band::G1).maxTxPowerDbm);
        CHECK(rig.usedUs(link::Band::G1) / 1000u == rig.lastTxAirtimeUs() / 1000u);
        CHECK(rig.usedUs(link::Band::G3) == 0);
    }
}

TEST_CASE("a setting changed while a frame is on the air does not move under it")
{
    Rig rig;
    rig.sendText();
    while (!rig.radio.transmitting()) {
        rig.step();
    }
    const uint16_t sentWith = rig.radio.preambleSymbols();
    REQUIRE(sentWith == link::preambleSymbolsForInterval(9, 2000));

    rig.configure(sniffTlv(500));
    rig.configure(bandTlv(1));
    // The frame on the air keeps what it started with ...
    CHECK(rig.radio.preambleSymbols() == sentWith);
    CHECK(bandOf(rig.radio.frequencyHz()) == link::Band::G3);

    rig.run(3000);
    // ... is charged for exactly that, in the band it was in ...
    CHECK(rig.radio.lastTxPreambleSymbols == sentWith);
    CHECK(rig.usedUs(link::Band::G3) / 1000u == rig.lastTxAirtimeUs() / 1000u);
    CHECK(rig.usedUs(link::Band::G1) == 0);

    // ... and the new settings take over once the radio is free again (after
    // the ACK window this frame opened has run out).
    rig.run(2000);
    CHECK(rig.radio.preambleSymbols() == link::preambleSymbolsForInterval(9, 500));
    CHECK(bandOf(rig.radio.frequencyHz()) == link::Band::G1);
}
