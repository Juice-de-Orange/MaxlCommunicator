/*
 * Stop-and-wait ARQ with budget-aware scheduling (CLAUDE.md 2.4).
 *
 * The sequence:
 *   1. Sender transmits with ACK_REQ set.
 *   2. Receiver validates, dedupes, delivers, and ACKs -- if its own budget
 *      allows. A budget-blocked ACK is queued.
 *   3. The retry timer is 2 x expectedAirtime(SF) + 300 ms, but the retry is
 *      scheduled at max(timerExpiry, budget.earliestLegalTx()). The budget always
 *      wins.
 *   4. Up to 3 retries with randomised backoff (base x 2^attempt +- 25 % jitter),
 *      each subject to the same scheduling.
 *   5. After 3 failed attempts the frame is marked undelivered and surfaced in
 *      the UI. It is not silently dropped.
 *
 * Point 3 is the whole reason this module exists rather than a plain timer.
 * REVIEW.md A3: at SF12 the retry timer is 3.6 s but a single frame locks g1 for
 * 163 s, so every retry would be blocked by a budget the timer knew nothing
 * about, and the state machine had no defined behaviour for that.
 *
 * The three outcomes are kept distinct because they mean different things to the
 * user (CLAUDE.md 2.4): `queued until HH:MM` is the budget, `in flight` is
 * retrying, `undelivered` is gave up.
 *
 * Broadcast frames never request an ACK, so they pass straight through.
 *
 * No timers, no threads, no clock reads scattered about: the caller drives this
 * with poll(nowMs) and acts on what it returns. That is what makes the whole
 * retry schedule testable in simulated time.
 */

#ifndef MAXL_LINK_ARQ_H
#define MAXL_LINK_ARQ_H

#include <stddef.h>
#include <stdint.h>

#include "band.h"
#include "budget.h"
#include "frame.h"

namespace link {

/// "Up to 3 retries" -- so four transmissions in total.
constexpr uint8_t kMaxRetries = 3;

/// The fixed part of the retry timer, CLAUDE.md 2.4.
constexpr uint32_t kRetryTimerSlackMs = 300;

/// Jitter applied to the backoff, in per cent either way.
constexpr uint8_t kBackoffJitterPercent = 25;

/// Messages awaiting delivery. Stop-and-wait means one is in flight at a time;
/// the rest are queued behind it.
constexpr size_t kMaxOutstanding = 8;

/// What the user is shown (CLAUDE.md 2.4).
enum class DeliveryState : uint8_t {
    Idle = 0,
    Queued,       ///< waiting for the budget; releaseUnix says until when
    InFlight,     ///< transmitted, waiting for an ACK or the retry timer
    Delivered,
    Undelivered,  ///< retries exhausted -- surfaced, never silently dropped
};

/// What poll() is telling the caller to do.
enum class ArqAction : uint8_t {
    Nothing = 0,
    Transmit,     ///< hand `frame` to the radio now
    Deliver,      ///< a message reached a terminal state; read it and clear it
};

struct ArqOutcome {
    ArqAction action;
    size_t slot;
    DeliveryState state;
    /// For Queued: the wall-clock second the UI should show. Zero if unknown.
    uint32_t releaseUnix;
};

/*
 * A pseudo-random source for the backoff jitter.
 *
 * An interface rather than rand(): CLAUDE.md 2.4 asks for randomised backoff, and
 * a test that cannot pin the randomness cannot check the schedule. On the device
 * this is seeded from the FICR device ID so two nodes do not retry in lockstep.
 */
class IJitterSource {
public:
    virtual ~IJitterSource() = default;
    /// Uniform in [0, bound). bound is never zero.
    virtual uint32_t next(uint32_t bound) = 0;
};

class Arq {
public:
    Arq();

    void begin(DutyCycleBudget &budget, IJitterSource &jitter);

    /*
     * Queue a message for transmission.
     *
     * `frameLen` is the whole on-air frame. `ackRequested` is false for broadcast
     * -- CLAUDE.md 2.4: "Broadcast frames never request an ACK."
     *
     * Returns the slot, or kMaxOutstanding if the queue is full.
     */
    size_t submit(const uint8_t *frame, size_t frameLen, uint8_t seq, uint8_t sf, Band band,
                  bool ackRequested, uint32_t nowMs);

    /// Drive the machine. Call often; it does nothing until something is due.
    ArqOutcome poll(uint32_t nowMs);

    /// The radio finished sending what poll() handed it.
    void onTransmitComplete(size_t slot, uint32_t nowMs, bool success);

    /// An ACK arrived for this sequence number.
    void onAckReceived(uint8_t seq, uint32_t nowMs);

    /// The slot whose unresolved message carries this seq, or kMaxOutstanding.
    /// Lets the caller check the ACK's sender against the message's destination
    /// BEFORE onAckReceived marks anything delivered.
    size_t slotOfActiveSeq(uint8_t seq) const;

    /*
     * Swap the frame bytes of a slot for a rebuilt copy.
     *
     * A retry must be rebuilt, not replayed: the receiver's replay window judges
     * a byte-identical retransmission -- same counter -- as an attack replay and
     * never re-ACKs it. Only the seq history recognises a retry, and that needs
     * the same seq under a FRESH counter (link/replay.h). The payload is
     * unchanged, so the length must match what was submitted.
     */
    bool replaceFrame(size_t slot, const uint8_t *frame, size_t frameLen);

    DeliveryState stateOf(size_t slot) const;
    uint8_t attemptsOf(size_t slot) const;

    /// Whether that slot's frame carried ACK_REQ. The sender opens its
    /// continuous receive window only for frames that are waiting for an answer
    /// -- a broadcast never asks for one (CLAUDE.md 2.4).
    bool ackRequestedOf(size_t slot) const;
    uint32_t releaseUnixOf(size_t slot) const;

    /// Read back the frame bytes of a slot, for the caller to transmit.
    const uint8_t *frameOf(size_t slot, size_t *lenOut) const;

    /// Release a slot the caller has finished reporting on.
    void clear(size_t slot);

    size_t pending() const;

private:
    struct Message {
        bool used;
        uint8_t frame[kMaxFrameBytes];
        size_t frameLen;
        uint8_t seq;
        uint8_t sf;
        Band band;
        bool ackRequested;
        uint8_t attempts;      ///< transmissions so far
        DeliveryState state;
        uint32_t dueMs;        ///< earliest monotonic time for the next attempt
        uint32_t releaseUnix;  ///< what the UI shows while Queued
        bool awaitingRadio;    ///< handed to the radio, completion not yet reported
    };

    uint32_t retryTimerMs(const Message &message) const;
    uint32_t backoffMs(const Message &message);

    /// Set the retry deadline for a message whose attempt just went unanswered.
    /// Called once per attempt -- rolling a fresh backoff on every poll would
    /// postpone the retry for ever.
    void armRetryTimer(Message &message, uint32_t nowMs);

    /// How long the budget still wants this message to wait, in milliseconds.
    /// Asked afresh on every poll, because the rolling window moves.
    uint32_t budgetGate(Message &message, uint32_t nowMs);

    DutyCycleBudget *budget_;
    IJitterSource *jitter_;
    Message messages_[kMaxOutstanding];
    /// Stop-and-wait: at most one message is on the air at a time.
    size_t inFlightSlot_;
};

} // namespace link

#endif // MAXL_LINK_ARQ_H
