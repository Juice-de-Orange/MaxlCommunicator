/*
 * Turning a running node into a view model.
 *
 * app/view.cpp is the one place allowed to see both the node and the screens, so
 * it is also the one place where a mistake shows up as a screen that quietly
 * lies -- a stale age, a message that never appears, an unread count that counts
 * the wrong thing. All three are checked here against a real app::Node with
 * fake flash and a fake clock.
 *
 * The received-message path is the newest and the one worth the most attention:
 * until this existed, MESSAGES showed only what the device had SENT, because
 * app::MessageQueue is the outbound queue and received frames live in the
 * journal.
 */

#include "doctest.h"

#include <cstring>
#include <string>

#include "app/view.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_key_store.h"

namespace {

constexpr uint32_t kNow = 1788126105;

struct Fixture {
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeKeyStore keys;
    app::Node node{clock, store, keys};
    app::Peripherals peripherals;

    Fixture()
    {
        clock.setUnix(kNow);
        REQUIRE(node.begin(0x0001));

        // Without a key the node refuses to send, and sendText() would fail
        // before anything reached the queue. The same fixed test key
        // test_node_integration.cpp uses -- a vector, not a secret.
        uint8_t key[hal::kKeyBytes];
        for (size_t i = 0; i < sizeof(key); ++i) {
            key[i] = static_cast<uint8_t>(0x10 + i);
        }
        REQUIRE(node.provisionKey(0, 0x2A, key) == 0);
    }

    /// A TEXT frame arriving from `src`, which is what puts an EVT_FRAME_RX in
    /// the journal.
    void receiveText(uint16_t src, const char *text)
    {
        node.recordReceivedFrame(1000, src, static_cast<uint8_t>(link::FrameType::Text), -97,
                                 7, 9,
                                 reinterpret_cast<const uint8_t *>(text),
                                 std::strlen(text));
    }

    ui::ViewModel build(uint32_t lastReadCounter = 0, uint32_t uptimeS = 3600)
    {
        ui::ViewModel model;
        app::buildViewModel(node, peripherals, clock.unixSeconds(), uptimeS, lastReadCounter,
                            model);
        return model;
    }
};

} // namespace

TEST_CASE("a received message reaches the MESSAGES screen")
{
    Fixture fixture;
    fixture.receiveText(2, "on my way");

    const ui::ViewModel model = fixture.build();
    REQUIRE(model.messageCount == 1);
    CHECK(std::strcmp(model.messages[0].preview, "on my way") == 0);
    CHECK(model.messages[0].peerId == 2);
    CHECK(model.messages[0].state == ui::DeliveryState::Received);
}

TEST_CASE("received messages come out newest first")
{
    Fixture fixture;
    fixture.receiveText(2, "first");
    fixture.receiveText(2, "second");
    fixture.receiveText(3, "third");

    const ui::ViewModel model = fixture.build();
    REQUIRE(model.messageCount == 3);
    CHECK(std::strcmp(model.messages[0].preview, "third") == 0);
    CHECK(std::strcmp(model.messages[1].preview, "second") == 0);
    CHECK(std::strcmp(model.messages[2].preview, "first") == 0);
}

TEST_CASE("frames that are not TEXT do not turn up as messages")
{
    Fixture fixture;
    const uint8_t position[12] = {0};
    fixture.node.recordReceivedFrame(1001, 2, static_cast<uint8_t>(link::FrameType::Position),
                                     -90, 6, 9,
                                     position, sizeof(position));
    fixture.node.recordReceivedFrame(1002, 2, static_cast<uint8_t>(link::FrameType::Beacon),
                                     -90, 6, 9,
                                     nullptr, 0);

    const ui::ViewModel model = fixture.build();
    CHECK(model.messageCount == 0);
    CHECK(model.unreadCount == 0);
}

TEST_CASE("unread counts what arrived after the mark, and nothing else")
{
    Fixture fixture;
    fixture.receiveText(2, "one");
    fixture.receiveText(2, "two");

    ui::ViewModel model = fixture.build(0);
    CHECK(model.unreadCount == 2);
    CHECK(model.messages[0].unread);

    // MarkAllRead stores this and everything at or below it stops being unread.
    const uint32_t mark = app::newestReceivedCounter(fixture.node);
    CHECK(mark != 0);

    model = fixture.build(mark);
    CHECK(model.unreadCount == 0);
    CHECK_FALSE(model.messages[0].unread);

    fixture.receiveText(3, "three");
    model = fixture.build(mark);
    CHECK(model.unreadCount == 1);
}

TEST_CASE("the unread total keeps counting past what the screen can show")
{
    Fixture fixture;
    for (int i = 0; i < 12; ++i) {
        fixture.receiveText(2, "hello");
    }

    const ui::ViewModel model = fixture.build();
    CHECK(model.messageCount == ui::kMaxMessages);
    // A total that stopped at five would be a worse answer than none.
    CHECK(model.unreadCount == 12);
}

TEST_CASE("a sent message shows its state and its age")
{
    Fixture fixture;
    REQUIRE(fixture.node.sendText(2, reinterpret_cast<const uint8_t *>("hello"), 5) == 0);

    fixture.clock.setUnix(kNow + 300);
    const ui::ViewModel model = fixture.build();

    REQUIRE(model.messageCount >= 1);
    bool foundOutbound = false;
    for (size_t i = 0; i < model.messageCount; ++i) {
        if (model.messages[i].state != ui::DeliveryState::Received) {
            foundOutbound = true;
            CHECK(std::strcmp(model.messages[i].preview, "hello") == 0);
            CHECK(model.messages[i].ageS == 300);
        }
    }
    CHECK(foundOutbound);
}

TEST_CASE("received messages take the screen before sent ones")
{
    Fixture fixture;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(fixture.node.sendText(2, reinterpret_cast<const uint8_t *>("out"), 3) == 0);
    }
    for (int i = 0; i < 5; ++i) {
        fixture.receiveText(2, "in");
    }

    const ui::ViewModel model = fixture.build();
    REQUIRE(model.messageCount == ui::kMaxMessages);
    for (size_t i = 0; i < model.messageCount; ++i) {
        CAPTURE(i);
        CHECK(model.messages[i].state == ui::DeliveryState::Received);
    }
}

TEST_CASE("a received message carries no age, because the journal carries no time")
{
    Fixture fixture;
    fixture.receiveText(2, "hello");
    fixture.clock.setUnix(kNow + 86400);

    const ui::ViewModel model = fixture.build();
    REQUIRE(model.messageCount == 1);
    // Zero rather than a day, and the screen draws the sender instead. An age
    // here would be invented, and it would be wrong by exactly a day.
    CHECK(model.messages[0].ageS == 0);
}

TEST_CASE("a clock that jumped backwards does not produce an age of seventy years")
{
    Fixture fixture;
    REQUIRE(fixture.node.sendText(2, reinterpret_cast<const uint8_t *>("hello"), 5) == 0);

    // SET_TIME and a GNSS fix both move this clock, and not always forwards.
    fixture.clock.setUnix(kNow - 5000);
    const ui::ViewModel model = fixture.build();

    for (size_t i = 0; i < model.messageCount; ++i) {
        CAPTURE(i);
        CHECK(model.messages[i].ageS == 0);
    }
}

TEST_CASE("peers appear with their radio numbers and their age")
{
    Fixture fixture;
    fixture.receiveText(2, "hello");
    fixture.clock.setUnix(kNow + 45);

    const ui::ViewModel model = fixture.build();
    REQUIRE(model.peerCount == 1);
    CHECK(model.peers[0].nodeId == 2);
    CHECK(model.peers[0].everHeard);
    CHECK(model.peers[0].rssi == -97);
    CHECK(model.peers[0].snr == 7);
    CHECK(model.peers[0].sf == 9);
    CHECK(model.peers[0].lastSeenS == 45);
}

TEST_CASE("the peripherals come through unchanged, including a missing humidity sensor")
{
    Fixture fixture;
    fixture.peripherals.sensor.valid = true;
    fixture.peripherals.sensor.humidityValid = false;
    fixture.peripherals.sensor.tempCentiC = 2134;
    fixture.peripherals.sensor.pressurePa = 98123;
    fixture.peripherals.batteryMv = 3950;
    fixture.peripherals.batteryLow = false;

    const ui::ViewModel model = fixture.build();
    CHECK(model.haveLocalSensor);
    CHECK_FALSE(model.localHumidityValid);
    CHECK(model.localTempCentiC == 2134);
    CHECK(model.localPressurePa == 98123u);
    CHECK(model.batteryMv == 3950);
}

TEST_CASE("a fix age is measured from when the fix was taken")
{
    Fixture fixture;
    fixture.peripherals.fix.hasPosition = true;
    fixture.peripherals.fix.latitudeE7 = 482082000;  // Vienna, Stephansplatz
    fixture.peripherals.fix.longitudeE7 = 163738000;
    fixture.peripherals.fix.hdopTenths = 18;
    fixture.peripherals.fixTakenAtUnix = kNow;

    fixture.clock.setUnix(kNow + 120);
    const ui::ViewModel model = fixture.build();

    CHECK(model.haveFix);
    CHECK(model.latE7 == 482082000);
    CHECK(model.hdopTenths == 18);
    CHECK(model.fixAgeS == 120);
}

TEST_CASE("newestReceivedCounter answers zero when nothing has arrived")
{
    Fixture fixture;
    CHECK(app::newestReceivedCounter(fixture.node) == 0);

    fixture.receiveText(2, "hello");
    CHECK(app::newestReceivedCounter(fixture.node) != 0);
}

TEST_CASE("ADC noise in the battery reading does not reach the screen -- gate 4.1")
{
    Fixture fixture;

    /*
     * The two readings sketch 12 took 30 s apart on node A. Seven millivolts of
     * noise crossed a centivolt boundary and cost a 325 ms partial refresh; over
     * an hour that is 120 of them for a number that never moved.
     */
    fixture.peripherals.batteryMv = 4910;
    const ui::ViewModel a = fixture.build();
    fixture.peripherals.batteryMv = 4903;
    const ui::ViewModel b = fixture.build();
    CHECK(a.batteryMv == b.batteryMv);

    // A real 50 mV step still gets through -- this is quantisation, not a filter.
    fixture.peripherals.batteryMv = 4850;
    const ui::ViewModel c = fixture.build();
    CHECK(c.batteryMv != a.batteryMv);

    // And the quantisation is to 50 mV, rounded rather than truncated.
    CHECK(a.batteryMv % 50 == 0);
    CHECK(c.batteryMv % 50 == 0);
    CHECK(a.batteryMv == 4900);
    CHECK(c.batteryMv == 4850);
}
