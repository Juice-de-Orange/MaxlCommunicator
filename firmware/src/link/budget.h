/*
 * The duty cycle budget (CLAUDE.md 1.2).
 *
 * "The duty cycle is legally binding and is enforced in firmware. The budget
 * tracker maintains a rolling 60-minute airtime total per sub-band, per device,
 * and MUST refuse to transmit when the budget is exhausted. It is not a
 * configurable option and is not exposed as a user setting. A blocked
 * transmission is queued with a scheduled release time, never dropped."
 *
 * TWO CONSTRAINTS, and a transmission must satisfy both (decision D9):
 *
 *   the hourly total   at most 360 s (g3) or 36 s (g1) in any rolling 60 minutes
 *   the frame lockout  after a frame of airtime A, silence for A * (1/dc - 1),
 *                      measured from the end of that transmission
 *
 * CLAUDE.md 1.2 specifies the first and 1.4 tabulates the second; earliestLegalTx
 * returns the later of the two. Together they are strictly more conservative than
 * either alone, and the lockout is what stops the hourly total being spent as one
 * burst -- 1590 frames back to back at SF9 in g3, then 54 minutes of silence,
 * would satisfy the hourly rule and be useless as a communicator.
 *
 * Three properties this module exists to hold, each of which was a finding in
 * REVIEW.md before it was a requirement:
 *
 *  1. It survives reboot (A4). The ring is on flash and is reconstructed against
 *     the RTC at boot. A budget a power cycle can clear is not a budget.
 *  2. With no valid time, the node is fully transmit-blocked. Not "assume empty",
 *     not "assume full" -- blocked, until SET_TIME or a GNSS fix.
 *  3. It answers "when may I next transmit", not just "may I now" (A3). At SF12
 *     in g1 a single frame locks the band for 163 seconds, so every retry would
 *     otherwise be blocked by a budget the ARQ layer could not reason about.
 *
 * There is deliberately no way to raise, reset or disable the limit. Not a TLV
 * (docs/bridge-protocol.md 3 says so explicitly), not a compile flag, not a
 * parameter. Anything that transmits goes through here, and there is no second
 * path (CLAUDE.md 6).
 *
 * Storage layout, 10 bytes per record:
 *
 *   bytes 0-3  unixSeconds : u32   when the transmission started
 *   bytes 4-7  airtimeUs   : u32   what it cost
 *   byte  8    band        : u8
 *   byte  9    check       : u8    xor of the preceding nine bytes, plus 0x5A
 */

#ifndef MAXL_LINK_BUDGET_H
#define MAXL_LINK_BUDGET_H

#include <stddef.h>
#include <stdint.h>

#include "band.h"
#include "hal/i_block_store.h"
#include "hal/i_clock.h"

namespace link {

constexpr uint32_t kBudgetWindowSeconds = 3600;
constexpr size_t kBudgetRecordBytes = 10;

/*
 * How many transmissions the window can hold.
 *
 * The realistic worst case is bounded by the design, not by the allowance. A
 * frame sent to a duty-cycled receiver carries a preamble spanning the sniff
 * interval (CLAUDE.md 2.3), which at the default costs 2196 ms and caps the rate
 * at 163 per hour. ACKs are short -- they go to a receiver already listening --
 * but a node only ACKs what it receives, so their rate is bounded by the
 * incoming frame rate. Call it 350 an hour for a busy two-node link; 1024 is
 * threefold headroom on that.
 *
 * The theoretical floor is much lower: an ACK at SF7 costs 57 ms, so the hourly
 * allowance alone would permit about 6300. A ring that size would cost 76 KB of
 * RAM to defend against traffic this firmware cannot generate.
 *
 * When the ring is full the node refuses to transmit until the oldest record
 * expires, exactly as if the budget were exhausted. That is the conservative
 * direction: an unrecorded transmission is an uncounted one.
 */
constexpr size_t kMaxBudgetRecords = 1024;

/// Returned by earliestLegalTxUnix when the frame can never be legal, which
/// means its airtime alone exceeds the hourly allowance.
constexpr uint32_t kNeverLegal = UINT32_MAX;
constexpr uint32_t kNeverLegalDelay = UINT32_MAX;

enum class BudgetState : uint8_t {
    Ready = 0,
    NoTime,       ///< RTC invalid -- fully transmit-blocked (CLAUDE.md 1.2)
    StorageFault, ///< the ring could not be persisted; blocked until reboot
};
// There is deliberately no "Exhausted" state. Whether the budget is spent depends
// on the band and on the size of the frame you are asking about, so it is
// answered by canTransmit() and earliestLegalTx(), not by a global flag.

class DutyCycleBudget {
public:
    DutyCycleBudget();

    /*
     * Reconstruct the budget against the clock.
     *
     * Returns false if the store is unusable. A false return does NOT mean an
     * empty budget -- it means blocked, which is what state() will report.
     */
    bool begin(hal::IBlockStore &store, hal::IClock &clock);

    BudgetState state() const;

    /// Whether a frame of this airtime may be sent right now.
    bool canTransmit(Band band, uint32_t airtimeUs) const;

    /*
     * How long to wait before a frame of this airtime becomes legal, in
     * milliseconds from now. Zero means now; kNeverLegalDelay means never.
     *
     * This is what the ARQ scheduler uses. CLAUDE.md 2.4 schedules a retry at
     * max(timerExpiry, budget.earliestLegalTx()) -- the budget always wins -- and
     * the scheduler works in monotonic milliseconds, so it needs the answer in
     * milliseconds rather than in whole seconds.
     */
    uint32_t earliestLegalTxDelayMs(Band band, uint32_t airtimeUs) const;

    /*
     * The same answer as a wall-clock second, for the UI and for EVT_BUDGET's
     * nextTxUnix (docs/bridge-protocol.md 4). This is the "queued until HH:MM"
     * that CLAUDE.md 2.4 requires the user to see.
     *
     * Rounded up, so it never names a second at which transmission would still
     * be illegal. Returns kNeverLegal if the frame's own airtime exceeds the
     * hourly allowance, and also when there is no valid time -- with no clock
     * there is no answer, and the caller must resolve that first.
     */
    uint32_t earliestLegalTxUnix(Band band, uint32_t airtimeUs) const;

    /*
     * Charge a completed transmission to the budget and persist it.
     *
     * Call at the START of a transmission if you can; calling afterwards records
     * a timestamp up to one frame late, which shifts the expiry later and is
     * therefore conservative rather than permissive.
     *
     * Returns false if the record could not be persisted, which latches
     * StorageFault: the airtime was spent but a reboot would forget it, and a
     * budget a power cycle can clear is not a budget.
     */
    bool recordTransmission(Band band, uint32_t airtimeUs);

    /// Airtime already spent in the current window, in milliseconds. This is what
    /// EVT_BUDGET reports (docs/bridge-protocol.md 4).
    uint32_t usedMs(Band band) const;

    /// Remaining allowance in the current window, in milliseconds.
    uint32_t remainingMs(Band band) const;

    /// Records currently held in the window. Diagnostics only.
    size_t recordCount() const { return count_; }

private:
    struct Entry {
        uint32_t unixSeconds;
        uint32_t airtimeUs;
        Band band;
    };

    void purgeExpired(uint32_t nowUnix);
    uint64_t usedUs(Band band, uint32_t nowUnix) const;
    bool appendEntry(const Entry &entry);
    uint32_t hourlyDelayMs(Band band, uint32_t airtimeUs) const;
    /// Records still inside the window. Expired ones still occupy the array
    /// until the next purge, so capacity must be judged on the live count.
    size_t liveCount(uint32_t nowUnix) const;
    uint32_t lockoutDelayMs(Band band) const;
    void noteLockout(Band band, uint32_t airtimeUs, uint32_t nowUnix, uint32_t nowMonotonicMs);

    hal::IBlockStore *store_;
    hal::IClock *clock_;
    bool storageFault_;

    /*
     * The frame lockout, tracked twice on purpose.
     *
     * While the node is running, monotonic milliseconds give the exact release
     * moment -- at SF7 in g3 the lockout is 0.6 s, and whole seconds cannot
     * express that. Across a reboot only the persisted record survives, and its
     * timestamp has the RTC's one-second resolution, so the release second is
     * rounded up. Rounding up waits slightly too long, which is the safe
     * direction; rounding down would transmit slightly too early, which is the
     * one thing this module exists to prevent.
     */
    uint32_t lockoutUntilMonotonicMs_[static_cast<uint8_t>(Band::Count)];
    bool lockoutMonotonicValid_[static_cast<uint8_t>(Band::Count)];
    uint32_t lockoutUntilUnix_[static_cast<uint8_t>(Band::Count)];

    // Oldest first. Kept sorted by construction: entries are appended with a
    // non-decreasing timestamp and expiry removes from the front.
    Entry entries_[kMaxBudgetRecords];
    size_t count_;
};

} // namespace link

#endif // MAXL_LINK_BUDGET_H
