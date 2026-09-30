/*
 * IGnss on the L76K, over Serial1 at 9600 baud.
 *
 * Pins from docs/hardware/pinmap.md, and the direction of the UART pair is the
 * one thing in that document with a warning attached: Meshtastic's comments
 * contradict its own defines, and the defines are the ones that are right --
 * P1.09 is the nRF's RX, P1.08 its TX, confirmed against both the vendor
 * firmware and cfr34k.
 *
 * "Off" means the reset line held low. The module has no supply switch of its
 * own on this board -- it hangs off VDD_POWR with everything else -- so holding
 * it in reset is the strongest off available without cutting the peripheral rail
 * out from under the RTC and the flash as well. Whether that is enough to matter
 * is docs/test-plan.md gate 1.7's second half, and it needs a meter: a module
 * that stays powered because a pin was left high is exactly the failure CLAUDE.md
 * 1.5 is written against, and a firmware that believes its own powerOff() is not
 * evidence.
 *
 * Serial1 is closed on power-down too. An open UART holds the nRF's UARTE
 * peripheral awake, which is a milliamp of its own on a 2.38 mA budget.
 */

#ifndef MAXL_HAL_GNSS_L76K_H
#define MAXL_HAL_GNSS_L76K_H

#include "hal/i_gnss.h"

#include <stdint.h>

namespace hal {

class GnssL76k : public IGnss {
public:
    static constexpr uint32_t kBaud = 9600;

    /// A cold start on this class of module is tens of seconds outdoors and
    /// never indoors. The caller passes its own bound; this is what the UI uses.
    static constexpr uint32_t kDefaultTimeoutMs = 120000;

    bool begin() override;
    void requestFix(uint32_t timeoutMs, uint32_t nowMs) override;
    void powerOff() override;
    GnssState poll(uint32_t nowMs) override;
    GnssState state() const override { return state_; }
    const GnssFix &fix() const override { return parser_.fix(); }
    uint32_t timeToFixMs() const override { return timeToFixMs_; }

    /// Sentences the parser accepted and rejected. Nonzero rejections right
    /// after a power-up are normal -- the first bytes out of the module are
    /// always partial.
    uint32_t acceptedSentences() const { return parser_.acceptedCount(); }
    uint32_t rejectedSentences() const { return parser_.rejectedCount(); }

    /// Bytes seen since the last requestFix(). Zero with the module powered is
    /// a wiring fault, and it is the first thing to look at.
    uint32_t bytesSeen() const { return bytesSeen_; }

private:
    NmeaParser parser_;
    GnssState state_ = GnssState::Off;
    uint32_t requestedAtMs_ = 0;
    uint32_t timeoutMs_ = 0;
    uint32_t timeToFixMs_ = 0;
    uint32_t bytesSeen_ = 0;
    bool started_ = false;

    /*
     * Whether Serial1.begin() has been called and not yet undone.
     *
     * Calling Uart::end() on a UART that was never begun HANGS THE CALLER, and
     * it hangs it in a way that looks like nothing at all: the core's end() does
     *
     *     while (!(nrfUart->EVENTS_TXSTOPPED && nrfUart->EVENTS_RXTO)) yield();
     *
     * and a disabled UARTE never raises either event, so the loop spins for
     * ever. The yield() keeps FreeRTOS scheduling, so USB stays enumerated and
     * the device looks alive from the host while the loop task is gone. Nothing
     * prints, the dead man's timer is never poked -- and the dead man's timer
     * lives in loop() too, so it cannot rescue this.
     *
     * That matters here specifically because CLAUDE.md 1.5 requires the module
     * to be powered down by default, so begin() calls powerOff() -- which is
     * exactly the first-call-before-any-begin() case. Found on node A,
     * 2026-08-31, by bring-up sketch 11 hanging silently for 140 seconds.
     */
    bool uartOpen_ = false;
};

} // namespace hal

#endif // MAXL_HAL_GNSS_L76K_H
