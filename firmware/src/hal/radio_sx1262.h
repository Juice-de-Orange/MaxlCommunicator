/*
 * IRadioLink on the SX1262, through RadioLib.
 *
 * CLAUDE.md 2.6: the link layer sits behind this interface "to keep the framing
 * code free of RadioLib types", and decision D3 puts the interface in hal/ so
 * that this file -- the only one that includes RadioLib -- does not have to
 * reach into link/. The useful side effect is that RadioLib's allocations stay
 * outside the no-alloc check that runs over link/ and app/.
 *
 * Three board facts are baked in because they are properties of the module, not
 * choices (docs/hardware/pinmap.md):
 *
 *   DIO2 drives the TX/RX switch and is bonded inside the module -- it is not an
 *   MCU pin, and telling RadioLib otherwise leaves the antenna disconnected on
 *   transmit with no error anywhere.
 *   DIO3 supplies the TCXO at 1.8 V. Without it the oscillator never starts and
 *   every operation times out.
 *   The TCXO adds about 5 ms of startup to every wake (CLAUDE.md 2.3).
 *
 * NOT verified on hardware. Bring-up sketch 07 reads the chip over raw SPI and
 * has not been run yet; nothing below has ever transmitted. Two devices and a
 * bench are what phase 2 needs, and this is written so that day starts with a
 * driver rather than a blank file.
 */

#ifndef MAXL_HAL_RADIO_SX1262_H
#define MAXL_HAL_RADIO_SX1262_H

#include "hal/i_radio_link.h"

#include <stddef.h>
#include <stdint.h>

namespace hal {

class Sx1262Radio : public IRadioLink {
public:
    /// Reset the module and bring the modem up in the rendezvous configuration
    /// (CLAUDE.md 2.5: SF9 / BW125 / CR4/5 on the primary channel).
    bool begin(const Modulation &initial);

    void setObserver(IRadioObserver *observer) override;
    RadioResult setModulation(const Modulation &modulation) override;
    RadioResult setPreambleLength(uint16_t symbols) override;
    RadioResult transmit(const uint8_t *data, size_t len) override;
    RadioResult startReceiveDutyCycle(uint8_t minSymbols) override;
    RadioResult startReceiveContinuous() override;
    bool rxDutyCycleActive() const override { return rxDutyCycleActive_; }
    RadioResult sleep() override;

    /**
     * Drain whatever the DIO1 interrupt flagged.
     *
     * RadioLib's callback runs in interrupt context, where nothing may allocate,
     * block, or call back into the application. So the ISR sets a flag and this
     * -- called from the main loop -- does the work and notifies the observer.
     */
    void service();

    /// Microseconds the last transmission spent on air, measured between the
    /// start of transmit() and the DIO1 TxDone. Gate 2.10 compares this against
    /// what link/airtime computed, and 5 % is the allowance.
    uint32_t lastAirtimeUs() const { return lastAirtimeUs_; }

    /// The raw 24-bit GetPacketStatus word of the last received frame:
    /// RssiPkt << 16 | SnrPkt << 8 | SignalRssiPkt. Reported by the bring-up
    /// sketches so the byte order the RSSI fix relies on can be confirmed on
    /// air (see the RadioLib note in radio_sx1262.cpp).
    uint32_t lastPacketStatusRaw() const { return lastPacketStatusRaw_; }

private:
    IRadioObserver *observer_ = nullptr;
    bool transmitting_ = false;
    uint32_t txStartedUs_ = 0;
    uint32_t lastAirtimeUs_ = 0;
    uint32_t lastPacketStatusRaw_ = 0;
    uint16_t preambleSymbols_ = 8;

    /*
     * What the chip was last told to do, so service() can put it back.
     *
     * Under a real RxDutyCycle the SX1262 lands in STDBY_RC after RxDone and
     * stays there. Nothing re-armed it: a node that receives and does not
     * transmit -- a broadcast, or an ACK the duty cycle budget parked -- went
     * deaf after one frame. It never showed because the argument order above
     * kept the chip in continuous receive, where RxDone does not end the
     * window. Fixing that bug armed this one, which is why they are fixed
     * together.
     */
    enum class RxMode : uint8_t { Idle, DutyCycle, Continuous };
    RxMode rxMode_ = RxMode::Idle;
    uint8_t rxMinSymbols_ = 8;
    bool rxDutyCycleActive_ = false;

    /*
     * Why a receive window ended without a frame. Both are ordinary radio
     * weather, and both used to leave service() through an early return -- which
     * under a real duty cycle meant the chip stayed in STDBY_RC and the node
     * never heard anything again. They are counted because "the receiver went
     * quiet" and "the receiver is dead" look identical from the link layer.
     */
    uint32_t rxCrcFailures_ = 0;
    uint32_t rxEmptyIrqs_ = 0;

    /// Put the receiver back into whatever mode it was in. Every path out of a
    /// receive event goes through this.
    void rearmReceive();

public:
    uint32_t rxCrcFailures() const { return rxCrcFailures_; }
    uint32_t rxEmptyIrqs() const { return rxEmptyIrqs_; }

private:
    uint8_t spreadingFactor_ = 9;

    RadioResult mapError(int16_t code) const;
};

} // namespace hal

#endif // MAXL_HAL_RADIO_SX1262_H
