/*
 * A BLE link and an application, in memory.
 *
 * The transport records every chunk it was asked to notify and can be told
 * whether the connection is bonded -- which is the whole of gates 6.3 and 6.4.
 * The host records what it was asked to do, so a test can assert that an
 * unauthorised command reached nothing at all rather than merely returning an
 * error after having already acted.
 */

#ifndef MAXL_TEST_FAKE_BLE_H
#define MAXL_TEST_FAKE_BLE_H

#include <cstring>
#include <vector>

#include "ble/i_host.h"
#include "hal/i_ble_transport.h"

namespace fakes {

class FakeBleTransport : public hal::IBleTransport {
public:
    bool notify(const uint8_t *chunk, size_t length) override
    {
        if (!linkUp) {
            return false;
        }
        chunks.emplace_back(chunk, chunk + length);
        return true;
    }

    uint16_t mtu() const override { return negotiatedMtu; }
    bool bonded() const override { return isBonded; }
    bool connected() const override { return linkUp; }

    void publishStatus(const uint8_t *body, size_t length) override
    {
        published.assign(body, body + length);
        ++statusPublishes;
    }

    /// Reassemble everything notified so far back into whole messages, so a test
    /// can assert on what the phone would actually have seen.
    std::vector<std::vector<uint8_t>> messages() const
    {
        std::vector<std::vector<uint8_t>> out;
        std::vector<uint8_t> current;
        for (const auto &chunk : chunks) {
            if (chunk.size() < 2) {
                continue;
            }
            if ((chunk[0] & 0x80) != 0) {
                current.clear();
            }
            current.insert(current.end(), chunk.begin() + 2, chunk.end());
            if ((chunk[0] & 0x40) != 0) {
                out.push_back(current);
                current.clear();
            }
        }
        return out;
    }

    void clear() { chunks.clear(); }

    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> published;
    size_t statusPublishes = 0;
    uint16_t negotiatedMtu = 247;
    bool isBonded = false;
    bool linkUp = true;
};

class FakeHost : public ble::IHost {
public:
    ble::DeviceInfo info() const override
    {
        ble::DeviceInfo out;
        out.bridgeProtocol = 1;
        out.nodeId = 0x0001;
        out.firmwareMajor = 0;
        out.firmwareMinor = 1;
        out.firmwarePatch = 0;
        out.wireVersion = 1;
        return out;
    }

    size_t status(uint8_t *out, size_t max) const override
    {
        if (max < 17) {
            return 0;
        }
        std::memset(out, 0, 17);
        out[0] = 0xA0; // batteryMv low byte
        out[1] = 0x0F;
        out[16] = 0x03; // timeValid | keyProvisioned
        return 17;
    }

    size_t budget(uint8_t *out, size_t max) const override
    {
        if (max < 13) {
            return 0;
        }
        std::memset(out, 0, 13);
        return 13;
    }

    uint8_t sendText(uint16_t dst, const uint8_t *text, size_t length) override
    {
        sentTexts.emplace_back(text, text + length);
        lastDst = dst;
        return sendTextError;
    }

    size_t fetchQueue(uint32_t sinceCounter, ble::QueuedEvent *out, size_t max) override
    {
        lastSince = sinceCounter;
        size_t written = 0;
        for (size_t i = 0; i < journal.size() && written < max; ++i) {
            if (journal[i].counter > sinceCounter) {
                out[written].counter = journal[i].counter;
                out[written].opcode = journal[i].opcode;
                out[written].length = static_cast<uint8_t>(journal[i].body.size());
                out[written].body = journal[i].body.data();
                ++written;
            }
        }
        return written;
    }

    void ackQueue(uint32_t upToCounter) override { acked.push_back(upToCounter); }

    uint8_t setConfig(uint32_t configVersion, const uint8_t *tlvs, size_t length,
                      uint32_t *appliedMask, uint8_t *unapplied, size_t *unappliedCount) override
    {
        configVersions.push_back(configVersion);
        configBlobs.emplace_back(tlvs, tlvs + length);
        *appliedMask = 0b0011;
        unapplied[0] = 0x09;
        *unappliedCount = 1;
        return 0;
    }

    size_t getConfig(uint8_t *out, size_t max) const override
    {
        if (max < 4) {
            return 0;
        }
        out[0] = 0x02; out[1] = 0x02; out[2] = 0xD0; out[3] = 0x07; // sniff = 2000 ms
        return 4;
    }

    uint8_t setTime(uint32_t unixSeconds) override
    {
        timesSet.push_back(unixSeconds);
        return 0;
    }

    uint8_t requestFix(uint16_t timeoutSeconds) override
    {
        fixRequests.push_back(timeoutSeconds);
        return 0;
    }

    uint8_t provisionKey(uint8_t slot, uint8_t netId, const uint8_t key[16]) override
    {
        provisioned.emplace_back(key, key + 16);
        lastSlot = slot;
        lastNetId = netId;
        return 0;
    }

    uint8_t rotateKey(uint8_t newSlot) override
    {
        rotations.push_back(newSlot);
        return 0;
    }

    uint8_t factoryReset() override
    {
        ++factoryResets;
        return 0;
    }

    uint8_t linkTest(uint16_t dst, uint8_t count, uint8_t sf) override
    {
        linkTests.push_back({dst, count, sf});
        return linkTestError;
    }

    struct JournalRow {
        uint32_t counter;
        uint8_t opcode;
        std::vector<uint8_t> body;
    };
    struct LinkTestCall {
        uint16_t dst;
        uint8_t count;
        uint8_t sf;
    };

    std::vector<std::vector<uint8_t>> sentTexts;
    std::vector<std::vector<uint8_t>> provisioned;
    std::vector<std::vector<uint8_t>> configBlobs;
    std::vector<uint32_t> configVersions;
    std::vector<uint32_t> timesSet;
    std::vector<uint32_t> acked;
    std::vector<uint16_t> fixRequests;
    std::vector<uint8_t> rotations;
    std::vector<LinkTestCall> linkTests;
    std::vector<JournalRow> journal;
    uint16_t lastDst = 0;
    uint32_t lastSince = 0;
    uint8_t lastSlot = 0;
    uint8_t lastNetId = 0;
    size_t factoryResets = 0;
    uint8_t sendTextError = 0;
    uint8_t linkTestError = 0;
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_BLE_H
