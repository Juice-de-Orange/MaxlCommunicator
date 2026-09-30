/*
 * LoRa time on air, and the preamble sizing that follows from the sniff interval.
 *
 * CLAUDE.md 1.4 -- "airtime is the scarce resource" -- and 2.3, where the sniff
 * interval is the single tuning knob. This module is what makes both concrete:
 * the duty cycle budget charges what this computes, and the receiver's preamble
 * length comes from here rather than from a constant someone picked.
 *
 * Fixed by CLAUDE.md 1.4 and therefore not parameters: BW125, CR4/5, explicit
 * header, CRC on. Only the spreading factor, the payload length and the preamble
 * length vary.
 *
 * All arithmetic is exact integer arithmetic in microseconds. No floating point:
 * the results feed a legally binding budget, and a value that depends on the
 * rounding mode of a double is not something to build compliance on. The unit
 * tests reproduce every entry of the CLAUDE.md 1.4 and 2.3 tables to the
 * millisecond, so a future edit that drifts from the spec fails the build.
 */

#ifndef MAXL_LINK_AIRTIME_H
#define MAXL_LINK_AIRTIME_H

#include <stdint.h>

namespace link {

/// Symbol duration in microseconds at BW125. Exact: 2^sf / 125000 s = 2^sf * 8 us.
uint32_t symbolDurationUs(uint8_t sf);

/// Time on air of a frame of `payloadBytes` total on-air bytes, in microseconds.
uint32_t timeOnAirUs(uint8_t sf, uint8_t payloadBytes, uint16_t preambleSymbols);

/// The same, rounded to the nearest millisecond. This is the form the CLAUDE.md
/// tables are written in.
uint32_t timeOnAirMs(uint8_t sf, uint8_t payloadBytes, uint16_t preambleSymbols);

/*
 * Preamble long enough to span one sniff interval (CLAUDE.md 2.3).
 *
 * The sender's preamble must still be running when the receiver's next sniff
 * happens, or the frame is missed entirely. ceil(interval / Tsym).
 *
 * REVIEW.md C5: the receiver must be given the same number via
 * IRadioLink::setPreambleLength, or startReceiveDutyCycleAuto() silently drops
 * 10-25 % of packets. That is why this is one function used by both sides.
 */
uint16_t preambleSymbolsForInterval(uint8_t sf, uint32_t sniffIntervalMs);

/*
 * Symbols the receiver needs to latch a preamble.
 *
 * REVIEW.md C5: below 8 this is unreliable; Semtech's guidance is 8. CLAUDE.md
 * 2.3 asks for 8 at SF <= 10 and 12 above.
 */
uint8_t minSymbolsForSf(uint8_t sf);

/*
 * Standard preamble for a frame that is not bridging a sniff interval -- an ACK
 * sent into a receiver that is already listening continuously (CLAUDE.md 1.4
 * tabulates airtime with an 8-symbol preamble).
 */
constexpr uint16_t kStandardPreambleSymbols = 8;

/**
 * How long the sender listens continuously for an ACK, and therefore how long
 * the short preamble above is safe to use.
 *
 * Both ends compute it from the spreading factor alone, so neither has to be
 * told and neither can disagree. The sender opens a continuous receive window
 * of this length when it transmits a frame that asked for an ACK; the receiver
 * uses the short preamble only if its ACK fits inside that window, and the full
 * sniff preamble otherwise, because by then the sender is asleep again.
 *
 * Without the window an ACK has to carry the sniff preamble -- at SF9 with a
 * 500 ms interval that is 123 symbols against 8, turning a 185 ms frame into a
 * 690 ms one and, at 10 %, a 1.7 s lockout into a 6.2 s one. CLAUDE.md 1.4
 * tabulates the ACK at the short preamble, so that table describes this window.
 */
constexpr uint32_t kAckWindowSlackMs = 300;

uint32_t ackWindowMs(uint8_t sf);

} // namespace link

#endif // MAXL_LINK_AIRTIME_H
