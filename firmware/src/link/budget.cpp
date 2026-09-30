#include "budget.h"

namespace link {
namespace {

constexpr hal::StoreRegion kRegion = hal::StoreRegion::BudgetRing;
constexpr uint8_t kCheckSeed = 0x5Au;

uint8_t checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = kCheckSeed;
    for (size_t i = 0; i < len; ++i) {
        sum = static_cast<uint8_t>(sum ^ bytes[i]);
    }
    return sum;
}

void put32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

} // namespace

DutyCycleBudget::DutyCycleBudget()
    : store_(nullptr), clock_(nullptr), storageFault_(false),
      lockoutUntilMonotonicMs_{}, lockoutMonotonicValid_{}, lockoutUntilUnix_{},
      entries_{}, count_(0)
{
}

uint32_t DutyCycleBudget::lockoutDelayMs(Band band) const
{
    const uint8_t index = static_cast<uint8_t>(band);
    uint32_t delayMs = 0;

    if (lockoutMonotonicValid_[index]) {
        /*
         * This session armed the lockout itself, so the monotonic form is exact
         * and is the answer. The wall-clock form is deliberately NOT consulted
         * here: it is rounded up to a whole second, and taking the later of the
         * two would impose that rounding on a running node that does not need it.
         */
        const uint32_t now = clock_->monotonicMs();
        if (static_cast<int32_t>(lockoutUntilMonotonicMs_[index] - now) > 0) {
            delayMs = lockoutUntilMonotonicMs_[index] - now;
        }
        return delayMs;
    }

    // The reboot path: nothing survives but the persisted record, so the lockout
    // is reconstructed at the RTC's one-second resolution and rounded up.
    if (lockoutUntilUnix_[index] != 0 && clock_->timeValid()) {
        const uint32_t nowUnix = clock_->unixSeconds();
        if (lockoutUntilUnix_[index] > nowUnix) {
            delayMs = (lockoutUntilUnix_[index] - nowUnix) * 1000u;
        }
    }
    return delayMs;
}

void DutyCycleBudget::noteLockout(Band band, uint32_t airtimeUs, uint32_t nowUnix,
                                  uint32_t nowMonotonicMs)
{
    const uint8_t index = static_cast<uint8_t>(band);
    const uint32_t duty = plan(band).dutyCyclePercent;

    /*
     * Off time is measured from the END of the transmission, so counting from
     * its start the release is at start + airtime / dutyCycle:
     *
     *   end + airtime * (1/dc - 1) = start + airtime + airtime * (1/dc - 1)
     *                              = start + airtime / dc
     */
    const uint64_t releaseAfterStartUs = (static_cast<uint64_t>(airtimeUs) * 100u) / duty;

    const uint64_t releaseMs = static_cast<uint64_t>(nowMonotonicMs) +
                               ((releaseAfterStartUs + 999u) / 1000u);
    lockoutUntilMonotonicMs_[index] = static_cast<uint32_t>(releaseMs);
    lockoutMonotonicValid_[index] = true;

    // Round the wall-clock form up to the next whole second: it is the form that
    // survives a reboot and it must never name a second that is still illegal.
    const uint32_t releaseSeconds =
        static_cast<uint32_t>((releaseAfterStartUs + 999999u) / 1000000u);
    lockoutUntilUnix_[index] = nowUnix + releaseSeconds;
}

bool DutyCycleBudget::begin(hal::IBlockStore &store, hal::IClock &clock)
{
    store_ = &store;
    clock_ = &clock;
    storageFault_ = false;
    count_ = 0;

    if (store.recordSize(kRegion) != kBudgetRecordBytes) {
        storageFault_ = true;
        return false;
    }

    const size_t records = store.count(kRegion);
    for (size_t i = 0; i < records; ++i) {
        uint8_t raw[kBudgetRecordBytes];
        if (store.read(kRegion, i, raw, sizeof(raw)) != hal::StoreResult::Ok) {
            continue;
        }
        if (checksum(raw, kBudgetRecordBytes - 1) != raw[kBudgetRecordBytes - 1]) {
            continue;  // torn or never written
        }
        const uint8_t rawBand = raw[8];
        if (rawBand >= static_cast<uint8_t>(Band::Count)) {
            continue;
        }

        Entry entry{};
        entry.unixSeconds = get32(raw + 0);
        entry.airtimeUs = get32(raw + 4);
        entry.band = static_cast<Band>(rawBand);

        if (count_ < kMaxBudgetRecords) {
            entries_[count_++] = entry;
        } else {
            /*
             * More unexpired records than the ring can hold. This cannot arise
             * from this firmware's own behaviour -- it refuses to transmit once
             * the ring is full -- so it means foreign or corrupt data. Keeping
             * the newest and dropping the oldest leaves the ring full, and a full
             * ring already refuses to transmit until the oldest expires. The
             * conservative outcome falls out of the existing rule.
             */
            for (size_t j = 1; j < kMaxBudgetRecords; ++j) {
                entries_[j - 1] = entries_[j];
            }
            entries_[kMaxBudgetRecords - 1] = entry;
        }
    }

    /*
     * Only purge if the clock is actually valid. With no time we cannot know what
     * has expired, and guessing in the permissive direction is exactly the hole
     * CLAUDE.md 1.2 closes -- so the records stay and state() reports NoTime.
     */
    if (clock.timeValid()) {
        purgeExpired(clock.unixSeconds());
    }

    /*
     * Reconstruct the frame lockout from the newest surviving record of each
     * band. Without this a node could shorten a 163-second lockout in g1 by
     * rebooting, which is the same hole REVIEW.md A4 found in the hourly total.
     * Monotonic milliseconds mean nothing across a reboot, so only the
     * second-resolution form is restored.
     */
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        const uint8_t index = static_cast<uint8_t>(entry.band);
        const uint32_t duty = plan(entry.band).dutyCyclePercent;
        const uint64_t releaseAfterStartUs = (static_cast<uint64_t>(entry.airtimeUs) * 100u) / duty;
        const uint32_t releaseAt =
            entry.unixSeconds + static_cast<uint32_t>((releaseAfterStartUs + 999999u) / 1000000u);
        if (releaseAt > lockoutUntilUnix_[index]) {
            lockoutUntilUnix_[index] = releaseAt;
        }
    }

    return true;
}

void DutyCycleBudget::purgeExpired(uint32_t nowUnix)
{
    size_t keep = 0;
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        /*
         * A record counts while now - t < 3600. Records timestamped in the future
         * are kept, not discarded: the clock may have been corrected backwards,
         * and airtime that was really spent does not stop having been spent.
         */
        const bool expired = (nowUnix >= entry.unixSeconds) &&
                             ((nowUnix - entry.unixSeconds) >= kBudgetWindowSeconds);
        if (!expired) {
            entries_[keep++] = entry;
        }
    }
    count_ = keep;
}

uint64_t DutyCycleBudget::usedUs(Band band, uint32_t nowUnix) const
{
    uint64_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        if (entry.band != band) {
            continue;
        }
        const bool expired = (nowUnix >= entry.unixSeconds) &&
                             ((nowUnix - entry.unixSeconds) >= kBudgetWindowSeconds);
        if (!expired) {
            total += entry.airtimeUs;
        }
    }
    return total;
}

BudgetState DutyCycleBudget::state() const
{
    if (storageFault_ || store_ == nullptr || clock_ == nullptr) {
        return BudgetState::StorageFault;
    }
    if (!clock_->timeValid()) {
        return BudgetState::NoTime;
    }
    return BudgetState::Ready;
}

bool DutyCycleBudget::canTransmit(Band band, uint32_t airtimeUs) const
{
    return earliestLegalTxDelayMs(band, airtimeUs) == 0;
}

size_t DutyCycleBudget::liveCount(uint32_t nowUnix) const
{
    size_t live = 0;
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        const bool expired = (nowUnix >= entry.unixSeconds) &&
                             ((nowUnix - entry.unixSeconds) >= kBudgetWindowSeconds);
        if (!expired) {
            ++live;
        }
    }
    return live;
}

uint32_t DutyCycleBudget::hourlyDelayMs(Band band, uint32_t airtimeUs) const
{
    const uint64_t limitUs = static_cast<uint64_t>(plan(band).airtimeBudgetMsPerHour) * 1000u;
    if (airtimeUs > limitUs) {
        return kNeverLegalDelay;  // one frame cannot fit in an hour's allowance
    }

    const uint32_t now = clock_->unixSeconds();
    uint64_t used = usedUs(band, now);
    // Expired records still sit in the array until the next purge, so capacity
    // must be judged on the live count -- otherwise the ring reads as full while
    // most of it is about to be reclaimed.
    const bool ringFull = liveCount(now) >= kMaxBudgetRecords;

    if (!ringFull && used + airtimeUs <= limitUs) {
        return 0;
    }

    /*
     * Walk the records oldest first, retiring them one at a time. When record k
     * leaves the window -- at k.unixSeconds + 3600 -- every record at or before
     * it has gone too, so the remaining total is what is left after subtracting
     * them. The first moment at which the frame fits is the answer.
     *
     * Entries of other bands still occupy ring slots, so they are stepped over
     * for the airtime sum but still count towards freeing a slot.
     */
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        if (entry.band == band) {
            const bool counted = !((now >= entry.unixSeconds) &&
                                   ((now - entry.unixSeconds) >= kBudgetWindowSeconds));
            if (counted) {
                used -= entry.airtimeUs;
            }
        }
        const uint32_t releaseAt = entry.unixSeconds + kBudgetWindowSeconds;
        // One slot has been freed by this point, so the ring is no longer full.
        if (used + airtimeUs <= limitUs) {
            return (releaseAt > now) ? ((releaseAt - now) * 1000u) : 0u;
        }
    }

    // Every record has been retired and it still does not fit. Only reachable if
    // the airtime exceeds the allowance, which was checked above.
    return 0;
}

uint32_t DutyCycleBudget::earliestLegalTxDelayMs(Band band, uint32_t airtimeUs) const
{
    if (state() != BudgetState::Ready) {
        return kNeverLegalDelay;
    }

    const uint32_t hourly = hourlyDelayMs(band, airtimeUs);
    if (hourly == kNeverLegalDelay) {
        return kNeverLegalDelay;
    }
    const uint32_t lockout = lockoutDelayMs(band);

    // Decision D9: both constraints apply, and the later one wins.
    return (lockout > hourly) ? lockout : hourly;
}

uint32_t DutyCycleBudget::earliestLegalTxUnix(Band band, uint32_t airtimeUs) const
{
    const uint32_t delayMs = earliestLegalTxDelayMs(band, airtimeUs);
    if (delayMs == kNeverLegalDelay) {
        return kNeverLegal;
    }
    // Round up: never name a second at which transmission would still be illegal.
    return clock_->unixSeconds() + ((delayMs + 999u) / 1000u);
}

bool DutyCycleBudget::appendEntry(const Entry &entry)
{
    uint8_t raw[kBudgetRecordBytes];
    put32(raw + 0, entry.unixSeconds);
    put32(raw + 4, entry.airtimeUs);
    raw[8] = static_cast<uint8_t>(entry.band);
    raw[9] = checksum(raw, kBudgetRecordBytes - 1);

    hal::StoreResult result = store_->append(kRegion, raw, sizeof(raw));
    if (result == hal::StoreResult::Full) {
        /*
         * The flash region filled up while the in-RAM window still has room,
         * which means it holds expired records. Rewrite it from the live window.
         */
        if (store_->erase(kRegion) != hal::StoreResult::Ok) {
            return false;
        }
        for (size_t i = 0; i < count_; ++i) {
            uint8_t rewritten[kBudgetRecordBytes];
            put32(rewritten + 0, entries_[i].unixSeconds);
            put32(rewritten + 4, entries_[i].airtimeUs);
            rewritten[8] = static_cast<uint8_t>(entries_[i].band);
            rewritten[9] = checksum(rewritten, kBudgetRecordBytes - 1);
            if (store_->append(kRegion, rewritten, sizeof(rewritten)) != hal::StoreResult::Ok) {
                return false;
            }
        }
        result = store_->append(kRegion, raw, sizeof(raw));
    }
    if (result != hal::StoreResult::Ok) {
        return false;
    }

    // Not recorded until it is on flash. Everything else here is bookkeeping.
    return store_->sync(kRegion) == hal::StoreResult::Ok;
}

bool DutyCycleBudget::recordTransmission(Band band, uint32_t airtimeUs)
{
    if (store_ == nullptr || clock_ == nullptr) {
        return false;
    }
    if (!clock_->timeValid()) {
        // Nothing should have transmitted in this state, but if something did,
        // recording it against an unknown time would corrupt the window.
        return false;
    }

    const uint32_t now = clock_->unixSeconds();
    purgeExpired(now);

    if (count_ >= kMaxBudgetRecords) {
        storageFault_ = true;
        return false;
    }

    Entry entry{};
    entry.unixSeconds = now;
    entry.airtimeUs = airtimeUs;
    entry.band = band;
    entries_[count_++] = entry;

    noteLockout(band, airtimeUs, now, clock_->monotonicMs());

    if (!appendEntry(entry)) {
        /*
         * The airtime was spent but a reboot would forget it. Latch: the session
         * keeps accounting in RAM, and no further transmission is permitted,
         * because from here on the budget is exactly as bypassable as the one
         * REVIEW.md A4 called a compliance hole.
         */
        storageFault_ = true;
        return false;
    }
    return true;
}

uint32_t DutyCycleBudget::usedMs(Band band) const
{
    if (clock_ == nullptr || !clock_->timeValid()) {
        return 0;
    }
    return static_cast<uint32_t>(usedUs(band, clock_->unixSeconds()) / 1000u);
}

uint32_t DutyCycleBudget::remainingMs(Band band) const
{
    const uint32_t limit = plan(band).airtimeBudgetMsPerHour;
    const uint32_t used = usedMs(band);
    return (used >= limit) ? 0u : (limit - used);
}

} // namespace link
