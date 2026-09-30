#include "adaptive_sf.h"

namespace link {

int8_t demodulationFloorDb(uint8_t sf)
{
    /*
     * Semtech's published SNR limits at BW125, rounded to whole dB. These are the
     * point at which demodulation stops working, so the thresholds in CLAUDE.md
     * 2.5 are expressed as a margin above them rather than as absolute numbers --
     * -10 dB is a comfortable link at SF12 and a dead one at SF7.
     */
    switch (sf) {
    case 7:
        return -7;
    case 8:
        return -10;
    case 9:
        return -12;
    case 10:
        return -15;
    case 11:
        return -17;
    case 12:
    default:
        return -20;
    }
}

AdaptiveSf::AdaptiveSf()
    : sf_(kRendezvousSf), retryInProgress_(false), consecutiveFailures_(0),
      consecutiveSuccesses_(0), snrHistory_{}, snrCount_(0), snrNext_(0)
{
}

void AdaptiveSf::resetToRendezvous()
{
    sf_ = kRendezvousSf;
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    snrCount_ = 0;
    snrNext_ = 0;
    retryInProgress_ = false;
}

void AdaptiveSf::recordSnr(int8_t snrDb)
{
    if (snrCount_ < kSnrHistoryLength) {
        snrHistory_[snrCount_++] = snrDb;
    } else {
        snrHistory_[snrNext_] = snrDb;
        snrNext_ = static_cast<uint8_t>((snrNext_ + 1u) % kSnrHistoryLength);
    }
}

bool AdaptiveSf::recentSnrBelowEscalationThreshold() const
{
    // "the last 5 ACKs" -- fewer than five is not yet evidence.
    if (snrCount_ < kSnrHistoryLength) {
        return false;
    }
    const int8_t threshold = static_cast<int8_t>(demodulationFloorDb(sf_) + kEscalateMarginDb);
    for (uint8_t i = 0; i < kSnrHistoryLength; ++i) {
        if (snrHistory_[i] >= threshold) {
            return false;
        }
    }
    return true;
}

bool AdaptiveSf::recentSnrAboveDeescalationThreshold() const
{
    if (snrCount_ == 0) {
        return false;
    }
    /*
     * The margin is judged against the floor of the SF we would drop TO, not the
     * one we are on. Dropping a step costs roughly 2-3 dB of link budget, and
     * measuring against the current floor would let the link fall off a cliff it
     * had 10 dB of margin above.
     */
    const uint8_t targetSf = (sf_ > kMinSf) ? static_cast<uint8_t>(sf_ - 1u) : kMinSf;
    const int8_t threshold =
        static_cast<int8_t>(demodulationFloorDb(targetSf) + kDeescalateMarginDb);
    for (uint8_t i = 0; i < snrCount_; ++i) {
        if (snrHistory_[i] < threshold) {
            return false;
        }
    }
    return true;
}

void AdaptiveSf::escalate()
{
    if (sf_ < kMaxSf) {
        ++sf_;
    }
    // Whether or not the step happened, the evidence has been acted on. Leaving
    // the counters up at SF12 would make the next failure escalate instantly on
    // the way back down.
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    snrCount_ = 0;
    snrNext_ = 0;
}

void AdaptiveSf::deescalate()
{
    if (sf_ > kMinSf) {
        --sf_;
    }
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    snrCount_ = 0;
    snrNext_ = 0;
}

void AdaptiveSf::onTransmitStart(uint8_t attempt)
{
    // "Never change SF mid-retry sequence." The lock goes on at the first retry
    // and comes off when the message reaches an outcome.
    retryInProgress_ = (attempt > 0);
}

void AdaptiveSf::onDelivered(uint8_t attempt, int8_t snrDb)
{
    retryInProgress_ = false;
    consecutiveFailures_ = 0;
    recordSnr(snrDb);

    if (attempt == 0) {
        if (consecutiveSuccesses_ < 255) {
            ++consecutiveSuccesses_;
        }
    } else {
        // A delivery that needed a retry is not evidence of a good link.
        consecutiveSuccesses_ = 0;
    }

    if (recentSnrBelowEscalationThreshold()) {
        escalate();
        return;
    }

    if (consecutiveSuccesses_ >= kDeescalateAfterSuccesses &&
        recentSnrAboveDeescalationThreshold()) {
        deescalate();
    }
}

void AdaptiveSf::onUndelivered()
{
    retryInProgress_ = false;
    consecutiveSuccesses_ = 0;
    if (consecutiveFailures_ < 255) {
        ++consecutiveFailures_;
    }
    if (consecutiveFailures_ >= kEscalateAfterFailures) {
        escalate();
    }
}

void AdaptiveSf::onPeerHeard()
{
    // CLAUDE.md 2.5: "treat any received frame as an implicit beacon". Contact
    // timing is the caller's; all this does is note that the peer is alive.
}

bool AdaptiveSf::tick(uint32_t msSinceLastContact, uint32_t beaconIntervalMs)
{
    if (beaconIntervalMs == 0) {
        return false;
    }
    // Guard the multiplication rather than the comparison: 3 x a large interval
    // would otherwise wrap and make the timeout fire immediately.
    const uint64_t limit = static_cast<uint64_t>(beaconIntervalMs) * 3u;
    if (static_cast<uint64_t>(msSinceLastContact) < limit) {
        return false;
    }
    if (sf_ == kRendezvousSf) {
        return false;
    }
    resetToRendezvous();
    return true;
}

} // namespace link
