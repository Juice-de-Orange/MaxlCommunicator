/*
 * A pair of radios connected by a simulated channel.
 *
 * Frames take their real airtime to arrive -- computed by link/airtime, the same
 * function the budget charges -- so the simulation's timing is the firmware's own
 * arithmetic rather than a guess. Loss and SNR are set by the scenario, which is
 * how the two-node simulation stands in for the step attenuator that
 * docs/test-plan.md calls "the single most useful purchase here".
 *
 * One thing it DOES model, because a bug hid behind its absence: whether the
 * sender's preamble is long enough for a duty-cycled receiver to catch it. A
 * receiver that is sniffing is asleep for most of the cycle and only hears a
 * frame whose preamble spans it -- which is the whole reason CLAUDE.md 2.3
 * derives the preamble from the sniff interval. A short-preamble frame reaches
 * a receiver in continuous receive and nobody else.
 *
 * What it still does NOT model, and what therefore needs hardware: which end of
 * the sniff cycle a frame lands in, collisions between the two nodes, TCXO
 * startup, and every way a real SX1262 can disappoint you.
 */

#ifndef MAXL_TEST_FAKE_RADIO_H
#define MAXL_TEST_FAKE_RADIO_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hal/i_radio_link.h"
#include "link/airtime.h"
#include "link/frame.h"

namespace fakes {

/// Deterministic PRNG. A simulation whose failures cannot be reproduced is a
/// simulation that cannot be debugged.
class Lcg {
public:
    explicit Lcg(uint32_t seed) : state_(seed ? seed : 1u) {}

    uint32_t next()
    {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

    /// Uniform in [0, bound).
    uint32_t below(uint32_t bound) { return bound ? (next() >> 8) % bound : 0u; }

    /// True with probability percent/100.
    bool chance(uint32_t percent) { return below(100u) < percent; }

private:
    uint32_t state_;
};

class FakeRadio;

/// The medium. Holds frames in flight and hands them over when their airtime is
/// up.
class SimChannel {
public:
    struct Conditions {
        uint32_t lossPercent = 0;
        int8_t snrDb = 8;
        int16_t rssiDbm = -80;
    };

    explicit SimChannel(uint32_t seed = 12345u) : rng_(seed) {}

    void attach(FakeRadio *a, FakeRadio *b)
    {
        radios_[0] = a;
        radios_[1] = b;
    }

    Conditions conditions;

    /// Called by a radio when it starts transmitting.
    void submit(const FakeRadio *from, const uint8_t *data, size_t len, uint8_t sf,
                uint16_t preambleSymbols, uint32_t nowMs);

    /// Advance to `nowMs`, delivering anything whose airtime has elapsed.
    void pump(uint32_t nowMs);

    uint32_t framesSent = 0;
    uint32_t framesDropped = 0;

private:
    struct InFlight {
        bool used = false;
        const FakeRadio *from = nullptr;
        uint8_t data[link::kMaxFrameBytes];
        size_t length = 0;
        uint32_t arrivesAtMs = 0;
        uint16_t preambleSymbols = 0;
        bool lost = false;
    };

    static constexpr size_t kMaxInFlight = 8;

    InFlight inFlight_[kMaxInFlight];
    FakeRadio *radios_[2] = {nullptr, nullptr};
    Lcg rng_;
};

class FakeRadio : public hal::IRadioLink {
public:
    FakeRadio(SimChannel &channel, uint32_t *nowMs) : channel_(channel), nowMs_(nowMs) {}

    void setObserver(hal::IRadioObserver *observer) override { observer_ = observer; }

    hal::RadioResult setModulation(const hal::Modulation &modulation) override
    {
        modulation_ = modulation;
        return hal::RadioResult::Ok;
    }

    hal::RadioResult setPreambleLength(uint16_t symbols) override
    {
        preambleSymbols_ = symbols;
        return hal::RadioResult::Ok;
    }

    hal::RadioResult transmit(const uint8_t *data, size_t len) override
    {
        if (transmitting_) {
            return hal::RadioResult::Busy;
        }
        transmitting_ = true;
        transmitDoneMs_ =
            *nowMs_ + link::timeOnAirMs(modulation_.spreadingFactor,
                                        static_cast<uint8_t>(len), preambleSymbols_);
        channel_.submit(this, data, len, modulation_.spreadingFactor, preambleSymbols_, *nowMs_);
        ++transmitCount;
        return hal::RadioResult::Ok;
    }

    hal::RadioResult startReceiveDutyCycle(uint8_t minSymbols) override
    {
        if (minSymbols < 8) {
            return hal::RadioResult::BadParam;
        }
        listening_ = true;
        dutyCycle_ = true;
        // The same condition RadioLib applies: with no room to sleep between
        // sniffs it silently listens continuously instead.
        dutyCycleActive_ = (2u * static_cast<uint32_t>(minSymbols)) <= preambleSymbols_;
        return hal::RadioResult::Ok;
    }

    bool rxDutyCycleActive() const override { return dutyCycleActive_; }

    hal::RadioResult startReceiveContinuous() override
    {
        listening_ = true;
        dutyCycle_ = false;
        dutyCycleActive_ = false;
        return hal::RadioResult::Ok;
    }

    hal::RadioResult sleep() override
    {
        listening_ = false;
        dutyCycle_ = false;
        dutyCycleActive_ = false;
        return hal::RadioResult::Ok;
    }

    /// Called by the channel. `senderPreambleSymbols` is what went on the air.
    void deliver(const uint8_t *data, size_t len, const hal::RxInfo &info,
                 uint16_t senderPreambleSymbols)
    {
        if (!listening_ || observer_ == nullptr) {
            return;
        }
        /*
         * The preamble is a parameter of BOTH modems, and a receiver configured
         * for a longer one than the sender used does not hear the frame.
         *
         * REVIEW.md C5 says this for the duty cycle -- a sniffing receiver
         * sleeps through most of its cycle and can only latch a preamble that
         * spans it -- and the bench extended it to continuous receive on
         * 2026-09-01: node B acknowledged on 8 symbols while node A listened
         * for 123, and A's driver never saw a single receive interrupt in five
         * minutes. Its packet status word never left 0x000000. With both ends
         * on 123 the run before it, A heard ACKs.
         *
         * So this models the receiver's SETTING, not just its mode. Whoever
         * changes the preamble on one side has to change it on the other, which
         * is the coupling app::Node's ACK window exists to keep.
         */
        if (senderPreambleSymbols < preambleSymbols_) {
            ++missedTooShort;
            return;
        }
        ++receiveCount;
        // Tap: keep the last frame verbatim so a scenario can capture one off the
        // air and replay it, which is what docs/test-plan.md 2.3 describes.
        lastFrameLength = (len < sizeof(lastFrame)) ? len : sizeof(lastFrame);
        std::memcpy(lastFrame, data, lastFrameLength);

        /*
         * The chip leaves the receive window here.
         *
         * Under a real RxDutyCycle the SX1262 lands in STDBY_RC on RxDone and
         * stays there. This fake used to keep listening_ set until sleep(), so a
         * driver that forgot to re-arm looked perfect in simulation -- and the
         * bug that hid behind that is precisely the one this models. Continuous
         * receive does not end on RxDone, so only the duty cycle drops out.
         */
        const bool wasDutyCycle = dutyCycle_;
        if (wasDutyCycle) {
            listening_ = false;
        }

        observer_->onFrameReceived(data, len, info);

        /*
         * And the driver puts it back, unless the observer started a
         * transmission -- Sx1262Radio::service() does exactly this, guarded the
         * same way. Set rearmAfterReceive false to model a driver that forgets.
         */
        if (wasDutyCycle && rearmAfterReceive && !transmitting_) {
            listening_ = true;
        }
    }

    /// Mirrors Sx1262Radio::service() re-arming the receiver after RxDone.
    /// Turn it off to prove what a missing re-arm costs.
    bool rearmAfterReceive = true;

    /// Frames that arrived while sniffing with a preamble too short to latch.
    uint32_t missedTooShort = 0;

    /// Inject a frame directly, as an attacker with a transmitter would.
    void inject(const uint8_t *data, size_t len, const hal::RxInfo &info)
    {
        if (observer_ != nullptr) {
            observer_->onFrameReceived(data, len, info);
        }
    }

    uint8_t lastFrame[link::kMaxFrameBytes] = {};
    size_t lastFrameLength = 0;

    /// Complete any transmission whose airtime has elapsed.
    void pump()
    {
        if (transmitting_ && *nowMs_ >= transmitDoneMs_) {
            transmitting_ = false;
            if (observer_ != nullptr) {
                observer_->onTransmitComplete(hal::RadioResult::Ok);
            }
        }
    }

    bool transmitting() const { return transmitting_; }
    uint8_t spreadingFactor() const { return modulation_.spreadingFactor; }

    /// Set false to model a node that is switched off.
    bool powered = true;

    uint32_t transmitCount = 0;
    uint32_t receiveCount = 0;

private:
    SimChannel &channel_;
    uint32_t *nowMs_;
    hal::IRadioObserver *observer_ = nullptr;
    hal::Modulation modulation_{9, 869575000u, 22};
    uint16_t preambleSymbols_ = link::kStandardPreambleSymbols;
    bool listening_ = false;
    bool transmitting_ = false;
    bool dutyCycle_ = false;
    bool dutyCycleActive_ = false;
    uint32_t transmitDoneMs_ = 0;
};

inline void SimChannel::submit(const FakeRadio *from, const uint8_t *data, size_t len,
                               uint8_t sf, uint16_t preambleSymbols, uint32_t nowMs)
{
    ++framesSent;
    for (size_t i = 0; i < kMaxInFlight; ++i) {
        if (inFlight_[i].used) {
            continue;
        }
        InFlight &slot = inFlight_[i];
        slot.used = true;
        slot.from = from;
        slot.length = (len < sizeof(slot.data)) ? len : sizeof(slot.data);
        std::memcpy(slot.data, data, slot.length);
        slot.preambleSymbols = preambleSymbols;
        slot.arrivesAtMs = nowMs + link::timeOnAirMs(sf, static_cast<uint8_t>(len),
                                                     preambleSymbols);
        slot.lost = rng_.chance(conditions.lossPercent);
        if (slot.lost) {
            ++framesDropped;
        }
        return;
    }
    // No slot: the medium is saturated beyond what this simulation models.
    ++framesDropped;
}

inline void SimChannel::pump(uint32_t nowMs)
{
    for (size_t i = 0; i < kMaxInFlight; ++i) {
        InFlight &slot = inFlight_[i];
        if (!slot.used || nowMs < slot.arrivesAtMs) {
            continue;
        }
        slot.used = false;
        if (slot.lost) {
            continue;
        }
        hal::RxInfo info{conditions.rssiDbm, conditions.snrDb};
        for (FakeRadio *radio : radios_) {
            if (radio != nullptr && radio != slot.from && radio->powered) {
                radio->deliver(slot.data, slot.length, info, slot.preambleSymbols);
            }
        }
    }
}

} // namespace fakes

#endif // MAXL_TEST_FAKE_RADIO_H
