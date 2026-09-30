/*
 * The node: where the four layers finally meet.
 *
 * Until this file existed, link/, app/, ble/ and hal/ were four sets of modules
 * that had never been introduced. Each one compiled and each one was tested, and
 * that is not the same as the interfaces fitting together -- an interface that
 * nobody implements is a guess about what will be needed.
 *
 * Node is what implements ble::IHost, so every command in
 * docs/bridge-protocol.md section 3 arrives here and is answered from the real
 * counter, the real budget and the real queue. It takes hal:: interfaces and
 * nothing concrete, so the whole of it runs on a host against the fakes.
 *
 * What it does NOT do yet is drive the radio. That loop belongs with a working
 * hal/radio_sx1262 and two devices to point at each other, and writing it now
 * would be writing against an interface nothing has ever exercised.
 */

#ifndef MAXL_APP_NODE_H
#define MAXL_APP_NODE_H

#include <stddef.h>
#include <stdint.h>

#include "app/journal.h"
#include "app/message_queue.h"
#include "app/peer_state.h"
#include "app/scheduler.h"
#include "ble/i_host.h"
#include "hal/i_block_store.h"
#include "hal/i_clock.h"
#include "hal/i_key_store.h"
#include "hal/i_radio_link.h"
#include "link/adaptive_sf.h"
#include "link/airtime.h"
#include "link/arq.h"
#include "link/band.h"
#include "link/budget.h"
#include "link/counter.h"
#include "link/crypto.h"
#include "link/frame.h"
#include "link/replay.h"

namespace app {

/// The settings that travel as config TLVs (docs/bridge-protocol.md section 3).
struct NodeConfig {
    uint16_t sniffIntervalMs = 2000;   ///< CLAUDE.md 2.3 default
    uint8_t band = 0;                  ///< 0 = g3, 1 = g1
    uint8_t sfMode = 0;                ///< 0 = adaptive, 1 = fixed
    uint8_t fixedSf = 9;
    int8_t txPowerDbm = 22;
    uint16_t telemetryIntervalS = 600;
    uint16_t beaconIntervalS = 900;
    uint16_t gnssFixTimeoutS = 120;
};

class Node : public ble::IHost, public hal::IRadioObserver {
public:
    Node(hal::IClock &clock, hal::IBlockStore &store, hal::IKeyStore &keys);

    /**
     * Restore everything that survives a power cycle and decide whether this
     * node may transmit at all.
     *
     * Returns false when the counter or the budget could not be restored.
     * CLAUDE.md 2.1: "If the counter cannot be persisted, the device must refuse
     * to transmit." 1.2 says the same about the budget and about an invalid
     * clock. A false here is not a warning.
     */
    bool begin(uint16_t nodeId);

    /**
     * Hand the node a radio.
     *
     * Separate from begin() because everything above works without one: the GATT
     * server answers, the queue persists, the journal fills. A node with no radio
     * is a node that cannot transmit, not a node that cannot run -- which is also
     * exactly the state the tests use to exercise the rest.
     */
    bool attachRadio(hal::IRadioLink &radio, link::IJitterSource &jitter);

    /// True when a frame may legally and safely be sent right now.
    bool transmitAllowed() const;

    void tick(uint32_t nowMs);

    /**
     * Drive the transmit path: promote queued messages into the ARQ, hand it
     * whatever it says to send, and report what reached a terminal state.
     *
     * Called from the main loop alongside tick(). Separate because a node
     * without a radio still has to tick.
     */
    void pumpRadio(uint32_t nowMs);

    // --- hal::IRadioObserver ---------------------------------------------
    void onFrameReceived(const uint8_t *data, size_t len, const hal::RxInfo &info) override;
    void onTransmitComplete(hal::RadioResult result) override;

    // --- ble::IHost ------------------------------------------------------
    ble::DeviceInfo info() const override;
    size_t status(uint8_t *out, size_t max) const override;
    size_t budget(uint8_t *out, size_t max) const override;
    uint8_t sendText(uint16_t dst, const uint8_t *text, size_t length) override;
    size_t fetchQueue(uint32_t sinceCounter, ble::QueuedEvent *out, size_t max) override;
    void ackQueue(uint32_t upToCounter) override;
    uint8_t setConfig(uint32_t configVersion, const uint8_t *tlvs, size_t length,
                      uint32_t *appliedMask, uint8_t *unapplied, size_t *unappliedCount) override;
    size_t getConfig(uint8_t *out, size_t max) const override;
    uint8_t setTime(uint32_t unixSeconds) override;
    uint8_t requestFix(uint16_t timeoutSeconds) override;
    uint8_t provisionKey(uint8_t slot, uint8_t netId, const uint8_t key[16]) override;
    uint8_t rotateKey(uint8_t newSlot) override;
    uint8_t factoryReset() override;
    uint8_t linkTest(uint16_t dst, uint8_t count, uint8_t sf) override;

    // --- accessors, for the UI and the tests ------------------------------
    const MessageQueue &queue() const { return queue_; }
    const Journal &journal() const { return journal_; }
    const PeerTable &peers() const { return peers_; }
    const NodeConfig &config() const { return config_; }
    /// Asked of the store, never remembered. A flag that said yes while the
    /// flash said no would let the node believe it can transmit when it cannot.
    uint8_t activeKeySlot() const { return activeSlot_; }
    uint8_t netId() const { return netId_; }
    bool keyProvisioned() const { return keys_.hasAnyKey(); }

    /// Whether a GNSS fix has been asked for and not yet answered.
    bool fixPending() const { return fixPending_; }

    /*
     * A GNSS fix arrived (driven from the main loop, which owns the receiver).
     *
     * Journals EVT_FIX -- docs/bridge-protocol.md section 4 makes journaling
     * binding for every event type it lists, and until 2026-08-31 this one was
     * never produced at all. `hdopTenths` follows D11: tenths, 255 means
     * unusable or unknown -- and 0, which the NMEA parser leaves for an absent
     * field, is mapped to 255 here so a missing figure cannot read as a perfect
     * fix.
     */
    void onFix(int32_t latitudeE7, int32_t longitudeE7, int16_t altitudeM, uint8_t hdopTenths,
               uint8_t fixAgeS, uint8_t satellites);

    /// Record a received frame: peer state, and a journal entry for the phone.
    /// `frameCounter` is the counter from the received frame's header -- the
    /// SENDER's counter, which is what lets the server correlate this event with
    /// the sender's own EVT_FRAME_TX_RESULT over (src, counter).
    /// Public because the tests drive it directly, without a radio.
    void recordReceivedFrame(uint32_t frameCounter, uint16_t src, uint8_t frameType,
                             int16_t rssi, int8_t snr, uint8_t sf, const uint8_t *payload,
                             size_t length);

    /// Frames that failed authentication. CLAUDE.md 2.4 treats a bad MIC exactly
    /// like a frame that never arrived, so this is the only place it is visible.
    uint32_t micFailures() const { return micFailures_; }
    uint32_t replaysRejected() const { return replaysRejected_; }

    /// ARQ retries that arrived again because our ACK was lost. Each one was
    /// re-acknowledged and NOT delivered to the application a second time.
    /// Counted apart from replaysRejected() -- a lost ACK is link weather, a
    /// replayed counter is an attack.
    uint32_t duplicatesReacked() const { return duplicatesReacked_; }

    /// The adaptive-SF state, for the PEERS screen and the bring-up reports.
    const link::AdaptiveSf &adaptiveSf() const { return adaptiveSf_; }

    /// The spreading factor the modem is actually configured to right now.
    uint8_t currentSf() const { return currentSf_; }

    /*
     * The frame counter, read-only (gate 2.6).
     *
     * peek() is the next value a draw would return; reservedUpTo() is the flash
     * high-water mark a reboot resumes from. Bring-up 18 measured the JOURNAL's
     * highest counter before these existed and proved nothing about either.
     */
    uint32_t frameCounterPeek() const { return counter_.peek(); }
    uint32_t frameCounterReservedUpTo() const { return counter_.reservedUpTo(); }

#ifdef MAXL_BENCH
    /*
     * The bench seam (docs/test-plan.md, gates 2.2 and 2.3).
     *
     * A tamper test needs a chosen buffer on the air: a valid frame with one
     * bit flipped, or a byte-exact repeat of one already accepted.
     * Node::buildFrame stays private -- it is the only thing between the
     * application and a frame without a counter -- so the bench gets this pair
     * instead, which takes the SAME path: benchFrame draws a real counter and
     * encrypts with the real key, and benchTransmit goes through the budget,
     * because "anything that transmits goes through the budget tracker. There
     * is no second path" (CLAUDE.md 6) is not suspended on a bench. Bring-up
     * builds only; build_guard.h fails a release that defines MAXL_BENCH.
     */
    size_t benchFrame(uint16_t dst, const uint8_t *payload, size_t length, uint8_t seq,
                      uint8_t *out, size_t capacity);
    /// Transmit a prepared frame. 0 on start, else a BridgeError code --
    /// BudgetExhausted means ask again later, exactly like everything else.
    uint8_t benchTransmit(const uint8_t *frame, size_t length);
    bool benchTxBusy() const { return benchTxInFlight_; }
#endif

    /*
     * Where a queued message stops, if it stops.
     *
     * The send path has four places it can quietly decline -- a frame that will
     * not build, an ARQ window with no room, a budget that says not yet, a radio
     * that refuses the buffer -- and on 2026-08-31 a node sat with 24 messages
     * queued, transmitAllowed() true and a budget of zero for half an hour
     * without any of them saying so. These make the silent one nameable.
     *
     * They cost four words of RAM and are read by bring-up 17.
     */
    uint32_t promoteSeen() const { return promoteSeen_; }
    uint32_t buildFailures() const { return buildFailures_; }
    uint32_t arqRejects() const { return arqRejects_; }
    uint32_t txRequested() const { return txRequested_; }
    uint32_t txRefused() const { return txRefused_; }

    /**
     * How many of our own messages reached each terminal state, counted as the
     * transition happens.
     *
     * Not the same question as queue().countInState(). The queue holds
     * kQueueCapacity == 24 entries and evicts the oldest DELIVERED one to make
     * room (message_queue.cpp, gate 3.2), so its delivered count saturates in
     * the low twenties and then stops rising. Any gate that compares it against
     * a message target above the capacity -- docs/test-plan.md asks gate 2.1 for
     * 100 frames -- reports a failure on a run in which every single message was
     * acknowledged. These counters are the history; the queue is the working set.
     *
     * Two limits, because the paragraph above would otherwise promise more than
     * they deliver:
     *
     * - They live in RAM. The queue's own state is restored from flash on boot,
     *   these are not, so they count within one power cycle. Gate 2.1's forced
     *   power cycle restarts them at zero -- right for bring-up 19, which erases
     *   the queue region at boot anyway, and wrong for a lifetime total on the
     *   STATUS characteristic or the dashboard.
     * - "Delivered" means the message reached MessageState::Delivered, which for
     *   a broadcast is the moment it was sent: link/arq.cpp marks anything
     *   without ACK_REQ delivered at once, and CLAUDE.md 2.4 says broadcast
     *   frames never request an ACK. So this is not a count of acknowledgements.
     *   ackSeenTotal() below is.
     */
    uint32_t deliveredTotal() const { return deliveredTotal_; }
    uint32_t undeliveredTotal() const { return undeliveredTotal_; }

    /**
     * What the peer's most recent ACK said it heard from us.
     *
     * docs/test-plan.md gate 2.4 is not "an ACK arrived" but "RSSI/SNR in the
     * ACK match the receiver's log", and until now nothing carried those values
     * out of the ARQ: they went into the journal event and nowhere else, so the
     * comparison the gate asks for could not be made from a bring-up report.
     *
     * ackSeenTotal() counts ACKs that verified, so zero distinguishes "never
     * acknowledged" from "acknowledged at 0 dBm" without a second flag.
     */
    int16_t lastAckRssi() const { return lastAckRssi_; }
    int8_t lastAckSnr() const { return lastAckSnr_; }
    uint32_t ackSeenTotal() const { return ackSeenTotal_; }

    /**
     * ACKs this node put on the air.
     *
     * txRequested() does not count them -- it is incremented only in the ARQ
     * data path -- so a receiving node reports tx.requested = 0 however hard it
     * is working, and "the ACK path is broken" and "the ACK path is fine and
     * the ACKs are not arriving" look identical from its report. They are not
     * the same problem and this is what tells them apart.
     */
    uint32_t ackTxTotal() const { return ackTxTotal_; }

    /// How many continuous receive windows were opened, and whether one is open
    /// now. Without them a run cannot say whether the window mechanism ran at
    /// all -- which cost a whole bench cycle on 2026-09-01.
    uint32_t ackWindowOpens() const { return ackWindowOpens_; }
    bool inAckWindow() const { return inAckWindow_; }

    /// Record the outcome of one of our own transmissions. `frameCounter` is the
    /// counter of the message's FIRST transmission: it stays the same across the
    /// queued/delivered/undelivered transitions of one message, which is what
    /// docs/bridge-protocol.md section 4 means by "state 2 may be followed by 0
    /// or 1 for the same counter". (A retry draws a fresh counter on air, so a
    /// message delivered on a retry will not match the receiver's EVT_FRAME_RX
    /// counter -- a known, documented imperfection of the correlation.)
    void onTransmitResult(uint32_t messageId, uint32_t frameCounter, uint16_t dst, uint8_t seq,
                          uint8_t result, uint8_t attempts, int16_t rssi, int8_t snr);

private:
    hal::IClock &clock_;
    hal::IBlockStore &store_;
    hal::IKeyStore &keys_;

    link::FrameCounter counter_;
    link::DutyCycleBudget budget_;
    link::Crypto crypto_;
    link::Arq arq_;
    link::ReplayGuard replay_;
    hal::IRadioLink *radio_ = nullptr;
    MessageQueue queue_;
    Journal journal_;
    PeerTable peers_;
    Scheduler scheduler_;
    NodeConfig config_;

    uint16_t nodeId_ = 0;
    bool ready_ = false;
    bool fixPending_ = false;
    uint8_t netId_ = 0;
    uint8_t activeSlot_ = 0;  ///< which key slot frames are sent under
    uint16_t fixTimeoutS_ = 0;
    uint32_t appliedConfigVersion_ = 0;

    /// Buffer the journal hands out through fetchQueue. One at a time is enough:
    /// GET_QUEUE reads a batch and the entries are copied into it before the
    /// next call.
    mutable JournalEntry fetched_[32];

    /// Which message occupies which ARQ slot, and what has happened to it since.
    static constexpr uint32_t kNoMessage = 0;
    struct SlotMeta {
        uint32_t messageId = kNoMessage;
        uint32_t frameCounter = 0;  ///< counter of the first transmission
        int16_t ackRssi = 0;        ///< what the peer's ACK reported about us
        int8_t ackSnr = 0;
        bool ackSeen = false;
        bool queuedJournaled = false;  ///< current budget-blocked episode reported
    };
    SlotMeta slotMeta_[link::kMaxOutstanding] = {};

    /*
     * ACKs waiting for the budget (CLAUDE.md 2.4 step 2: "A budget-blocked ACK
     * is queued"). One slot per peer: a retry that arrives before our ACK went
     * out replaces the pending ACK rather than stacking behind it -- the sender
     * only needs the newest answer.
     */
    struct PendingAck {
        bool used = false;
        uint16_t dst = 0;
        uint8_t seq = 0;
        int16_t rssiDbm = 0;
        int8_t snrDb = 0;
        /// When the frame being acknowledged arrived. The sender's continuous
        /// receive window starts at the same instant, so this decides whether
        /// the short preamble still reaches it (link::ackWindowMs).
        uint32_t heardAtMs = 0;
    };
    static constexpr size_t kMaxPendingAcks = 4;
    PendingAck pendingAcks_[kMaxPendingAcks] = {};
    bool ackTxInFlight_ = false;   ///< the frame on the radio is an ACK, not an ARQ slot
    size_t ackTxIndex_ = 0;        ///< which pendingAcks_ entry is on the radio
    size_t ackTxFrameLen_ = 0;     ///< for charging the budget on completion
    uint16_t ackTxPreamble_ = link::kStandardPreambleSymbols; ///< and with which preamble

    /*
     * The continuous receive window after a frame that asked for an ACK.
     *
     * hal/i_radio_link.h has described this since the interface was written --
     * "the short window after a transmission that requested an ACK, where
     * sniffing would cost more latency than it saves" -- and nothing ever
     * called startReceiveContinuous(). So every ACK had to carry the full sniff
     * preamble to reach a sniffing sender, which at SF9/500 ms is 123 symbols
     * against 8: a 690 ms frame instead of 185 ms, and a 6.2 s duty cycle
     * lockout on the receiver instead of 1.7 s. CLAUDE.md 1.4 tabulates the ACK
     * at the short preamble, so the table assumed this window all along.
     */
    uint32_t ackWindowUntilMs_ = 0;
    bool inAckWindow_ = false;
    uint32_t ackWindowOpens_ = 0;

    /// Leave the continuous ACK window and go back to sniffing.
    void closeAckWindow();

    link::AdaptiveSf adaptiveSf_;

    uint32_t micFailures_ = 0;
    uint32_t replaysRejected_ = 0;
    uint32_t duplicatesReacked_ = 0;
    uint32_t promoteSeen_ = 0;
    uint32_t buildFailures_ = 0;
    uint32_t arqRejects_ = 0;
    uint32_t txRequested_ = 0;
    uint32_t txRefused_ = 0;
    uint32_t deliveredTotal_ = 0;
    uint32_t undeliveredTotal_ = 0;
    int16_t lastAckRssi_ = 0;
    int8_t lastAckSnr_ = 0;
    uint32_t ackSeenTotal_ = 0;
    uint32_t ackTxTotal_ = 0;
    uint8_t currentSf_ = 9;
    size_t txSlot_ = link::kMaxOutstanding;
    uint8_t lastStatusFlags_ = 0xFF; ///< 0xFF = no EVT_STATUS journaled yet

#ifdef MAXL_BENCH
    bool benchTxInFlight_ = false;
    size_t benchTxFrameLen_ = 0;
#endif

    link::Band activeBand() const;
    bool journalEvent(uint8_t opcode, const uint8_t *body, size_t length);
    bool loadKeys();
    size_t buildFrame(uint16_t dst, link::FrameType type, const uint8_t *payload, size_t length,
                      uint8_t seq, bool ackRequested, bool retry, uint8_t *out, size_t capacity,
                      uint32_t *counterOut = nullptr);
    void promoteQueued(uint32_t nowMs);
    void queueAck(uint16_t dst, uint8_t seq, int16_t rssiDbm, int8_t snrDb, uint32_t nowMs);
    /// Journal EVT_BUDGET. Called after every recordTransmission -- the
    /// protocol says the event marks the budget CHANGING, and that is the only
    /// place it changes upward.
    void journalBudget();
    /// Journal EVT_STATUS when the flags byte moved. The protocol never says
    /// when EVT_STATUS is emitted; journaling every tick would drown the
    /// journal in battery jitter, so the state transitions -- time valid, key
    /// provisioned, GNSS power -- are what get recorded.
    void journalStatusIfChanged();
    /// Try to put one pending ACK on the air. Returns true if the radio is now
    /// busy with it (or was already busy), so the caller holds data frames back.
    bool serviceAcks(uint32_t nowMs);
    const QueuedMessage *messageById(uint32_t id) const;
    /// Reconfigure the modem to `sf` if it is safe to do so right now.
    void applySpreadingFactor(uint8_t sf);
};

} // namespace app

#endif // MAXL_APP_NODE_H
