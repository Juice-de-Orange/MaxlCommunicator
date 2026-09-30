/*
 * Adaptive spreading factor (CLAUDE.md 2.5).
 *
 * The rules, verbatim, because every one of them is here for a reason:
 *
 *   - Start at the rendezvous configuration.
 *   - Escalate one SF step (max SF12) after 2 consecutive failed delivery
 *     attempts, or when the last 5 ACKs show SNR below the demodulation floor
 *     + 3 dB.
 *   - De-escalate one step after 10 consecutive first-attempt successes with SNR
 *     margin above 10 dB.
 *   - Never change SF mid-retry sequence.
 *   - After 3 x beaconInterval with no contact, both sides return to the
 *     rendezvous configuration rather than scanning the SF set.
 *
 * The last one is the important one. Scanning costs receive time at every SF and
 * is exactly what you cannot afford when the link is already bad. A fixed floor
 * that both nodes always accept converges without either side having to search.
 *
 * The asymmetry between escalation (2 failures) and de-escalation (10 successes)
 * is hysteresis: going up is cheap insurance, coming down risks losing the link
 * again. docs/test-plan.md 2.12 checks for oscillation over 30 minutes, and that
 * is the test people skip.
 *
 * The thresholds here are plausible defaults, not measured ones. docs/test-plan.md
 * 2.16 exists to calibrate them against real range data; expect to come back.
 */

#ifndef MAXL_LINK_ADAPTIVE_SF_H
#define MAXL_LINK_ADAPTIVE_SF_H

#include <stdint.h>

#include "band.h"

namespace link {

constexpr uint8_t kEscalateAfterFailures = 2;
constexpr uint8_t kDeescalateAfterSuccesses = 10;
constexpr uint8_t kSnrHistoryLength = 5;

/// Escalate when the recent SNR sits below the demodulation floor plus this.
constexpr int8_t kEscalateMarginDb = 3;

/// De-escalate only with this much margin above the floor.
constexpr int8_t kDeescalateMarginDb = 10;

/// Semtech's demodulation floor per spreading factor at BW125, in whole dB.
int8_t demodulationFloorDb(uint8_t sf);

class AdaptiveSf {
public:
    AdaptiveSf();

    uint8_t currentSf() const { return sf_; }

    /// True while a retry sequence is in flight, during which SF must not move.
    bool locked() const { return retryInProgress_; }

    /// Called when a frame is handed to the radio. `attempt` is 0 for the first
    /// transmission of a message and rises with each retry.
    void onTransmitStart(uint8_t attempt);

    /*
     * A message was delivered. `attempt` is the attempt number that succeeded and
     * `snrDb` is what the peer's ACK reported.
     *
     * Only a first-attempt success counts towards de-escalation: a message that
     * needed a retry is evidence the link is marginal, not that it is good.
     */
    void onDelivered(uint8_t attempt, int8_t snrDb);

    /// A message was given up on after its retries were exhausted.
    void onUndelivered();

    /// Any frame received from the peer, which CLAUDE.md 2.5 treats as an
    /// implicit beacon.
    void onPeerHeard();

    /*
     * Called periodically. If `msSinceLastContact` exceeds 3 x beaconInterval,
     * the configuration returns to the rendezvous floor.
     *
     * Returns true if the SF changed on this call.
     */
    bool tick(uint32_t msSinceLastContact, uint32_t beaconIntervalMs);

    /// Force the rendezvous configuration, as a divergence recovery would.
    void resetToRendezvous();

    // Diagnostics, surfaced on the PEERS screen and over the bridge.
    uint8_t consecutiveFailures() const { return consecutiveFailures_; }
    uint8_t consecutiveFirstAttemptSuccesses() const { return consecutiveSuccesses_; }

private:
    void escalate();
    void deescalate();
    void recordSnr(int8_t snrDb);
    bool recentSnrBelowEscalationThreshold() const;
    bool recentSnrAboveDeescalationThreshold() const;

    uint8_t sf_;
    bool retryInProgress_;
    uint8_t consecutiveFailures_;
    uint8_t consecutiveSuccesses_;

    int8_t snrHistory_[kSnrHistoryLength];
    uint8_t snrCount_;
    uint8_t snrNext_;
};

} // namespace link

#endif // MAXL_LINK_ADAPTIVE_SF_H
