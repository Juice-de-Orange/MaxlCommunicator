#include "radio_sx1262.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <RadioLib.h>
#pragma GCC diagnostic pop

namespace hal {
namespace {

/*
 * CLAUDE.md 1.4 fixes these and they are not parameters anywhere in the system:
 * BW125, CR4/5, explicit header, CRC on. Every airtime figure in the
 * specification and every number link/airtime computes assumes exactly this.
 */
constexpr float kBandwidthKHz = 125.0f;
constexpr uint8_t kCodingRate = 5;
constexpr uint8_t kSyncWord = 0x12; // private network, not LoRaWAN's 0x34
constexpr float kTcxoVoltage = 1.8f;
constexpr uint16_t kTcxoStartupMs = 5;

/*
 * DIO2 as the RF switch and DIO3 as the TCXO supply are properties of this
 * module, not choices. Getting either wrong produces a radio that reports
 * success and does nothing: with DIO2 unset the antenna is never connected on
 * transmit, and without DIO3 the oscillator never starts.
 */
/*
 * SX1262 with the packet status readable.
 *
 * RadioLib 7.7.1's getRSSI(true) returns the WRONG BYTE of GetPacketStatus
 * (0x14): the command answers RssiPkt, SnrPkt, SignalRssiPkt in that order, the
 * library packs them as data[0]<<16 | data[1]<<8 | data[2] -- and then reads the
 * packet RSSI from `& 0xFF`, which is SignalRssiPkt, while getSNR() correctly
 * reads bits 15:8. Measured on node A, 2026-08-31: SNR 11 dB with an "RSSI" of
 * exactly 0 on the same frame. So the RSSI is derived here from bits 23:16 of
 * the raw word, and the raw word is kept for the bring-up report so the byte
 * order can be confirmed over the air rather than argued from source.
 * getPacketStatus() is protected in SX126x, hence the subclass.
 */
class Sx1262WithStatus : public SX1262 {
public:
    using SX1262::SX1262;
    using SX1262::getPacketStatus;
};

Module g_module(SX126X_CS, SX126X_DIO1, SX126X_RESET, SX126X_BUSY);
Sx1262WithStatus g_radio(&g_module);

volatile bool g_irqFlag = false;

// No placement attribute: on the nRF52 the vector table and the handlers live in
// the same flash the rest of the image does. ESP-style IRAM_ATTR is an ESP32
// concern and does not exist here.
void onDio1()
{
    // Interrupt context: set a flag and leave. Everything else happens in
    // service(), from the main loop.
    g_irqFlag = true;
}

} // namespace

RadioResult Sx1262Radio::mapError(int16_t code) const
{
    switch (code) {
    case RADIOLIB_ERR_NONE:
        return RadioResult::Ok;
    case RADIOLIB_ERR_TX_TIMEOUT:
    case RADIOLIB_ERR_RX_TIMEOUT:
        return RadioResult::Timeout;
    case RADIOLIB_ERR_INVALID_FREQUENCY:
    case RADIOLIB_ERR_INVALID_BANDWIDTH:
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR:
    case RADIOLIB_ERR_INVALID_CODING_RATE:
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER:
    case RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH:
        return RadioResult::BadParam;
    default:
        return RadioResult::HardwareError;
    }
}

bool Sx1262Radio::begin(const Modulation &initial)
{
    const int16_t state = g_radio.begin(static_cast<float>(initial.frequencyHz) / 1e6f,
                                        kBandwidthKHz, initial.spreadingFactor, kCodingRate,
                                        kSyncWord, initial.txPowerDbm, preambleSymbols_,
                                        kTcxoVoltage, false);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    spreadingFactor_ = initial.spreadingFactor;

    // Bonded inside the module. RadioLib has to be told, or transmit() leaves
    // the antenna disconnected and reports success.
    g_radio.setDio2AsRfSwitch(true);
    g_radio.setTCXO(kTcxoVoltage, kTcxoStartupMs * 1000UL);

    // Explicit header and CRC, per CLAUDE.md 1.4. The explicit header is also
    // what carries the length, which is why the frame format has no length field.
    g_radio.explicitHeader();
    g_radio.setCRC(true);

    g_radio.setPacketReceivedAction(onDio1);
    g_radio.setPacketSentAction(onDio1);
    return true;
}

void Sx1262Radio::setObserver(IRadioObserver *observer)
{
    observer_ = observer;
}

RadioResult Sx1262Radio::setModulation(const Modulation &modulation)
{
    if (transmitting_) {
        // CLAUDE.md 2.5: "Never change SF mid-retry sequence." Refusing here is
        // the last line of that rule; the scheduler above is the first.
        return RadioResult::Busy;
    }
    if (modulation.spreadingFactor < 7 || modulation.spreadingFactor > 12) {
        return RadioResult::BadParam;
    }

    int16_t state = g_radio.setFrequency(static_cast<float>(modulation.frequencyHz) / 1e6f);
    if (state != RADIOLIB_ERR_NONE) {
        return mapError(state);
    }
    state = g_radio.setSpreadingFactor(modulation.spreadingFactor);
    if (state != RADIOLIB_ERR_NONE) {
        return mapError(state);
    }
    /*
     * The power is taken as given. link/band has already clamped it to the
     * per-band ERP ceiling (CLAUDE.md 1.3: "the configured TX power is capped
     * per band in code, not by the user"), and clamping it a second time here
     * with a different number is how the two would drift apart.
     */
    state = g_radio.setOutputPower(modulation.txPowerDbm);
    if (state != RADIOLIB_ERR_NONE) {
        return mapError(state);
    }
    spreadingFactor_ = modulation.spreadingFactor;
    return RadioResult::Ok;
}

RadioResult Sx1262Radio::setPreambleLength(uint16_t symbols)
{
    /*
     * The receiver's preamble must match the sender's or
     * startReceiveDutyCycleAuto() silently drops 10-25 % of packets (REVIEW.md
     * C5). Both sides get this from link/airtime, computed from the sniff
     * interval -- nobody picks a constant.
     */
    const int16_t state = g_radio.setPreambleLength(symbols);
    if (state == RADIOLIB_ERR_NONE) {
        preambleSymbols_ = symbols;
    }
    return mapError(state);
}

RadioResult Sx1262Radio::transmit(const uint8_t *data, size_t len)
{
    if (transmitting_) {
        return RadioResult::Busy;
    }
    if (data == nullptr || len == 0 || len > 255) {
        return RadioResult::BadParam;
    }

    // micros() before the call, so the measurement includes the ramp and the
    // TCXO settling that gate 2.10 is comparing against a computed figure.
    txStartedUs_ = micros();
    const int16_t state = g_radio.startTransmit(const_cast<uint8_t *>(data),
                                                static_cast<uint8_t>(len));
    if (state != RADIOLIB_ERR_NONE) {
        return mapError(state);
    }
    transmitting_ = true;
    return RadioResult::Ok;
}

RadioResult Sx1262Radio::startReceiveDutyCycle(uint8_t minSymbols)
{
    if (transmitting_) {
        return RadioResult::Busy;
    }
    /*
     * Below 8 symbols the preamble does not reliably latch -- Semtech's own
     * guidance, and REVIEW.md C5 says the same. The caller passes 8 for SF <= 10
     * and 12 above; refusing anything lower here means a future caller cannot
     * quietly reintroduce the bug.
     */
    if (minSymbols < 8) {
        return RadioResult::BadParam;
    }

    /*
     * The whole point of CLAUDE.md 2.3: the SX1262 sniffs for a preamble and
     * returns to sleep on its own, without waking the MCU.
     *
     * The argument order is (senderPreambleLength, minSymbols) and getting it
     * backwards is silent -- see i_radio_link.h. RadioLib derives the sleep and
     * wake periods from the preamble we are already configured for, so that is
     * what it gets.
     */
    const int16_t state = g_radio.startReceiveDutyCycleAuto(preambleSymbols_, minSymbols);
    if (state != RADIOLIB_ERR_NONE) {
        rxMode_ = RxMode::Idle;
        rxDutyCycleActive_ = false;
        return mapError(state);
    }

    /*
     * And whether it actually started one. RadioLib returns success either way:
     * when 2*minSymbols exceeds the sender's preamble there is no room to sleep,
     * so it quietly calls startReceive(RX_TIMEOUT_INF) instead
     * (PhysicalLayer::calculateRxDutyCycle). That receives perfectly and costs
     * the entire power budget, so it must not look like success from up here.
     * The condition is reproduced rather than queried because RadioLib exposes
     * no way to ask.
     */
    rxDutyCycleActive_ = (2u * static_cast<uint32_t>(minSymbols)) <= preambleSymbols_;
    rxMode_ = RxMode::DutyCycle;
    rxMinSymbols_ = minSymbols;
    return RadioResult::Ok;
}

RadioResult Sx1262Radio::startReceiveContinuous()
{
    if (transmitting_) {
        return RadioResult::Busy;
    }
    const int16_t state = g_radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        rxMode_ = RxMode::Continuous;
        rxDutyCycleActive_ = false;
    }
    return mapError(state);
}

RadioResult Sx1262Radio::sleep()
{
    // Warm start: the configuration survives, so waking does not have to reload
    // the modem. That matters at a 2 s sniff interval, where the wake happens
    // every two seconds for the life of the battery.
    rxMode_ = RxMode::Idle;
    rxDutyCycleActive_ = false;
    return mapError(g_radio.sleep(true));
}

void Sx1262Radio::service()
{
    if (!g_irqFlag) {
        return;
    }
    g_irqFlag = false;

    if (transmitting_) {
        transmitting_ = false;
        lastAirtimeUs_ = micros() - txStartedUs_;
        g_radio.finishTransmit();
        if (observer_ != nullptr) {
            observer_->onTransmitComplete(RadioResult::Ok);
        }
        return;
    }

    uint8_t buffer[256];
    const size_t length = g_radio.getPacketLength();
    if (length == 0 || length > sizeof(buffer)) {
        ++rxEmptyIrqs_;
        rearmReceive();
        return;
    }
    const int16_t state = g_radio.readData(buffer, static_cast<uint8_t>(length));
    if (state != RADIOLIB_ERR_NONE) {
        /*
         * A CRC failure is a frame that never arrived, not an error to report.
         * The link layer treats a failed MIC the same way (i_radio_link.h), and
         * surfacing radio-level noise as an event would give the application a
         * second, weaker notion of "a frame" to reason about.
         *
         * It still ends the receive window, though, and that is not weather:
         * measured on the bench on 2026-09-01, node A took four ACKs and then
         * heard nothing for five minutes while node B went on transmitting
         * them. The re-arm below used to sit only on the success path, so the
         * first corrupted frame left the chip in STDBY_RC for good.
         */
        ++rxCrcFailures_;
        rearmReceive();
        return;
    }

    RxInfo info{};
    lastPacketStatusRaw_ = g_radio.getPacketStatus();
    // RssiPkt is bits 23:16 of the raw word; the value is -RssiPkt/2 dBm. See
    // the Sx1262WithStatus comment for why RadioLib's getRSSI() is not used.
    const uint8_t rssiRaw = static_cast<uint8_t>((lastPacketStatusRaw_ >> 16) & 0xFFu);
    info.rssiDbm = static_cast<int16_t>(-(static_cast<int16_t>(rssiRaw) / 2));
    info.snrDb = static_cast<int8_t>(g_radio.getSNR());
    if (observer_ != nullptr) {
        observer_->onFrameReceived(buffer, length, info);
    }

    rearmReceive();
}

void Sx1262Radio::rearmReceive()
{
    /*
     * Under RxDutyCycle the chip is in STDBY_RC after any RxDone -- a good
     * frame, a corrupted one, or a preamble that led nowhere -- and will hear
     * nothing further until it is armed again. The only paths that armed it
     * were attachRadio, a spreading-factor change and the end of a
     * transmission, so a node that received without transmitting went deaf.
     *
     * Guarded on transmitting_ because the observer may have started a
     * transmission -- an ACK for the frame it just took -- and re-arming would
     * abort it. That path arms the receiver itself when the transmission
     * completes.
     */
    if (transmitting_) {
        return;
    }
    if (rxMode_ == RxMode::DutyCycle) {
        g_radio.startReceiveDutyCycleAuto(preambleSymbols_, rxMinSymbols_);
    } else if (rxMode_ == RxMode::Continuous) {
        g_radio.startReceive();
    }
}

} // namespace hal
