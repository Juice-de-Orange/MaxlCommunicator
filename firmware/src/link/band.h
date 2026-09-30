/*
 * EU 868 band plan.
 *
 * Reference: ETSI EN 300 220-2 V3.3.1 (2025-03), via CLAUDE.md 1.3.
 *
 *   g3 / P  869.4-869.65 MHz  500 mW ERP  10 %  360 s/hour  -- primary
 *   g1 / M  868.0-868.6 MHz    25 mW ERP   1 %   36 s/hour  -- fallback
 *
 * g3 is the working band, decided rather than provisional. It gives ten times the
 * airtime and legalises the SX1262's full +22 dBm. The budget tracker is
 * per-sub-band anyway, so keeping g1 selectable costs nothing.
 *
 * The transmit power ceiling lives here, in code, and not in a config TLV -- ERP
 * counts antenna gain, so a user-settable dBm number would be a compliance hole
 * (CLAUDE.md 1.3, REVIEW.md D3). docs/bridge-protocol.md 3 says the same from the
 * other side: txPowerDbm is "clamped by the per-band ceiling, never trusted".
 */

#ifndef MAXL_LINK_BAND_H
#define MAXL_LINK_BAND_H

#include <stdint.h>

namespace link {

enum class Band : uint8_t {
    G3 = 0,  ///< 869.4-869.65, primary. Value matches config TLV 0x03.
    G1 = 1,  ///< 868.0-868.6, fallback.
    Count = 2,
};

struct BandPlan {
    uint32_t lowerHz;
    uint32_t upperHz;
    uint32_t defaultHz;
    uint32_t airtimeBudgetMsPerHour;
    int8_t maxTxPowerDbm;
    uint8_t dutyCyclePercent;
};

/// The plan for a band. Out-of-range input yields g3, because defaulting to the
/// band with the tighter power ceiling would be the wrong kind of safe -- the
/// caller would silently transmit on the wrong frequency.
const BandPlan &plan(Band band);

/// Clamp a requested transmit power to what the band and the SX1262 allow.
int8_t clampTxPowerDbm(Band band, int8_t requestedDbm);

/// Whether a centre frequency leaves the whole BW125 channel inside the band.
bool channelFitsInBand(Band band, uint32_t centreHz);

/*
 * Rendezvous configuration (CLAUDE.md 2.5).
 *
 * "There is a fixed rendezvous configuration that never changes: SF9 / BW125 /
 * CR4/5 on the primary channel, with the default sniff interval. It is the floor
 * the link falls back to, and both nodes always accept it."
 *
 * These are constants and not configuration on purpose. A configurable floor is
 * not a floor.
 */
constexpr uint8_t kRendezvousSf = 9;
constexpr Band kRendezvousBand = Band::G3;
constexpr uint32_t kRendezvousSniffIntervalMs = 2000;

/// The SX1262's own output ceiling, independent of any band.
constexpr int8_t kRadioMaxTxPowerDbm = 22;

/// Spreading factor limits (CLAUDE.md 1.4 tabulates SF7..SF12).
constexpr uint8_t kMinSf = 7;
constexpr uint8_t kMaxSf = 12;

/// Sniff interval limits, from the config TLV range in docs/bridge-protocol.md 3.
constexpr uint32_t kMinSniffIntervalMs = 250;
constexpr uint32_t kMaxSniffIntervalMs = 10000;

} // namespace link

#endif // MAXL_LINK_BAND_H
