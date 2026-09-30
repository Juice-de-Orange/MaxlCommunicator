/*
 * IBlockStore on LittleFS, on the external ZD25WQ16B.
 *
 * CLAUDE.md 3.0 puts the frame counter high-water mark, the duty cycle budget
 * ring, the message queue and the event journal here rather than in internal
 * flash: "Everything else benefits from LittleFS's wear levelling, which matters
 * for the counter and budget records that are written continuously." The network
 * key stays internal, because the external chip "is trivially readable by anyone
 * with a clip and a logic analyser".
 *
 * Confirmed on node A, 2026-08-31: JEDEC 0xBA6015, 2,097,152 bytes, and
 * Adafruit_SPIFlash::begin() must be handed the device descriptors explicitly --
 * with no arguments it returns false on this board, because ZD25WQ16B is in the
 * library's flash_devices.h but not in the list begin() consults when it is
 * given nothing.
 *
 * One region is one file of fixed-size records. That is enough for all four
 * users and it keeps each one's failure contained: a corrupted budget ring
 * cannot take the frame counter with it, and a counter that came back wrong
 * would reuse CCM nonces (CLAUDE.md 2.1).
 *
 * NOT covered by the host tests -- this is the half that needs the chip.
 * Everything above it is tested against fakes/fake_block_store.h.
 */

#ifndef MAXL_HAL_BLOCK_STORE_LITTLEFS_H
#define MAXL_HAL_BLOCK_STORE_LITTLEFS_H

#include "hal/i_block_store.h"

#include <stddef.h>
#include <stdint.h>

class Adafruit_SPIFlash;

namespace hal {

/// Per-region record geometry. Set once at construction; the modules that own
/// each region declare their own sizes and must agree with these.
struct RegionLayout {
    size_t recordBytes;
    size_t maxRecords;
};

class LittleFsBlockStore : public IBlockStore {
public:
    LittleFsBlockStore();

    /**
     * Mount, formatting once if the volume is not there yet.
     *
     * Returns false when the chip does not answer or the volume cannot be
     * mounted after a format. The caller must treat that as "refuse to
     * transmit": CLAUDE.md 2.1 says a device that cannot persist its counter
     * must not send, and the budget from 1.2 is in the same position.
     */
    bool begin(Adafruit_SPIFlash &flash);

    /**
     * Declare a region's record geometry.
     *
     * Called by whoever wires the modules together, with the sizes each owning
     * module declares -- not read from those modules here. hal/ must not include
     * link/ or app/ (decision D3), and the first draft of this file did exactly
     * that to reach app::kBlobBytes. check_layering.py caught it, which is what
     * it is for.
     *
     * An unconfigured region answers BadLength to every call rather than
     * guessing a size, because a guessed record size silently misreads every
     * record in it.
     */
    void configureRegion(StoreRegion region, size_t recordBytes, size_t maxRecords);

    bool mounted() const { return mounted_; }

    /// Total bytes the volume reports. For the STATUS screen and for sanity.
    uint32_t volumeBytes() const { return volumeBytes_; }

    size_t recordSize(StoreRegion region) const override;
    size_t capacity(StoreRegion region) const override;
    size_t count(StoreRegion region) const override;
    StoreResult append(StoreRegion region, const uint8_t *data, size_t len) override;
    StoreResult read(StoreRegion region, size_t index, uint8_t *out, size_t len) const override;
    StoreResult write(StoreRegion region, size_t index, const uint8_t *data, size_t len) override;
    StoreResult erase(StoreRegion region) override;
    StoreResult replaceAll(StoreRegion region, const uint8_t *data, size_t len) override;
    StoreResult sync(StoreRegion region) override;

private:
    static constexpr size_t kRegionCount = static_cast<size_t>(StoreRegion::RegionCount);

    bool mounted_ = false;
    uint32_t volumeBytes_ = 0;
    RegionLayout layouts_[kRegionCount] = {};

    static const char *pathOf(StoreRegion region);
    static const char *tempPathOf(StoreRegion region);
    RegionLayout layoutOf(StoreRegion region) const;
};

} // namespace hal

#endif // MAXL_HAL_BLOCK_STORE_LITTLEFS_H
