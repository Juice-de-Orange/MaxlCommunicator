#include "node.h"

#include <string.h>

#include "ble/bridge_codec.h"
#include "link/payloads.h"

namespace app {
namespace {

/// docs/bridge-protocol.md section 4, the status body.
constexpr size_t kStatusBytes = 17;
constexpr uint8_t kFlagTimeValid = 0x01;
constexpr uint8_t kFlagKeyProvisioned = 0x02;
constexpr uint8_t kFlagGnssPowered = 0x04;

/// EVT_BUDGET: band u8, usedMs u32, limitMs u32, nextTxUnix u32.
constexpr size_t kBudgetBytes = 13;

void putU16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void putU32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint16_t getU16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

/// A representative frame for "can I send anything at all". POSITION at SF9 is
/// the shape the budget is usually asked about (CLAUDE.md 1.4).
constexpr uint32_t kRepresentativeAirtimeUs = 226000;

} // namespace

Node::Node(hal::IClock &clock, hal::IBlockStore &store, hal::IKeyStore &keys)
    : clock_(clock), store_(store), keys_(keys), queue_(store), journal_(store)
{
}

link::Band Node::activeBand() const
{
    return config_.band == 1 ? link::Band::G1 : link::Band::G3;
}

bool Node::begin(uint16_t nodeId)
{
    nodeId_ = nodeId;

    /*
     * Order matters. The counter first, because CLAUDE.md 2.1 makes it the
     * hardest precondition -- "If the counter cannot be persisted, the device
     * must refuse to transmit" -- and there is no point restoring anything else
     * for a node that may not send.
     */
    const bool counterOk = counter_.begin(store_);
    const bool budgetOk = budget_.begin(store_, clock_);
    queue_.restore();
    // Bounded by the counter that was just restored: anything at or above it is
    // not something this device wrote. Without the bound one leftover record
    // from bring-up 10 silences the journal for the life of the board.
    journal_.restore(counter_.reservedUpTo());

    scheduler_.setInterval(Task::Telemetry, config_.telemetryIntervalS, clock_.monotonicMs());
    scheduler_.setInterval(Task::Beacon, config_.beaconIntervalS, clock_.monotonicMs());

    ready_ = counterOk && budgetOk;
    return ready_;
}

bool Node::transmitAllowed() const
{
    /*
     * Three independent conditions, and each one is somebody's rule:
     *
     *   the counter is healthy      CLAUDE.md 2.1 -- otherwise CCM nonces repeat
     *   the clock is valid          1.2 -- the budget is reconstructed against it
     *   a key is provisioned        2.1 -- there is nothing to authenticate with
     *
     * The budget's own refusal is separate and per-frame; this is the question
     * of whether the node may transmit at all.
     */
    return ready_ && counter_.transmitAllowed() && clock_.timeValid() && keys_.hasAnyKey();
}

void Node::tick(uint32_t nowMs)
{
    if (scheduler_.due(Task::Telemetry, nowMs)) {
        // The sample itself needs the BME280 and belongs to phase 1. What is
        // wired here is the schedule, which is what gate 3.4 measures.
    }
    if (scheduler_.due(Task::Beacon, nowMs)) {
        // Likewise: CLAUDE.md 2.5 says to beacon rarely with two nodes and to
        // treat any received frame as an implicit beacon.
    }

    // The durable record of the state transitions -- time valid, key
    // provisioned, GNSS power. See journalStatusIfChanged() for why it is these
    // and not a timer.
    journalStatusIfChanged();

    /*
     * Adaptive SF (CLAUDE.md 2.5). Wired on 2026-08-31: the class existed with
     * its fourteen unit tests and was attached to nothing -- currentSf_ was set
     * once in attachRadio() and never moved again.
     */
    if (radio_ != nullptr) {
        if (config_.sfMode == 1) {
            applySpreadingFactor(config_.fixedSf);
        } else {
            /*
             * The rendezvous fallback needs "time since last contact". Peers
             * record wall-clock seconds; only ask when the clock is valid, and
             * with no peer ever heard there is nothing to fall back FROM.
             */
            if (clock_.timeValid() && peers_.size() > 0) {
                uint32_t latest = 0;
                for (size_t i = 0; i < peers_.size(); ++i) {
                    if (peers_.at(i).lastSeenUnix > latest) {
                        latest = peers_.at(i).lastSeenUnix;
                    }
                }
                const uint32_t nowUnix = clock_.unixSeconds();
                const uint32_t sinceS = nowUnix > latest ? nowUnix - latest : 0;
                adaptiveSf_.tick(sinceS * 1000UL, config_.beaconIntervalS * 1000UL);
            }
            applySpreadingFactor(adaptiveSf_.currentSf());
        }
    }
}

void Node::applySpreadingFactor(uint8_t sf)
{
    if (radio_ == nullptr || sf == currentSf_ || sf < 7 || sf > 12) {
        return;
    }
    if (adaptiveSf_.locked() || txSlot_ < link::kMaxOutstanding || ackTxInFlight_) {
        // "Never change SF mid-retry sequence" (CLAUDE.md 2.5), and never with a
        // frame on the radio. This is the first line of that rule;
        // Sx1262Radio::setModulation refusing while transmitting is the last.
        return;
    }

    const link::BandPlan &plan = link::plan(activeBand());
    hal::Modulation modulation{};
    modulation.spreadingFactor = sf;
    modulation.frequencyHz = plan.defaultHz;
    modulation.txPowerDbm = link::clampTxPowerDbm(activeBand(), config_.txPowerDbm);
    if (radio_->setModulation(modulation) != hal::RadioResult::Ok) {
        return;
    }
    currentSf_ = sf;

    // The preamble and the sniff cadence move with the SF, on both sides, from
    // the same function -- or startReceiveDutyCycleAuto drops packets
    // (REVIEW.md C5, and the attachRadio comment below).
    const uint16_t preamble =
        link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
    radio_->setPreambleLength(preamble);
    radio_->startReceiveDutyCycle(currentSf_ > 10 ? 12 : 8);
}

// --- ble::IHost ----------------------------------------------------------

ble::DeviceInfo Node::info() const
{
    ble::DeviceInfo out;
    out.bridgeProtocol = ble::kBridgeProtocolVersion;
    out.nodeId = nodeId_;
    out.firmwareMajor = 0;
    out.firmwareMinor = 1;
    out.firmwarePatch = 0;
    out.wireVersion = 1;
    return out;
}

size_t Node::status(uint8_t *out, size_t max) const
{
    if (max < kStatusBytes) {
        return 0;
    }
    const link::Band band = activeBand();

    // Battery and uptime come from hal in phase 1; zero is honest until then,
    // and the field exists so the layout does not change when it arrives.
    putU16(out, 0);
    putU32(out + 2, clock_.monotonicMs() / 1000u);
    out[6] = static_cast<uint8_t>(queue_.size());
    out[7] = static_cast<uint8_t>(band);
    putU32(out + 8, budget_.usedMs(band));
    putU32(out + 12, budget_.usedMs(band) + budget_.remainingMs(band));

    uint8_t flags = 0;
    if (clock_.timeValid()) {
        flags |= kFlagTimeValid;
    }
    if (keys_.hasAnyKey()) {
        flags |= kFlagKeyProvisioned;
    }
    if (fixPending_) {
        flags |= kFlagGnssPowered;
    }
    out[16] = flags;
    return kStatusBytes;
}

size_t Node::budget(uint8_t *out, size_t max) const
{
    if (max < kBudgetBytes) {
        return 0;
    }
    const link::Band band = activeBand();
    out[0] = static_cast<uint8_t>(band);
    putU32(out + 1, budget_.usedMs(band));
    putU32(out + 5, budget_.usedMs(band) + budget_.remainingMs(band));

    /*
     * nextTxUnix is what lets the UI say "queued until HH:MM" instead of leaving
     * the user looking at a device that is doing nothing for no stated reason
     * (CLAUDE.md 2.4, and field test F.5). Zero means "now", which is a real
     * answer rather than a missing one.
     */
    const uint32_t earliest = budget_.earliestLegalTxUnix(band, kRepresentativeAirtimeUs);
    putU32(out + 9, earliest == link::kNeverLegal ? 0 : earliest);
    return kBudgetBytes;
}

uint8_t Node::sendText(uint16_t dst, const uint8_t *text, size_t length)
{
    if (length > link::kMaxTextBytes) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    if (!clock_.timeValid()) {
        return static_cast<uint8_t>(ble::BridgeError::NoTime);
    }
    if (!keys_.hasAnyKey()) {
        return static_cast<uint8_t>(ble::BridgeError::NoKey);
    }
    if (!counter_.transmitAllowed()) {
        return static_cast<uint8_t>(ble::BridgeError::Storage);
    }

    const Accept accepted = queue_.submit(dst, reinterpret_cast<const char *>(text), length,
                                          clock_.unixSeconds());
    switch (accepted) {
    case Accept::Ok:
        return 0;
    case Accept::TextTooLong:
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    case Accept::Full:
        return static_cast<uint8_t>(ble::BridgeError::QueueFull);
    case Accept::StorageFailed:
        return static_cast<uint8_t>(ble::BridgeError::Storage);
    }
    return static_cast<uint8_t>(ble::BridgeError::Storage);
}

size_t Node::fetchQueue(uint32_t sinceCounter, ble::QueuedEvent *out, size_t max)
{
    const size_t capacity = max > 32 ? 32 : max;
    const size_t howMany = journal_.fetch(sinceCounter, fetched_, capacity);
    for (size_t i = 0; i < howMany; ++i) {
        out[i].counter = fetched_[i].counter;
        out[i].opcode = fetched_[i].opcode;
        out[i].length = fetched_[i].length;
        out[i].body = fetched_[i].body;
    }
    return howMany;
}

void Node::ackQueue(uint32_t upToCounter)
{
    // The only thing that frees journal space, and deliberately so -- the phone
    // acknowledges only after the server has the data.
    journal_.acknowledge(upToCounter);
}

uint8_t Node::setConfig(uint32_t configVersion, const uint8_t *tlvs, size_t length,
                        uint32_t *appliedMask, uint8_t *unapplied, size_t *unappliedCount)
{
    *appliedMask = 0;
    *unappliedCount = 0;

    size_t cursor = 0;
    ble::TlvView tlv{};
    while (ble::nextTlv(tlvs, length, &cursor, &tlv)) {
        bool applied = true;
        switch (static_cast<ble::ConfigTlv>(tlv.type)) {
        case ble::ConfigTlv::SniffIntervalMs: {
            if (tlv.length != 2) {
                applied = false;
                break;
            }
            const uint16_t value = getU16(tlv.value);
            // The TLV table gives 250..10000. A node told to sniff every 20 ms
            // is a node with a flat battery by lunchtime.
            if (value < 250 || value > 10000) {
                applied = false;
                break;
            }
            config_.sniffIntervalMs = value;
            break;
        }
        case ble::ConfigTlv::Band:
            if (tlv.length != 1 || tlv.value[0] > 1) {
                applied = false;
                break;
            }
            config_.band = tlv.value[0];
            break;
        case ble::ConfigTlv::SfMode:
            if (tlv.length != 1 || tlv.value[0] > 1) {
                applied = false;
                break;
            }
            config_.sfMode = tlv.value[0];
            break;
        case ble::ConfigTlv::FixedSf:
            if (tlv.length != 1 || tlv.value[0] < 7 || tlv.value[0] > 12) {
                applied = false;
                break;
            }
            config_.fixedSf = tlv.value[0];
            break;
        case ble::ConfigTlv::TxPowerDbm:
            if (tlv.length != 1) {
                applied = false;
                break;
            }
            /*
             * Stored, never trusted. CLAUDE.md 1.3: "ERP includes antenna gain
             * and cable loss, so the configured TX power is capped per band in
             * code, not by the user." The cap lives in link/band and applies
             * whatever arrives here.
             */
            config_.txPowerDbm = static_cast<int8_t>(tlv.value[0]);
            break;
        case ble::ConfigTlv::TelemetryIntervalS:
            if (tlv.length != 2) {
                applied = false;
                break;
            }
            config_.telemetryIntervalS = getU16(tlv.value);
            scheduler_.setInterval(Task::Telemetry, config_.telemetryIntervalS,
                                   clock_.monotonicMs());
            break;
        case ble::ConfigTlv::BeaconIntervalS:
            if (tlv.length != 2) {
                applied = false;
                break;
            }
            config_.beaconIntervalS = getU16(tlv.value);
            scheduler_.setInterval(Task::Beacon, config_.beaconIntervalS, clock_.monotonicMs());
            break;
        case ble::ConfigTlv::GnssFixTimeoutS:
            if (tlv.length != 2) {
                applied = false;
                break;
            }
            config_.gnssFixTimeoutS = getU16(tlv.value);
            break;
        case ble::ConfigTlv::DeviceName:
            // Held by the BLE layer, which is what advertises it.
            break;
        default:
            applied = false;
            break;
        }

        if (applied) {
            *appliedMask |= (1UL << (tlv.type & 0x1F));
        } else if (*unappliedCount < 16) {
            /*
             * Section 3: "Unknown types are ignored and reported back in
             * EVT_CONFIG_APPLIED as unapplied, rather than failing the whole
             * write." A value out of range is reported the same way -- the
             * client learns which setting did not take instead of losing the
             * seven that did.
             */
            unapplied[(*unappliedCount)++] = tlv.type;
        }
    }

    appliedConfigVersion_ = configVersion;

    /*
     * Journal the acknowledgement, not just emit it.
     *
     * docs/bridge-protocol.md section 4 makes journaling binding for every event
     * type it lists, and this is the one that hurts most if it is missed:
     * `config_versions.applied_at` in the dashboard is set from EVT_CONFIG_APPLIED
     * and from nothing else (CLAUDE.md 4.3). Emitted only, it reaches a phone
     * that happens to be connected at this instant -- and section 4.2 builds the
     * product around a phone that usually is not.
     *
     * The spontaneous copy still goes out from ble/gatt_server; this is the
     * durable one, and GET_QUEUE will offer it wrapped in EVT_JOURNAL.
     */
    uint8_t applied[8 + 16];
    putU32(applied, configVersion);
    putU32(applied + 4, *appliedMask);
    const size_t reported = *unappliedCount < 16 ? *unappliedCount : 16;
    for (size_t i = 0; i < reported; ++i) {
        applied[8 + i] = unapplied[i];
    }
    journalEvent(static_cast<uint8_t>(ble::EventCode::ConfigApplied), applied, 8 + reported);

    return 0;
}

size_t Node::getConfig(uint8_t *out, size_t max) const
{
    size_t written = 0;
    uint8_t scratch[2];

    auto put = [&](ble::ConfigTlv type, const uint8_t *value, uint8_t length) {
        const size_t n = ble::encodeTlv(static_cast<uint8_t>(type), value, length, out + written,
                                        max - written);
        written += n;
    };

    putU16(scratch, config_.sniffIntervalMs);
    put(ble::ConfigTlv::SniffIntervalMs, scratch, 2);
    put(ble::ConfigTlv::Band, &config_.band, 1);
    put(ble::ConfigTlv::SfMode, &config_.sfMode, 1);
    put(ble::ConfigTlv::FixedSf, &config_.fixedSf, 1);
    const uint8_t power = static_cast<uint8_t>(config_.txPowerDbm);
    put(ble::ConfigTlv::TxPowerDbm, &power, 1);
    putU16(scratch, config_.telemetryIntervalS);
    put(ble::ConfigTlv::TelemetryIntervalS, scratch, 2);
    putU16(scratch, config_.beaconIntervalS);
    put(ble::ConfigTlv::BeaconIntervalS, scratch, 2);
    putU16(scratch, config_.gnssFixTimeoutS);
    put(ble::ConfigTlv::GnssFixTimeoutS, scratch, 2);

    return written;
}

uint8_t Node::setTime(uint32_t unixSeconds)
{
    /*
     * Node does not own the clock -- hal does, and only the concrete
     * implementation can write the RTC. What SET_TIME means here is that the
     * caller has to have already done it; this reports whether it took.
     *
     * Sanity: anything before 2020 is not a time somebody meant to set, and a
     * budget reconstructed against 1970 would let the node transmit freely.
     */
    if (unixSeconds < 1577836800UL) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    return clock_.timeValid() ? 0 : static_cast<uint8_t>(ble::BridgeError::NoTime);
}

uint8_t Node::requestFix(uint16_t timeoutSeconds)
{
    if (timeoutSeconds == 0 || timeoutSeconds > 900) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    // CLAUDE.md 1.5: GNSS is off by default and only enabled "for an explicit fix
    // request or while the position screen is open, with a hard timeout". This
    // is the explicit request; the driver is phase 1.
    fixPending_ = true;
    fixTimeoutS_ = timeoutSeconds;
    return 0;
}

uint8_t Node::provisionKey(uint8_t slot, uint8_t netId, const uint8_t key[16])
{
    if (slot >= hal::kKeySlots) {
        // Two slots, because rotation needs both live at once
        // (docs/versioning-and-updates.md).
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }

    // An all-zero key is not a key. Accepting one would encrypt every frame
    // under something an attacker guesses in a single try, and nothing about the
    // frames would look wrong.
    bool anySet = false;
    for (size_t i = 0; i < hal::kKeyBytes; ++i) {
        anySet = anySet || key[i] != 0;
    }
    if (!anySet) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }

    // Internal flash, never the external chip -- CLAUDE.md 3.0: that one "is
    // trivially readable by anyone with a clip and a logic analyser".
    const hal::KeyResult result = keys_.store(slot, netId, key);
    if (result != hal::KeyResult::Ok) {
        // Reported, never swallowed. A node that says a key was provisioned and
        // has none will fail later, on the air, where nobody can see why.
        return static_cast<uint8_t>(ble::BridgeError::Storage);
    }
    netId_ = netId;
    return 0;
}

uint8_t Node::rotateKey(uint8_t newSlot)
{
    if (newSlot >= hal::kKeySlots) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    /*
     * The slot being rotated *to* must already hold a key.
     *
     * Rotating onto an empty slot would have the node transmit under nothing
     * while both peers still believe they share a secret, and
     * docs/versioning-and-updates.md's rotation window is precisely the period
     * in which both keys have to be real. hasAnyKey() would have been the wrong
     * question -- it is true when the *other* slot is provisioned.
     */
    if (!keys_.hasKey(newSlot)) {
        return static_cast<uint8_t>(ble::BridgeError::NoKey);
    }
    activeSlot_ = newSlot;
    return 0;
}

uint8_t Node::factoryReset()
{
    /*
     * Every region except the frame counter's.
     *
     * CLAUDE.md 2.1: "Monotonic per device, never reset, never reused." A
     * factory reset that erased this region would send the counter back to the
     * beginning, and the node would then reuse CCM nonces against a peer that
     * still remembers the old ones -- which is the one failure the whole
     * persistent-counter design exists to prevent.
     *
     * The first version of this method erased everything. The end-to-end test
     * caught it, and it is worth saying plainly what that would have meant: a
     * user tapping "factory reset" would have quietly broken the crypto of every
     * link the device had.
     */
    for (uint8_t region = 0; region < static_cast<uint8_t>(hal::StoreRegion::RegionCount);
         ++region) {
        if (static_cast<hal::StoreRegion>(region) == hal::StoreRegion::FrameCounter) {
            continue;
        }
        store_.erase(static_cast<hal::StoreRegion>(region));
    }
    queue_.restore();
    journal_.restore(counter_.reservedUpTo());
    // The key lives in internal flash and has to be erased explicitly -- which
    // is the point of keeping it somewhere the loop above cannot reach.
    keys_.eraseAll();
    activeSlot_ = 0;
    config_ = NodeConfig{};
    return 0;
}

uint8_t Node::linkTest(uint16_t dst, uint8_t count, uint8_t sf)
{
    (void)dst;
    if (count == 0 || count > 20) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    if (sf != 0 && (sf < 7 || sf > 12)) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    if (!transmitAllowed()) {
        return static_cast<uint8_t>(clock_.timeValid()
                                        ? ble::BridgeError::NoKey
                                        : ble::BridgeError::NoTime);
    }
    /*
     * Refused, never queued. Section 3: LINK_TEST "is subject to the budget like
     * everything else and will return ERR_BUDGET_EXHAUSTED rather than queueing,
     * because a delayed link test is a useless link test."
     */
    if (!budget_.canTransmit(activeBand(), kRepresentativeAirtimeUs)) {
        return static_cast<uint8_t>(ble::BridgeError::BudgetExhausted);
    }
    return 0;
}

// --- radio ---------------------------------------------------------------

bool Node::loadKeys()
{
    bool any = false;
    for (uint8_t slot = 0; slot < hal::kKeySlots; ++slot) {
        uint8_t key[hal::kKeyBytes];
        uint8_t netId = 0;
        if (keys_.load(slot, &netId, key) != hal::KeyResult::Ok) {
            continue;
        }
        if (crypto_.setKey(slot, key) == link::CryptoError::None) {
            any = true;
            netId_ = netId;
        }
        // The key was on this stack. Leaving a copy behind is a copy nobody
        // accounted for.
        memset(key, 0, sizeof(key));
    }
    return any;
}

bool Node::attachRadio(hal::IRadioLink &radio, link::IJitterSource &jitter)
{
    radio_ = &radio;
    radio.setObserver(this);
    arq_.begin(budget_, jitter);
    loadKeys();

    // arq_.begin() dropped every slot, so everything tracking a slot restarts
    // with it. Re-attach happens in tests and after a key change; a stale
    // pending ACK for a dropped slot would still spend real airtime.
    for (size_t i = 0; i < link::kMaxOutstanding; ++i) {
        slotMeta_[i] = SlotMeta{};
    }
    for (size_t i = 0; i < kMaxPendingAcks; ++i) {
        pendingAcks_[i] = PendingAck{};
    }
    ackTxInFlight_ = false;
    txSlot_ = link::kMaxOutstanding;
    adaptiveSf_.resetToRendezvous();

    const link::BandPlan &plan = link::plan(activeBand());
    hal::Modulation modulation{};
    modulation.spreadingFactor = currentSf_;
    modulation.frequencyHz = plan.defaultHz;
    // Clamped here and not trusted from the config: CLAUDE.md 1.3 puts the ERP
    // ceiling "in code, not by the user".
    modulation.txPowerDbm = link::clampTxPowerDbm(activeBand(), config_.txPowerDbm);
    if (radio.setModulation(modulation) != hal::RadioResult::Ok) {
        return false;
    }

    /*
     * The preamble is computed from the sniff interval, not chosen.
     *
     * REVIEW.md C5: startReceiveDutyCycleAuto() silently drops 10-25 % of
     * packets unless the sender's preamble spans one sniff interval. Both sides
     * derive it from the same function so there is no constant for anyone to get
     * out of step.
     */
    const uint16_t preamble =
        link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
    radio.setPreambleLength(preamble);
    radio.startReceiveDutyCycle(currentSf_ > 10 ? 12 : 8);
    return true;
}

size_t Node::buildFrame(uint16_t dst, link::FrameType type, const uint8_t *payload, size_t length,
                        uint8_t seq, bool ackRequested, bool retry, uint8_t *out, size_t capacity,
                        uint32_t *counterOut)
{
    if (length > link::kMaxPayloadBytes || capacity < link::kHeaderBytes + length
        + link::kMicBytes) {
        return 0;
    }

    uint32_t counter = 0;
    if (!counter_.next(&counter)) {
        // No counter, no frame. CLAUDE.md 2.1: the counter is what makes nonce
        // reuse impossible, and a frame without one is a frame that must not go.
        return 0;
    }
    if (counterOut != nullptr) {
        *counterOut = counter;
    }

    link::Header header{};
    header.version = link::kWireVersion;
    header.type = type;
    header.netId = netId_;
    header.src = nodeId_;
    header.dst = dst;
    header.counter = counter;
    header.seq = seq;
    header.flags = static_cast<uint8_t>((ackRequested ? link::kFlagAckReq : 0)
                                        | (retry ? link::kFlagRetry : 0));
    link::encodeHeader(header, out);

    if (crypto_.encrypt(out, payload, length, out + link::kHeaderBytes,
                        capacity - link::kHeaderBytes)
        != link::CryptoError::None) {
        return 0;
    }
    return link::kHeaderBytes + length + link::kMicBytes;
}

void Node::promoteQueued(uint32_t nowMs)
{
    if (radio_ == nullptr || !transmitAllowed()) {
        return;
    }
    for (size_t i = 0; i < queue_.size(); ++i) {
        const QueuedMessage &message = queue_.at(i);
        if (message.state != MessageState::Pending) {
            continue;
        }
        ++promoteSeen_;

        uint8_t frame[link::kMaxFrameBytes];
        /*
         * Broadcast never asks for an ACK (CLAUDE.md 2.4), and asking would be
         * worse than useless: every peer in range would answer, and every answer
         * spends that peer's own hourly allowance.
         */
        const bool wantsAck = message.dst != link::kBroadcastAddress;
        uint32_t frameCounter = 0;
        const size_t frameLen =
            buildFrame(message.dst, link::FrameType::Text,
                       reinterpret_cast<const uint8_t *>(message.text), message.textLen,
                       message.seq, wantsAck, false, frame, sizeof(frame), &frameCounter);
        if (frameLen == 0) {
            ++buildFailures_;
            continue;
        }

        const size_t slot = arq_.submit(frame, frameLen, message.seq, currentSf_, activeBand(),
                                        wantsAck, nowMs);
        if (slot >= link::kMaxOutstanding) {
            // The ARQ window is full. The message stays Pending and is offered
            // again next time round -- nothing is lost, it just waits.
            ++arqRejects_;
            return;
        }
        slotMeta_[slot] = SlotMeta{};
        slotMeta_[slot].messageId = message.id;
        slotMeta_[slot].frameCounter = frameCounter;
        queue_.setState(message.id, MessageState::InFlight, 0);
    }
}

const QueuedMessage *Node::messageById(uint32_t id) const
{
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (queue_.at(i).id == id) {
            return &queue_.at(i);
        }
    }
    return nullptr;
}

void Node::queueAck(uint16_t dst, uint8_t seq, int16_t rssiDbm, int8_t snrDb, uint32_t nowMs)
{
    /*
     * One pending ACK per peer. A retry that arrives before our ACK went out
     * replaces the older entry -- the sender only needs the newest answer, and
     * two ACKs for the same message would spend airtime saying one thing.
     */
    PendingAck *slot = nullptr;
    for (size_t i = 0; i < kMaxPendingAcks; ++i) {
        if (pendingAcks_[i].used && pendingAcks_[i].dst == dst) {
            slot = &pendingAcks_[i];
            break;
        }
    }
    if (slot == nullptr) {
        for (size_t i = 0; i < kMaxPendingAcks; ++i) {
            if (!pendingAcks_[i].used) {
                slot = &pendingAcks_[i];
                break;
            }
        }
    }
    if (slot == nullptr) {
        // More peers waiting for ACKs than slots. The unacknowledged sender
        // retries, and the retry finds a slot -- late beats never.
        return;
    }
    slot->used = true;
    slot->dst = dst;
    slot->seq = seq;
    slot->rssiDbm = rssiDbm;
    slot->snrDb = snrDb;
    slot->heardAtMs = nowMs;
}

bool Node::serviceAcks(uint32_t nowMs)
{
    if (ackTxInFlight_) {
        return true;
    }
    if (radio_ == nullptr || txSlot_ < link::kMaxOutstanding || !transmitAllowed()) {
        return false;
    }
    for (size_t i = 0; i < kMaxPendingAcks; ++i) {
        PendingAck &pending = pendingAcks_[i];
        if (!pending.used) {
            continue;
        }
        /*
         * Every ACK spends the RECEIVER's budget (CLAUDE.md 1.4), so it goes
         * through the same gate as everything else. Blocked means queued, not
         * dropped -- 2.4 step 2 -- and the next pump asks again.
         */
        /*
         * Which preamble, and it is a decision rather than a constant.
         *
         * The sender listens continuously for link::ackWindowMs after asking
         * for an ACK. Inside that window the short preamble reaches it and
         * costs a quarter of the airtime; outside it the sender is sniffing
         * again and only the full sniff preamble can wake it. Getting this
         * wrong in the cheap direction does not lose a little efficiency, it
         * loses the ACK -- so the test includes the ACK's own flight time and
         * errs towards the long preamble.
         */
        const size_t ackFrameLen = link::kFrameOverheadBytes + link::kAckPayloadBytes;
        const uint32_t shortAirtimeMs = link::timeOnAirMs(
            currentSf_, static_cast<uint8_t>(ackFrameLen), link::kStandardPreambleSymbols);
        const uint32_t sinceHeardMs = nowMs - pending.heardAtMs;
        const bool insideWindow =
            sinceHeardMs + shortAirtimeMs <= link::ackWindowMs(currentSf_);
        const uint16_t preamble =
            insideWindow ? link::kStandardPreambleSymbols
                         : link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
        const uint32_t airtimeUs =
            link::timeOnAirUs(currentSf_, static_cast<uint8_t>(ackFrameLen), preamble);
        if (budget_.earliestLegalTxDelayMs(activeBand(), airtimeUs) > 0) {
            continue;
        }

        link::AckPayload payload{};
        payload.seq = pending.seq;
        payload.rssiDbm = pending.rssiDbm;
        payload.snrDb = pending.snrDb;
        uint8_t body[link::kAckPayloadBytes];
        link::encodeAck(payload, body);

        uint8_t frame[link::kMaxFrameBytes];
        const size_t built = buildFrame(pending.dst, link::FrameType::Ack, body, sizeof(body),
                                        pending.seq, false, false, frame, sizeof(frame));
        if (built == 0) {
            ++buildFailures_;
            return false;
        }
        // The preamble is part of the frame on the air, so it is set before the
        // transmission and restored when it completes.
        radio_->setPreambleLength(preamble);
        if (radio_->transmit(frame, built) != hal::RadioResult::Ok) {
            // Radio busy. The ACK stays pending and is offered again next pump.
            radio_->setPreambleLength(
                link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs));
            return false;
        }
        ackTxInFlight_ = true;
        ackTxIndex_ = i;
        ackTxFrameLen_ = built;
        ackTxPreamble_ = preamble;
        return true;
    }
    return false;
}

void Node::pumpRadio(uint32_t nowMs)
{
    if (radio_ == nullptr) {
        return;
    }
    /*
     * The continuous ACK window is bounded. Left open it would receive
     * perfectly and spend the whole power budget doing it -- which is the
     * failure mode CLAUDE.md 2.3 exists to avoid, so it closes on time even if
     * no ACK ever arrives.
     */
    if (inAckWindow_ && static_cast<int32_t>(nowMs - ackWindowUntilMs_) >= 0) {
        closeAckWindow();
    }
    promoteQueued(nowMs);

    /*
     * ACKs before data. The peer's retry timer is already running, and the
     * retry an overdue ACK provokes costs both sides more airtime than the ACK
     * itself does.
     */
    if (serviceAcks(nowMs)) {
        return;
    }
    if (txSlot_ < link::kMaxOutstanding) {
        return; // a data frame is on the radio; onTransmitComplete resumes us
    }

    for (;;) {
        const link::ArqOutcome outcome = arq_.poll(nowMs);
        if (outcome.action == link::ArqAction::Nothing) {
            /*
             * A message the budget parked is a state the user must see --
             * CLAUDE.md 2.4 keeps "queued until HH:MM" distinct from "in
             * flight" and "undelivered". Journaled once per episode, not once
             * per poll; the flag rearms when the message next transmits.
             */
            if (outcome.slot < link::kMaxOutstanding
                && outcome.state == link::DeliveryState::Queued) {
                SlotMeta &meta = slotMeta_[outcome.slot];
                if (meta.messageId != kNoMessage && !meta.queuedJournaled) {
                    const QueuedMessage *message = messageById(meta.messageId);
                    if (message != nullptr) {
                        meta.queuedJournaled = true;
                        onTransmitResult(meta.messageId, meta.frameCounter, message->dst,
                                         message->seq, 2 /* queued */,
                                         arq_.attemptsOf(outcome.slot), 0, 0);
                        queue_.setState(meta.messageId, MessageState::Queued,
                                        arq_.attemptsOf(outcome.slot), outcome.releaseUnix);
                    }
                }
            }
            return;
        }

        if (outcome.action == link::ArqAction::Transmit) {
            size_t frameLen = 0;
            const uint8_t *frame = arq_.frameOf(outcome.slot, &frameLen);
            if (frame == nullptr || frameLen == 0) {
                return;
            }
            SlotMeta &meta = slotMeta_[outcome.slot];
            meta.queuedJournaled = false;

            const uint8_t attempts = arq_.attemptsOf(outcome.slot); // already counts this one
            if (attempts > 1) {
                /*
                 * A retry is REBUILT under a fresh counter, with the RETRY flag,
                 * not replayed byte for byte: the receiver's replay window
                 * rejects a repeated counter as an attack, and only the seq
                 * history -- same seq, new counter -- recognises a retry and
                 * re-ACKs it (link/replay.h).
                 */
                uint8_t rebuilt[link::kMaxFrameBytes];
                size_t rebuiltLen = 0;
                const QueuedMessage *message = messageById(meta.messageId);
                if (message != nullptr) {
                    rebuiltLen = buildFrame(message->dst, link::FrameType::Text,
                                            reinterpret_cast<const uint8_t *>(message->text),
                                            message->textLen, message->seq, true, true,
                                            rebuilt, sizeof(rebuilt));
                }
                if (rebuiltLen == 0 || !arq_.replaceFrame(outcome.slot, rebuilt, rebuiltLen)) {
                    ++buildFailures_;
                    // Burn the attempt; the ARQ schedules the next one or gives
                    // up. Anything else strands the slot (see below).
                    arq_.onTransmitComplete(outcome.slot, nowMs, false);
                    return;
                }
                frame = arq_.frameOf(outcome.slot, &frameLen);
            }

            ++txRequested_;
            if (radio_->transmit(frame, frameLen) != hal::RadioResult::Ok) {
                ++txRefused_;
                /*
                 * Tell the ARQ the attempt failed. Until 2026-08-31 this path
                 * returned silently with awaitingRadio still set, and a single
                 * Busy from the radio parked the entire transmit side for ever:
                 * poll() saw a slot "on the radio" whose completion was never
                 * going to come.
                 */
                arq_.onTransmitComplete(outcome.slot, nowMs, false);
                return;
            }
            txSlot_ = outcome.slot;
            adaptiveSf_.onTransmitStart(attempts > 0 ? static_cast<uint8_t>(attempts - 1u) : 0u);
            return; // one transmission at a time; the SX1262 has one modem
        }

        // Deliver: a message reached a terminal state.
        const SlotMeta meta = slotMeta_[outcome.slot];
        const MessageState state = outcome.state == link::DeliveryState::Delivered
                                       ? MessageState::Delivered
                                       : MessageState::Undelivered;
        if (meta.messageId != kNoMessage) {
            const QueuedMessage *message = messageById(meta.messageId);
            const uint16_t dst = message != nullptr ? message->dst : 0;
            const uint8_t seq = message != nullptr ? message->seq : 0;
            onTransmitResult(meta.messageId, meta.frameCounter, dst, seq,
                             state == MessageState::Delivered ? 0 : 1,
                             arq_.attemptsOf(outcome.slot),
                             meta.ackSeen ? meta.ackRssi : 0,
                             meta.ackSeen ? meta.ackSnr : 0);
            if (state == MessageState::Undelivered) {
                adaptiveSf_.onUndelivered();
            }
        }
        slotMeta_[outcome.slot] = SlotMeta{};
        arq_.clear(outcome.slot);
    }
}

#ifdef MAXL_BENCH
size_t Node::benchFrame(uint16_t dst, const uint8_t *payload, size_t length, uint8_t seq,
                        uint8_t *out, size_t capacity)
{
    // Same path, same counter supply, same crypto. ACK_REQ is set so the frame
    // takes part in the receiver's dedupe, like the real traffic it imitates.
    return buildFrame(dst, link::FrameType::Text, payload, length, seq, true, false, out,
                      capacity);
}

uint8_t Node::benchTransmit(const uint8_t *frame, size_t length)
{
    if (radio_ == nullptr || frame == nullptr || length == 0 || length > link::kMaxFrameBytes) {
        return static_cast<uint8_t>(ble::BridgeError::BadParam);
    }
    if (!transmitAllowed()) {
        return static_cast<uint8_t>(clock_.timeValid() ? ble::BridgeError::NoKey
                                                       : ble::BridgeError::NoTime);
    }
    if (benchTxInFlight_ || ackTxInFlight_ || txSlot_ < link::kMaxOutstanding) {
        return static_cast<uint8_t>(ble::BridgeError::Busy);
    }

    const uint16_t preamble =
        link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
    const uint32_t airtimeUs =
        link::timeOnAirUs(currentSf_, static_cast<uint8_t>(length), preamble);
    if (budget_.earliestLegalTxDelayMs(activeBand(), airtimeUs) > 0) {
        return static_cast<uint8_t>(ble::BridgeError::BudgetExhausted);
    }
    if (radio_->transmit(frame, length) != hal::RadioResult::Ok) {
        return static_cast<uint8_t>(ble::BridgeError::Busy);
    }
    benchTxInFlight_ = true;
    benchTxFrameLen_ = length;
    return 0;
}
#endif // MAXL_BENCH

void Node::onTransmitComplete(hal::RadioResult result)
{
#ifdef MAXL_BENCH
    if (benchTxInFlight_) {
        benchTxInFlight_ = false;
        if (result == hal::RadioResult::Ok) {
            const uint16_t preamble =
                link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
            budget_.recordTransmission(
                activeBand(),
                link::timeOnAirUs(currentSf_, static_cast<uint8_t>(benchTxFrameLen_), preamble));
            journalBudget();
        }
        if (radio_ != nullptr) {
            radio_->startReceiveDutyCycle(currentSf_ > 10 ? 12 : 8);
        }
        return;
    }
#endif

    if (ackTxInFlight_) {
        ackTxInFlight_ = false;
        if (result == hal::RadioResult::Ok) {
            // An ACK spends airtime like anything else. Same computed figure,
            // same single path through the budget (CLAUDE.md 6) -- and with the
            // preamble this ACK actually carried, which serviceAcks chose.
            budget_.recordTransmission(activeBand(),
                                       link::timeOnAirUs(currentSf_,
                                                         static_cast<uint8_t>(ackTxFrameLen_),
                                                         ackTxPreamble_));
            journalBudget();
            ++ackTxTotal_;
            if (ackTxIndex_ < kMaxPendingAcks) {
                pendingAcks_[ackTxIndex_] = PendingAck{};
            }
        }
        // On failure the pending entry stays and the next pump offers it again.
        if (radio_ != nullptr) {
            // Back to the sniff preamble: the next thing we transmit is not an
            // ACK unless serviceAcks says so again, and the receiver we are
            // about to arm must match what our peer sends us.
            radio_->setPreambleLength(
                link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs));
            radio_->startReceiveDutyCycle(currentSf_ > 10 ? 12 : 8);
        }
        return;
    }

    if (txSlot_ >= link::kMaxOutstanding) {
        return;
    }
    const size_t slot = txSlot_;
    txSlot_ = link::kMaxOutstanding;

    /*
     * Charge the duty cycle budget. This is the only place it happens, and until
     * 2026-08-31 it happened nowhere at all.
     *
     * link::Budget was written, persisted, and covered by unit tests and the
     * two-node simulation -- and `recordTransmission` was called from the tests
     * and from nothing in src/. So `usedMs` stayed at zero for ever, the rolling
     * hour never filled, `earliestLegalTx` never moved, and the node would have
     * transmitted straight past the 10 % it is legally held to. Node A, seven
     * frames on the air, `budget.used_ms = 0`.
     *
     * CLAUDE.md 1.2 calls the duty cycle "legally binding and enforced in
     * firmware", and section 6 puts it plainly: "Anything that transmits goes
     * through the budget tracker. There is no second path." This is that path.
     *
     * The COMPUTED airtime, not the radio's measured one. Three reasons: the
     * measurement lives on Sx1262Radio and not on hal::IRadioLink, so reaching
     * it would drag a driver type through the interface D3 exists to keep clean;
     * the computed figure is the one both sides of the link agree on, because
     * both derive it from link/airtime; and gate 2.10 is precisely the
     * comparison of the two, which needs them to stay separate numbers.
     *
     * Charged on success only. A transmission the radio never completed spent no
     * airtime, and charging it would make the budget refuse on behalf of a frame
     * that was never on the air.
     */
    if (result == hal::RadioResult::Ok) {
        size_t frameLen = 0;
        if (arq_.frameOf(slot, &frameLen) != nullptr && frameLen > 0) {
            const uint16_t preamble =
                link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs);
            budget_.recordTransmission(
                activeBand(),
                link::timeOnAirUs(currentSf_, static_cast<uint8_t>(frameLen), preamble));
            // "EVT_BUDGET is emitted when the budget changes" -- and this is
            // where it changes (docs/bridge-protocol.md section 4).
            journalBudget();
        }
    }

    const bool wantedAck = arq_.ackRequestedOf(slot);
    arq_.onTransmitComplete(slot, clock_.monotonicMs(), result == hal::RadioResult::Ok);

    if (radio_ == nullptr) {
        return;
    }
    /*
     * If that frame asked for an ACK, listen continuously for it.
     *
     * hal/i_radio_link.h has described this window since the interface existed
     * and nothing opened it, so every ACK had to carry the full sniff preamble
     * to reach us -- 123 symbols instead of 8 at SF9/500 ms, which is a 690 ms
     * frame instead of 185 ms and a 6.2 s duty cycle lockout on our peer
     * instead of 1.7 s. Two nodes whose lockouts are both budget-driven and
     * therefore identical fall into step and transmit over each other; on the
     * bench on 2026-09-01 node A took four ACKs and then none for five minutes.
     *
     * The window is bounded: link::ackWindowMs, the same figure our peer uses to
     * decide whether the short preamble can still reach us.
     */
    if (wantedAck && result == hal::RadioResult::Ok) {
        inAckWindow_ = true;
        ++ackWindowOpens_;
        ackWindowUntilMs_ = clock_.monotonicMs() + link::ackWindowMs(currentSf_);
        /*
         * The preamble is set on BOTH sides of a link, not only the sending
         * one: it is a modem parameter, and the receiver's SetPacketParams
         * tells the chip how long a preamble to expect. Opening the window
         * without this leaves us listening continuously for a 123-symbol
         * preamble while our peer sends 8 -- which is how the first attempt at
         * this took node A from four ACKs to none at all, on 2026-09-01.
         */
        radio_->setPreambleLength(link::kStandardPreambleSymbols);
        radio_->startReceiveContinuous();
        return;
    }
    // Back to sniffing. A radio left in transmit state hears nothing.
    closeAckWindow();
}

void Node::closeAckWindow()
{
    inAckWindow_ = false;
    ackWindowUntilMs_ = 0;
    if (radio_ != nullptr) {
        // Back to the preamble the sniff interval demands, on the receiver as
        // well as the sender -- the peer's next data frame carries it.
        radio_->setPreambleLength(
            link::preambleSymbolsForInterval(currentSf_, config_.sniffIntervalMs));
        radio_->startReceiveDutyCycle(currentSf_ > 10 ? 12 : 8);
    }
}

void Node::onFrameReceived(const uint8_t *data, size_t len, const hal::RxInfo &info)
{
    link::Header header{};
    if (link::inspect(data, len, &header) != link::FrameError::None) {
        return;
    }
    if (header.netId != netId_) {
        // Another network on the same channel. Not an error, not ours.
        return;
    }
    if (header.dst != nodeId_ && header.dst != link::kBroadcastAddress) {
        return;
    }

    uint8_t plain[link::kMaxPayloadBytes];
    uint8_t usedSlot = 0;
    const size_t bodyLen = len - link::kHeaderBytes;
    if (crypto_.decrypt(data, data + link::kHeaderBytes, bodyLen, plain, sizeof(plain),
                        &usedSlot)
        != link::CryptoError::None) {
        /*
         * A failed MIC is a frame that never arrived (hal/i_radio_link.h), and
         * it is counted rather than reported. Gate 2.1 asks for zero of these
         * over a hundred frames, so a number that can be read off the device is
         * the whole point of keeping it.
         */
        ++micFailures_;
        return;
    }

    /*
     * Replay comes after authentication, never before. Updating a peer's
     * high-water mark from an unauthenticated frame would let anyone lock the
     * real peer out (link/replay.h says so at the call site too).
     *
     * Only ACK_REQ frames take part in the (src, seq) dedupe -- they are the
     * only ones ever retried, and recording an ACK's seq would poison the
     * history against the peer's own later messages (link/replay.h).
     */
    const bool seqParticipates = (header.flags & link::kFlagAckReq) != 0;
    const link::ReplayVerdict verdict =
        replay_.admit(header.src, header.counter, header.seq, seqParticipates);

    if (verdict == link::ReplayVerdict::Duplicate) {
        /*
         * An ARQ retry whose ACK went missing. CLAUDE.md 2.4 step 2: deduped --
         * the application sees the message once -- but the ACK still has to go
         * out, or the sender burns its remaining attempts on a message we
         * already have.
         */
        ++duplicatesReacked_;
        if (header.dst == nodeId_ && header.type != link::FrameType::Ack) {
            queueAck(header.src, header.seq, info.rssiDbm, info.snrDb, clock_.monotonicMs());
        }
        return;
    }
    if (verdict != link::ReplayVerdict::Accept) {
        ++replaysRejected_;
        return;
    }

    const size_t payloadLen = link::payloadLength(len);
    recordReceivedFrame(header.counter, header.src, static_cast<uint8_t>(header.type),
                        info.rssiDbm, info.snrDb, currentSf_, plain, payloadLen);

    if (header.type == link::FrameType::Ack) {
        link::AckPayload ack{};
        if (link::decodeAck(plain, payloadLen, &ack)) {
            /*
             * Check the ACK's sender against the message's destination BEFORE
             * anything is marked delivered: an ACK is matched by seq alone, and
             * a third node acknowledging someone else's seq must not close ours.
             */
            const size_t slot = arq_.slotOfActiveSeq(ack.seq);
            if (slot < link::kMaxOutstanding) {
                const QueuedMessage *message = messageById(slotMeta_[slot].messageId);
                if (message != nullptr && message->dst == header.src) {
                    slotMeta_[slot].ackRssi = ack.rssiDbm;
                    slotMeta_[slot].ackSnr = ack.snrDb;
                    slotMeta_[slot].ackSeen = true;
                    // Kept at node level too: gate 2.4 compares these against
                    // the other board's report, and slotMeta is cleared as soon
                    // as the message reaches a terminal state.
                    lastAckRssi_ = ack.rssiDbm;
                    lastAckSnr_ = ack.snrDb;
                    ++ackSeenTotal_;
                    const uint8_t attempts = arq_.attemptsOf(slot);
                    arq_.onAckReceived(ack.seq, clock_.monotonicMs());
                    // What the peer measured about OUR frame is what adaptive SF
                    // adapts on (CLAUDE.md 2.5).
                    adaptiveSf_.onDelivered(
                        attempts > 0 ? static_cast<uint8_t>(attempts - 1u) : 0u, ack.snrDb);
                    peers_.acked(header.src);
                    /*
                     * The answer is in. Nothing is waiting on the continuous
                     * window any more, and holding it open is pure current --
                     * close it rather than letting it time out.
                     */
                    if (inAckWindow_) {
                        closeAckWindow();
                    }
                }
            }
        }
        return;
    }

    if (seqParticipates && header.dst == nodeId_) {
        // CLAUDE.md 2.4 step 2: validated, deduped, delivered -- and now the
        // ACK, charged to OUR budget and queued when the budget says not yet.
        queueAck(header.src, header.seq, info.rssiDbm, info.snrDb, clock_.monotonicMs());
    }
}

// --- radio side ----------------------------------------------------------

void Node::journalBudget()
{
    uint8_t body[kBudgetBytes];
    if (budget(body, sizeof(body)) == kBudgetBytes) {
        journalEvent(static_cast<uint8_t>(ble::EventCode::Budget), body, kBudgetBytes);
    }
}

void Node::journalStatusIfChanged()
{
    uint8_t body[kStatusBytes];
    if (status(body, sizeof(body)) != kStatusBytes) {
        return;
    }
    if (body[16] == lastStatusFlags_) {
        return;
    }
    if (journalEvent(static_cast<uint8_t>(ble::EventCode::Status), body, kStatusBytes)) {
        lastStatusFlags_ = body[16];
    }
}

void Node::onFix(int32_t latitudeE7, int32_t longitudeE7, int16_t altitudeM, uint8_t hdopTenths,
                 uint8_t fixAgeS, uint8_t satellites)
{
    fixPending_ = false;

    // EVT_FIX: lat i32, lon i32, alt i16, hdop u8, fixAge u8, satellites u8.
    uint8_t body[13];
    putU32(body, static_cast<uint32_t>(latitudeE7));
    putU32(body + 4, static_cast<uint32_t>(longitudeE7));
    body[8] = static_cast<uint8_t>(static_cast<uint16_t>(altitudeM));
    body[9] = static_cast<uint8_t>(static_cast<uint16_t>(altitudeM) >> 8);
    body[10] = hdopTenths == 0 ? 255 : hdopTenths; // D11: unknown is 255, never 0
    body[11] = fixAgeS;
    body[12] = satellites;
    journalEvent(static_cast<uint8_t>(ble::EventCode::Fix), body, sizeof(body));
}

bool Node::journalEvent(uint8_t opcode, const uint8_t *body, size_t length)
{
    uint32_t counter = 0;
    if (!counter_.next(&counter)) {
        // No counter means no journal entry and, per 2.1, no transmission
        // either. Losing the event is the lesser failure.
        return false;
    }
    return journal_.append(counter, opcode, body, length);
}

void Node::recordReceivedFrame(uint32_t frameCounter, uint16_t src, uint8_t frameType,
                               int16_t rssi, int8_t snr, uint8_t sf, const uint8_t *payload,
                               size_t length)
{
    // CLAUDE.md 2.5: any received frame counts as contact.
    peers_.heard(src, rssi, snr, sf, clock_.unixSeconds());
    adaptiveSf_.onPeerHeard();

    /*
     * EVT_FRAME_RX: counter u32, src u16, type u8, rssi i16, snr i8, len u8,
     * payload. The counter is the SENDER's, from the received header -- until
     * 2026-08-31 this wrote our own counter_.peek(), which made the (src,
     * counter) correlation the dashboard is built on point at nothing.
     */
    uint8_t body[11 + link::kMaxTextBytes + 16];
    if (length > sizeof(body) - 11) {
        return;
    }
    putU32(body, frameCounter);
    putU16(body + 4, src);
    body[6] = frameType;
    body[7] = static_cast<uint8_t>(rssi);
    body[8] = static_cast<uint8_t>(static_cast<uint16_t>(rssi) >> 8);
    body[9] = static_cast<uint8_t>(snr);
    body[10] = static_cast<uint8_t>(length);
    if (length > 0) {
        memcpy(body + 11, payload, length);
    }
    journalEvent(static_cast<uint8_t>(ble::EventCode::FrameRx), body, 11 + length);
}

void Node::onTransmitResult(uint32_t messageId, uint32_t frameCounter, uint16_t dst, uint8_t seq,
                            uint8_t result, uint8_t attempts, int16_t rssi, int8_t snr)
{
    static constexpr uint8_t kDelivered = 0;
    static constexpr uint8_t kUndelivered = 1;
    static constexpr uint8_t kQueued = 2;

    MessageState state = MessageState::InFlight;
    if (result == kDelivered) {
        state = MessageState::Delivered;
    } else if (result == kUndelivered) {
        state = MessageState::Undelivered;
    } else if (result == kQueued) {
        state = MessageState::Queued;
    }
    queue_.setState(messageId, state, attempts);
    // Counted here rather than read off the queue, because the queue evicts
    // delivered entries once it is full and its count stops being a history.
    if (result == kDelivered) {
        ++deliveredTotal_;
    } else if (result == kUndelivered) {
        ++undeliveredTotal_;
    }
    if (result == kDelivered) {
        peers_.acked(dst);
    }

    /*
     * EVT_FRAME_TX_RESULT: counter u32, dst u16, seq u8, result u8, attempts u8,
     * rssi i16, snr i8. Each transition is its own journal entry (decision D10).
     *
     * The counter field carries the FRAME counter of the message's first
     * transmission -- until 2026-08-31 it carried the local queue id, which no
     * other node and no server table has ever heard of.
     */
    uint8_t body[12];
    putU32(body, frameCounter);
    putU16(body + 4, dst);
    body[6] = seq;
    body[7] = result;
    body[8] = attempts;
    body[9] = static_cast<uint8_t>(rssi);
    body[10] = static_cast<uint8_t>(static_cast<uint16_t>(rssi) >> 8);
    body[11] = static_cast<uint8_t>(snr);
    journalEvent(static_cast<uint8_t>(ble::EventCode::FrameTxResult), body, sizeof(body));
}

} // namespace app
