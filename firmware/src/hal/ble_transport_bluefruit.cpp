#include "ble_transport_bluefruit.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <bluefruit.h>
#pragma GCC diagnostic pop

#include <string.h>

namespace hal {
namespace {

/*
 * The service and its characteristics, byte for byte the same as
 * bridge/src/transport/webble.ts:
 *
 *   service 6d61786c-0001-4c6f-5261-4e6f64650000
 *   TX      …-0002-…   notify   device -> phone
 *   RX      …-0003-…   write    phone  -> device
 *   CONFIG  …-0004-…   read/write
 *   STATUS  …-0005-…   read/notify
 *
 * Bluefruit wants a 128-bit UUID least-significant byte first, so these are the
 * strings above reversed. Writing them the human way round produces a service no
 * client can find, and nothing anywhere reports an error.
 */
#define MAXL_UUID(second_byte)                                                                  \
    {                                                                                           \
        0x00, 0x00, 0x65, 0x64, 0x6f, 0x4e, 0x61, 0x52, 0x6f, 0x4c, (second_byte), 0x00, 0x6c,  \
            0x78, 0x61, 0x6d                                                                    \
    }

const uint8_t kServiceUuid[16] = MAXL_UUID(0x01);
const uint8_t kTxUuid[16] = MAXL_UUID(0x02);
const uint8_t kRxUuid[16] = MAXL_UUID(0x03);
const uint8_t kConfigUuid[16] = MAXL_UUID(0x04);
const uint8_t kStatusUuid[16] = MAXL_UUID(0x05);

/*
 * Braces, not parentheses.
 *
 * `BLEService g_service(BLEUuid(kServiceUuid));` is the most vexing parse: the
 * compiler reads it as the declaration of a *function* returning BLEService, and
 * every later use fails with "request for member in something of non-class
 * type". Braces cannot be read that way.
 */
BLEService g_service{BLEUuid(kServiceUuid)};
BLECharacteristic g_tx{BLEUuid(kTxUuid)};
BLECharacteristic g_rx{BLEUuid(kRxUuid)};
BLECharacteristic g_config{BLEUuid(kConfigUuid)};
BLECharacteristic g_status{BLEUuid(kStatusUuid)};

/*
 * Bluefruit's callbacks are plain function pointers with no context argument, so
 * the instance has to be reachable from file scope. There is one BLE peripheral
 * on this chip; a second instance would be a bug rather than a case to support.
 */
IBleChunkSink *g_sink = nullptr;
BluefruitBleTransport *g_transport = nullptr;
uint16_t g_connection = BLE_CONN_HANDLE_INVALID;

/// docs/bridge-protocol.md section 1.1: at most MTU - 3 - 2 payload bytes, so a
/// chunk is never larger than the negotiated MTU.
constexpr uint16_t kMaxChunk = 247;

void onWrite(uint16_t connHandle, BLECharacteristic *chr, uint8_t *data, uint16_t len)
{
    (void)connHandle;
    (void)chr;
    if (g_sink != nullptr && len > 0) {
        g_sink->onChunk(data, len, millis());
    }
}

void onConnect(uint16_t connHandle)
{
    g_connection = connHandle;
}

void onDisconnect(uint16_t connHandle, uint8_t reason)
{
    (void)connHandle;
    (void)reason;
    g_connection = BLE_CONN_HANDLE_INVALID;
    if (g_sink != nullptr) {
        // Any partial message dies with the connection. Stitching the tail of an
        // old transfer onto a new one is the kind of bug that only shows up
        // when somebody walks out of range mid-sync.
        g_sink->onDisconnect();
    }
}

} // namespace

bool BluefruitBleTransport::begin(const char *deviceName, IBleChunkSink &sink)
{
    g_sink = &sink;
    g_transport = this;

    Bluefruit.begin();
    Bluefruit.setName(deviceName);

    /*
     * +4 dBm, the lowest step above the default.
     *
     * BLE is not what the power budget is spent on -- CLAUDE.md 5 targets
     * 2.38 mA average and cfr34k reaches ~100 uA standby with BLE up -- but the
     * phone is in the same pocket as the node, and turning the transmitter up
     * for a link measured in centimetres costs current for nothing.
     */
    Bluefruit.setTxPower(4);

    /*
     * The library blinks LED_BLUE while advertising and holds it on while
     * connected. variant.h points that name at P0.14 because it is the only pin
     * that is an LED on both hardware revisions -- and P0.14 is also the status
     * LED this firmware uses. Handing it to Bluefruit would mean the node's own
     * state and its BLE state fight over the same LED, so the library is told to
     * leave it alone.
     */
    Bluefruit.autoConnLed(false);

    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    // Bonding with a passkey, per section 2. Without this a phone can pair with
    // "just works" and every bonded-tier command becomes reachable by anyone
    // who happens to be nearby when the user is not looking.
    Bluefruit.Security.setIOCaps(true, false, false); // display yes, keyboard no
    Bluefruit.Security.setMITM(true);

    g_service.begin();

    g_tx.setProperties(CHR_PROPS_NOTIFY);
    g_tx.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    g_tx.setMaxLen(kMaxChunk);
    g_tx.begin();

    /*
     * RX is writable without encryption, and that is deliberate.
     *
     * The open tier -- GET_INFO, GET_STATUS, GET_BUDGET -- has to work on an
     * unbonded connection (section 2), and every command arrives through this
     * one characteristic. So the link-layer permission cannot be the guard; the
     * tier check in ble/gatt_server.cpp is, and it runs before any command is
     * dispatched. That check is tested; this permission is not a second one.
     */
    g_rx.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    g_rx.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
    g_rx.setMaxLen(kMaxChunk);
    g_rx.setWriteCallback(onWrite);
    g_rx.begin();

    // CONFIG and STATUS exist so a generic BLE tool can inspect a node without
    // implementing the protocol (section 1). Reading is open; writing config is
    // not, and here the link layer *can* carry the guard.
    g_config.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
    g_config.setPermission(SECMODE_OPEN, SECMODE_ENC_WITH_MITM);
    g_config.setMaxLen(244);
    g_config.begin();

    g_status.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    g_status.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    g_status.setMaxLen(32);
    g_status.begin();

    return true;
}

void BluefruitBleTransport::startAdvertising()
{
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    // The service UUID goes in the advertising packet so Web Bluetooth can filter
    // on it -- without it the phone's chooser lists every device in the room.
    Bluefruit.Advertising.addService(g_service);
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244); // 20 ms fast, 152 ms slow
    Bluefruit.Advertising.setFastTimeout(30);
    /*
     * 0 = advertise forever.
     *
     * CLAUDE.md 4.2: the connection drops whenever the phone's tab is hidden,
     * and "the device queues everything, and the user opens the app to sync". A
     * node that stopped advertising after a timeout would be a node the user
     * cannot reach when they finally open the app -- which is the whole
     * interaction this product is built around.
     */
    Bluefruit.Advertising.start(0);
}

bool BluefruitBleTransport::notify(const uint8_t *chunk, size_t length)
{
    if (g_connection == BLE_CONN_HANDLE_INVALID || length == 0 || length > kMaxChunk) {
        return false;
    }
    return g_tx.notify(chunk, static_cast<uint16_t>(length));
}

uint16_t BluefruitBleTransport::mtu() const
{
    if (g_connection == BLE_CONN_HANDLE_INVALID) {
        // "Assume 23 until negotiation completes" (section 1).
        return 23;
    }
    BLEConnection *connection = Bluefruit.Connection(g_connection);
    return connection != nullptr ? connection->getMtu() : 23;
}

bool BluefruitBleTransport::bonded() const
{
    if (g_connection == BLE_CONN_HANDLE_INVALID) {
        return false;
    }
    BLEConnection *connection = Bluefruit.Connection(g_connection);
    if (connection == nullptr) {
        return false;
    }
    /*
     * Both, and the difference matters.
     *
     * bonded() means an LTK for this peer is on file. secured() means *this*
     * link is currently encrypted with it. Section 2 asks for "an authenticated,
     * bonded connection with passkey authentication", which is the second --
     * a peer that bonded last week and is now talking over an unencrypted link
     * must not reach PROVISION_KEY.
     */
    return connection->bonded() && connection->secured();
}

bool BluefruitBleTransport::connected() const
{
    return g_connection != BLE_CONN_HANDLE_INVALID;
}

void BluefruitBleTransport::publishStatus(const uint8_t *body, size_t length)
{
    if (length == 0 || length > 32) {
        return;
    }
    g_status.write(body, static_cast<uint16_t>(length));
    if (g_connection != BLE_CONN_HANDLE_INVALID && g_status.notifyEnabled(g_connection)) {
        g_status.notify(body, static_cast<uint16_t>(length));
    }
}

void BluefruitBleTransport::publishConfig(const uint8_t *body, size_t length)
{
    if (length == 0 || length > 244) {
        return;
    }
    g_config.write(body, static_cast<uint16_t>(length));
}

} // namespace hal
