#include "arq.h"

#include "airtime.h"

namespace link {
namespace {

/// Compare monotonic millisecond timestamps in a way that survives the 49-day
/// wrap. Signed difference, not `a >= b`.
bool reached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

} // namespace

Arq::Arq() : budget_(nullptr), jitter_(nullptr), messages_{}, inFlightSlot_(kMaxOutstanding)
{
}

void Arq::begin(DutyCycleBudget &budget, IJitterSource &jitter)
{
    budget_ = &budget;
    jitter_ = &jitter;
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        messages_[i] = Message{};
    }
    inFlightSlot_ = kMaxOutstanding;
}

uint32_t Arq::retryTimerMs(const Message &message) const
{
    // CLAUDE.md 2.4: 2 x expectedAirtime(SF) + 300 ms. The expected airtime is
    // the frame's own, which already carries whatever preamble the sniff interval
    // demanded -- an ACK comes back with a short preamble, so this is generous,
    // and generous is the right direction for a timer that triggers retries.
    const uint32_t airtimeMs =
        timeOnAirMs(message.sf, static_cast<uint8_t>(message.frameLen), kStandardPreambleSymbols);
    return 2u * airtimeMs + kRetryTimerSlackMs;
}

uint32_t Arq::backoffMs(const Message &message)
{
    /*
     * base x 2^attempt +- 25 % jitter, where the exponent counts RETRIES, not
     * transmissions. `attempts` has already been incremented for the attempt
     * that just went unanswered, so the first retry waits one plain `base` --
     * which is what CLAUDE.md 2.4 means by "the retry timer is 2 x
     * expectedAirtime(SF) + 300 ms". Using `attempts` directly would double the
     * very first wait and that sentence would describe nothing.
     */
    const uint32_t base = retryTimerMs(message);
    const uint8_t exponent = (message.attempts > 0) ? static_cast<uint8_t>(message.attempts - 1u)
                                                    : 0u;
    uint32_t scaled = base;
    for (uint8_t i = 0; i < exponent && i < 8; ++i) {
        scaled *= 2u;
    }

    const uint32_t spread = (scaled * kBackoffJitterPercent) / 100u;
    if (spread == 0 || jitter_ == nullptr) {
        return scaled;
    }
    // Uniform across [scaled - spread, scaled + spread].
    const uint32_t offset = jitter_->next(2u * spread + 1u);
    return scaled - spread + offset;
}

void Arq::armRetryTimer(Message &message, uint32_t nowMs)
{
    /*
     * Called ONCE per attempt, when that attempt finishes without an ACK. It must
     * not be recomputed on every poll: rolling a fresh backoff each time the due
     * time arrives would push the retry forward for ever and the message would
     * never reach a terminal state.
     */
    message.dueMs = nowMs + backoffMs(message);
    message.state = DeliveryState::InFlight;
    message.releaseUnix = 0;
}

uint32_t Arq::budgetGate(Message &message, uint32_t nowMs)
{
    (void)nowMs;  // the budget answers in a delay, not against our clock

    /*
     * The budget always wins (CLAUDE.md 2.4, REVIEW.md A3). This is asked afresh
     * on every poll -- unlike the retry timer -- because the answer genuinely
     * changes as the rolling window advances, and asking again is what turns
     * "blocked" into "queued until HH:MM" rather than into a failure.
     */
    if (budget_ == nullptr) {
        return 0;
    }

    const uint32_t airtimeUs = timeOnAirUs(
        message.sf, static_cast<uint8_t>(message.frameLen), kStandardPreambleSymbols);
    const uint32_t delayMs = budget_->earliestLegalTxDelayMs(message.band, airtimeUs);

    if (delayMs == kNeverLegalDelay) {
        /*
         * The band is unusable for this frame -- no valid time, a storage fault,
         * or an airtime that cannot fit in an hour. That is not a retry problem,
         * so no attempt is burned on it: the message waits and the UI says why.
         */
        message.state = DeliveryState::Queued;
        message.releaseUnix = 0;
        return 1000u;  // ask again in a second
    }

    if (delayMs > 0) {
        // Waiting on the budget is a different thing from retrying, and the user
        // is shown a different thing (CLAUDE.md 2.4).
        message.state = DeliveryState::Queued;
        message.releaseUnix = budget_->earliestLegalTxUnix(message.band, airtimeUs);
    }
    return delayMs;
}

size_t Arq::submit(const uint8_t *frame, size_t frameLen, uint8_t seq, uint8_t sf, Band band,
                   bool ackRequested, uint32_t nowMs)
{
    if (frame == nullptr || frameLen == 0 || frameLen > kMaxFrameBytes) {
        return kMaxOutstanding;
    }
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        if (messages_[i].used) {
            continue;
        }
        Message &message = messages_[i];
        message = Message{};
        message.used = true;
        for (size_t b = 0; b < frameLen; ++b) {
            message.frame[b] = frame[b];
        }
        message.frameLen = frameLen;
        message.seq = seq;
        message.sf = sf;
        message.band = band;
        message.ackRequested = ackRequested;
        message.attempts = 0;
        message.dueMs = nowMs;
        message.state = DeliveryState::Queued;
        return i;
    }
    return kMaxOutstanding;
}

ArqOutcome Arq::poll(uint32_t nowMs)
{
    ArqOutcome outcome{ArqAction::Nothing, kMaxOutstanding, DeliveryState::Idle, 0};

    // Terminal states first: the caller must be told about them even while
    // something else is on the air.
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        Message &message = messages_[i];
        if (message.used && (message.state == DeliveryState::Delivered ||
                             message.state == DeliveryState::Undelivered)) {
            outcome.action = ArqAction::Deliver;
            outcome.slot = i;
            outcome.state = message.state;
            outcome.releaseUnix = message.releaseUnix;
            return outcome;
        }
    }

    // Stop-and-wait: nothing else goes out while one message is unresolved.
    if (inFlightSlot_ < kMaxOutstanding) {
        const size_t slot = inFlightSlot_;
        Message &flying = messages_[slot];

        /*
         * Queued counts as unresolved here, not as finished. A retry that the
         * budget pushed back is moved to Queued so the UI can say "queued until"
         * rather than "retrying" -- and treating that as a terminal state would
         * strand the message in the in-flight slot for ever, blocking the whole
         * queue behind it. Only Delivered and Undelivered are terminal.
         */
        const bool waitingOnRadio = flying.awaitingRadio;
        const bool active = flying.used && (flying.state == DeliveryState::InFlight ||
                                            flying.state == DeliveryState::Queued);
        if (waitingOnRadio || !active || !reached(nowMs, flying.dueMs)) {
            return outcome;
        }

        if (flying.attempts > kMaxRetries) {
            /*
             * "After 3 failed attempts the frame is marked undelivered and
             * surfaced in the UI. It is not silently dropped." -- CLAUDE.md 2.4
             *
             * Marked here, reported on the next poll by the terminal-state loop
             * above, so there is exactly one place that hands terminal states to
             * the caller.
             */
            flying.state = DeliveryState::Undelivered;
            inFlightSlot_ = kMaxOutstanding;
            outcome.action = ArqAction::Deliver;
            outcome.slot = slot;
            outcome.state = DeliveryState::Undelivered;
            return outcome;
        }

        // The retry timer has expired; the budget may still hold it back.
        const uint32_t budgetDelayMs = budgetGate(flying, nowMs);
        if (budgetDelayMs > 0) {
            flying.dueMs = nowMs + budgetDelayMs;
            outcome.slot = slot;
            outcome.state = flying.state;
            outcome.releaseUnix = flying.releaseUnix;
            return outcome;
        }

        flying.awaitingRadio = true;
        ++flying.attempts;
        flying.state = DeliveryState::InFlight;
        flying.releaseUnix = 0;
        outcome.action = ArqAction::Transmit;
        outcome.slot = slot;
        outcome.state = DeliveryState::InFlight;
        return outcome;
    }

    // Nothing in flight: start the oldest message that is due.
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        Message &message = messages_[i];
        if (!message.used || message.awaitingRadio) {
            continue;
        }
        if (message.state != DeliveryState::Queued && message.state != DeliveryState::InFlight) {
            continue;
        }
        if (!reached(nowMs, message.dueMs)) {
            continue;
        }

        // Re-ask the budget: the rolling window moves under us.
        const uint32_t budgetDelayMs = budgetGate(message, nowMs);
        if (budgetDelayMs > 0) {
            message.dueMs = nowMs + budgetDelayMs;
            continue;
        }

        message.releaseUnix = 0;
        message.awaitingRadio = true;
        ++message.attempts;
        message.state = DeliveryState::InFlight;
        inFlightSlot_ = i;

        outcome.action = ArqAction::Transmit;
        outcome.slot = i;
        outcome.state = message.state;
        return outcome;
    }

    // Nothing to do. Report the soonest queued release so the UI has something.
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        const Message &message = messages_[i];
        if (message.used && message.state == DeliveryState::Queued && message.releaseUnix != 0) {
            outcome.slot = i;
            outcome.state = DeliveryState::Queued;
            outcome.releaseUnix = message.releaseUnix;
            break;
        }
    }
    return outcome;
}

void Arq::onTransmitComplete(size_t slot, uint32_t nowMs, bool success)
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return;
    }
    Message &message = messages_[slot];
    message.awaitingRadio = false;

    if (!success) {
        // The radio itself failed. Treat it as an attempt that produced nothing
        // and let the normal retry path handle it.
        message.state = DeliveryState::InFlight;
        message.dueMs = nowMs;
        return;
    }

    if (!message.ackRequested) {
        // Broadcast: sent is delivered, there is nothing to wait for.
        message.state = DeliveryState::Delivered;
        if (inFlightSlot_ == slot) {
            inFlightSlot_ = kMaxOutstanding;
        }
        return;
    }

    armRetryTimer(message, nowMs);
}

void Arq::onAckReceived(uint8_t seq, uint32_t nowMs)
{
    (void)nowMs;
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        Message &message = messages_[i];
        if (!message.used || message.seq != seq) {
            continue;
        }
        if (message.state != DeliveryState::InFlight && message.state != DeliveryState::Queued) {
            continue;
        }
        message.state = DeliveryState::Delivered;
        if (inFlightSlot_ == i) {
            inFlightSlot_ = kMaxOutstanding;
        }
        return;
    }
}

size_t Arq::slotOfActiveSeq(uint8_t seq) const
{
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        const Message &message = messages_[i];
        if (!message.used || message.seq != seq) {
            continue;
        }
        if (message.state == DeliveryState::InFlight || message.state == DeliveryState::Queued) {
            return i;
        }
    }
    return kMaxOutstanding;
}

bool Arq::replaceFrame(size_t slot, const uint8_t *frame, size_t frameLen)
{
    if (slot >= kMaxOutstanding || !messages_[slot].used || frame == nullptr) {
        return false;
    }
    Message &message = messages_[slot];
    if (frameLen != message.frameLen) {
        // Same payload, same overhead -- a different length means the caller
        // rebuilt something other than this message.
        return false;
    }
    for (size_t b = 0; b < frameLen; ++b) {
        message.frame[b] = frame[b];
    }
    return true;
}

DeliveryState Arq::stateOf(size_t slot) const
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return DeliveryState::Idle;
    }
    return messages_[slot].state;
}

uint8_t Arq::attemptsOf(size_t slot) const
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return 0;
    }
    return messages_[slot].attempts;
}

bool Arq::ackRequestedOf(size_t slot) const
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return false;
    }
    return messages_[slot].ackRequested;
}

uint32_t Arq::releaseUnixOf(size_t slot) const
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return 0;
    }
    return messages_[slot].releaseUnix;
}

const uint8_t *Arq::frameOf(size_t slot, size_t *lenOut) const
{
    if (slot >= kMaxOutstanding || !messages_[slot].used) {
        return nullptr;
    }
    if (lenOut != nullptr) {
        *lenOut = messages_[slot].frameLen;
    }
    return messages_[slot].frame;
}

void Arq::clear(size_t slot)
{
    if (slot >= kMaxOutstanding) {
        return;
    }
    if (inFlightSlot_ == slot) {
        inFlightSlot_ = kMaxOutstanding;
    }
    messages_[slot] = Message{};
}

size_t Arq::pending() const
{
    size_t total = 0;
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        if (messages_[i].used) {
            ++total;
        }
    }
    return total;
}

} // namespace link
