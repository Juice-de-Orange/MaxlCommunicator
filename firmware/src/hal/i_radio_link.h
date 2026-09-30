/*
 * Radio abstraction.
 *
 * CLAUDE.md 2.6 puts the link layer behind this interface "to keep the framing
 * code free of RadioLib types". docs/decisions/0001-open-decisions.md D3 places
 * the interface in hal/ rather than link/, because the SX1262 implementation
 * would otherwise have to include a link/ header and break the layering.
 *
 * The useful side effect: RadioLib's allocations stay in hal/, outside the
 * no-alloc check that scripts/check_no_alloc.py runs over link/ and app/.
 *
 * This is not a second protocol stack (CLAUDE.md 2.6). The SX1262 holds one modem
 * configuration at a time and this interface reflects that -- there is one
 * modulation, one preamble length, one state.
 */

#ifndef MAXL_HAL_I_RADIO_LINK_H
#define MAXL_HAL_I_RADIO_LINK_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

enum class RadioResult : uint8_t {
    Ok = 0,
    Busy,        ///< a transmission or receive window is already in progress
    BadParam,
    Timeout,
    HardwareError,
};

/// Modem configuration. Bandwidth and coding rate are fixed by CLAUDE.md 1.4
/// (BW125, CR4/5, explicit header, CRC on) and are therefore not parameters --
/// only the two things that actually vary are.
struct Modulation {
    uint8_t spreadingFactor;  ///< 7..12
    uint32_t frequencyHz;     ///< centre frequency, capped to the band by link/band
    int8_t txPowerDbm;        ///< already clamped by the per-band ERP ceiling
};

/// What the radio measured on a received frame. CLAUDE.md 2.2 has the ACK carry
/// these back to the sender, which is where adaptive SF gets its input.
struct RxInfo {
    int16_t rssiDbm;
    int8_t snrDb;
};

/// Delivered on every successfully demodulated frame. The link layer treats a
/// frame that fails the MIC exactly like one that never arrived, so validation
/// happens above this interface, not below it.
class IRadioObserver {
public:
    virtual ~IRadioObserver() = default;
    virtual void onFrameReceived(const uint8_t *data, size_t len, const RxInfo &info) = 0;
    virtual void onTransmitComplete(RadioResult result) = 0;
};

class IRadioLink {
public:
    virtual ~IRadioLink() = default;

    virtual void setObserver(IRadioObserver *observer) = 0;

    virtual RadioResult setModulation(const Modulation &modulation) = 0;

    /*
     * Preamble length in symbols.
     *
     * REVIEW.md C5: startReceiveDutyCycleAuto() silently drops 10-25 % of packets
     * unless the receiver's preamble length matches the sender's. So this is set
     * on both sides from the same number, and link/airtime computes it from the
     * sniff interval rather than anyone picking a constant.
     */
    virtual RadioResult setPreambleLength(uint16_t symbols) = 0;

    /// Transmit one frame. Completion arrives via onTransmitComplete.
    virtual RadioResult transmit(const uint8_t *data, size_t len) = 0;

    /*
     * Start the SX1262's own RxDutyCycle (CLAUDE.md 2.3). The MCU sleeps through
     * the sniff cycle; the radio wakes it only on a preamble.
     *
     * minSymbols below 8 is unreliable (REVIEW.md C5, Semtech's guidance is 8 to
     * latch a preamble); the caller passes 8 for SF <= 10 and 12 above.
     *
     * The sniff interval is deliberately NOT a parameter, and that is a
     * correction rather than a simplification. The SX1262 derives its sleep and
     * wake periods from the SENDER'S PREAMBLE LENGTH and minSymbols -- the
     * interval is the result, not the input. Passing an interval here invited
     * exactly the bug that shipped: the driver handed RadioLib
     * (minSymbols, intervalMs) for its (senderPreambleLength, minSymbols), which
     * made it read "preamble 8, minSymbols 500", trip its own
     * 2*minSymbols > senderPreambleLength guard, fall back to continuous receive
     * and return success. SetRxDutyCycle never ran on this hardware once.
     *
     * The preamble comes from setPreambleLength() above, which is the single
     * place it is configured and the number both ends must already agree on.
     */
    virtual RadioResult startReceiveDutyCycle(uint8_t minSymbols) = 0;

    /**
     * Whether the last startReceiveDutyCycle() really started a duty cycle.
     *
     * False means the radio is listening continuously instead -- which receives
     * perfectly well and destroys the power budget CLAUDE.md 2.3 is built on.
     * The distinction is invisible from the return code, so it is asked for
     * separately and reported by bring-up 19.
     */
    virtual bool rxDutyCycleActive() const = 0;

    /// Continuous receive. Used for the short window after a transmission that
    /// requested an ACK, where sniffing would cost more latency than it saves.
    virtual RadioResult startReceiveContinuous() = 0;

    /// Put the radio in its lowest power state.
    virtual RadioResult sleep() = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_RADIO_LINK_H
