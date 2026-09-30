#include "block_store_littlefs.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_LittleFS.h>
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

#include <string.h>

namespace hal {
namespace {

/*
 * The flash the lfs callbacks talk to.
 *
 * A file-scope pointer rather than a member, because lfs_config takes plain C
 * function pointers with no context argument. There is exactly one external
 * flash chip on this board, so a second instance would be a bug rather than a
 * case to support -- and begin() refuses to run twice.
 */
Adafruit_SPIFlash *g_flash = nullptr;

/*
 * The core ships LittleFS **v1**, not v2, and the configuration struct is
 * different: `lookahead` counts blocks rather than bytes, and there is no
 * `block_cycles` or `cache_size`. Wear levelling in v1 is not a tunable.
 *
 * The ZD25WQ16B erases in 4 KiB sectors, so a LittleFS block is one sector.
 * 2 MiB / 4 KiB = 512 blocks.
 */
constexpr uint32_t kBlockSize = 4096;

/// The chip's page size. Read and program buffers are one page each.
constexpr uint32_t kPageSize = 256;

/// Blocks to look ahead when allocating. One bit each, so 128 blocks is 16
/// bytes -- and v1 wants a multiple of 32.
constexpr uint32_t kLookaheadBlocks = 128;

// The lfs callbacks. All four return an lfs error code, 0 on success.

int lfsRead(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer,
            lfs_size_t size)
{
    (void)cfg;
    const uint32_t address = block * kBlockSize + off;
    return g_flash->readBuffer(address, static_cast<uint8_t *>(buffer), size) == size
               ? 0
               : LFS_ERR_IO;
}

int lfsProg(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, const void *buffer,
            lfs_size_t size)
{
    (void)cfg;
    const uint32_t address = block * kBlockSize + off;
    return g_flash->writeBuffer(address, static_cast<const uint8_t *>(buffer), size) == size
               ? 0
               : LFS_ERR_IO;
}

int lfsErase(const struct lfs_config *cfg, lfs_block_t block)
{
    (void)cfg;
    return g_flash->eraseSector(block) ? 0 : LFS_ERR_IO;
}

int lfsSync(const struct lfs_config *cfg)
{
    (void)cfg;
    // Adafruit_SPIFlash writes through; there is no cache of its own to flush.
    // waitUntilReady() is what makes "synced" mean the chip is done rather than
    // that the driver has stopped talking.
    g_flash->waitUntilReady();
    return 0;
}

uint8_t g_readBuffer[kPageSize];
uint8_t g_progBuffer[kPageSize];
uint8_t g_fileBuffer[kPageSize];
uint32_t g_lookahead[kLookaheadBlocks / 32];

lfs_config g_config = {};
Adafruit_LittleFS g_fs(&g_config);

} // namespace

LittleFsBlockStore::LittleFsBlockStore() = default;

const char *LittleFsBlockStore::pathOf(StoreRegion region)
{
    switch (region) {
    case StoreRegion::FrameCounter:
        return "/counter";
    case StoreRegion::BudgetRing:
        return "/budget";
    case StoreRegion::MessageQueue:
        return "/queue";
    case StoreRegion::EventJournal:
        return "/journal";
    case StoreRegion::BenchScratch:
        return "/bench";
    default:
        return nullptr;
    }
}

const char *LittleFsBlockStore::tempPathOf(StoreRegion region)
{
    switch (region) {
    case StoreRegion::FrameCounter:
        return "/counter.new";
    case StoreRegion::BudgetRing:
        return "/budget.new";
    case StoreRegion::MessageQueue:
        return "/queue.new";
    case StoreRegion::EventJournal:
        return "/journal.new";
    case StoreRegion::BenchScratch:
        return "/bench.new";
    default:
        return nullptr;
    }
}

void LittleFsBlockStore::configureRegion(StoreRegion region, size_t recordBytes,
                                         size_t maxRecords)
{
    const size_t index = static_cast<size_t>(region);
    if (index >= kRegionCount || recordBytes == 0) {
        return;
    }
    layouts_[index] = RegionLayout{recordBytes, maxRecords};
}

RegionLayout LittleFsBlockStore::layoutOf(StoreRegion region) const
{
    const size_t index = static_cast<size_t>(region);
    return index < kRegionCount ? layouts_[index] : RegionLayout{0, 0};
}

bool LittleFsBlockStore::begin(Adafruit_SPIFlash &flash)
{
    if (g_flash != nullptr) {
        // One chip, one store. A second call would silently rebind the
        // callbacks under a mounted volume.
        return mounted_;
    }
    g_flash = &flash;

    const uint32_t bytes = flash.size();
    if (bytes < kBlockSize * 8) {
        g_flash = nullptr;
        return false;
    }
    volumeBytes_ = bytes;

    g_config = {};
    g_config.read = lfsRead;
    g_config.prog = lfsProg;
    g_config.erase = lfsErase;
    g_config.sync = lfsSync;
    g_config.read_size = kPageSize;
    g_config.prog_size = kPageSize;
    g_config.block_size = kBlockSize;
    g_config.block_count = bytes / kBlockSize;
    g_config.lookahead = kLookaheadBlocks;
    g_config.read_buffer = g_readBuffer;
    g_config.prog_buffer = g_progBuffer;
    g_config.lookahead_buffer = g_lookahead;
    /*
     * A static file buffer, which in v1 means only one file may be open at a
     * time. The Adafruit wrapper already imposes that ("currently only one file
     * can be open at a time") and every method here opens, works and closes --
     * so the restriction costs nothing, and what it buys is that LittleFS never
     * calls malloc. CLAUDE.md 3 bans allocation in link/ and app/; there is no
     * reason for the layer underneath them to be the one that allocates.
     */
    g_config.file_buffer = g_fileBuffer;

    if (!g_fs.begin(&g_config)) {
        // First boot, or a volume this build cannot read. Formatting is right
        // for the first case and unavoidable for the second: an unmountable
        // volume holds nothing recoverable anyway.
        if (!g_fs.format() || !g_fs.begin(&g_config)) {
            g_flash = nullptr;
            return false;
        }
    }
    mounted_ = true;
    return true;
}

size_t LittleFsBlockStore::recordSize(StoreRegion region) const
{
    return layoutOf(region).recordBytes;
}

size_t LittleFsBlockStore::capacity(StoreRegion region) const
{
    return layoutOf(region).maxRecords;
}

size_t LittleFsBlockStore::count(StoreRegion region) const
{
    if (!mounted_) {
        return 0;
    }
    const char *path = pathOf(region);
    if (path == nullptr || !g_fs.exists(path)) {
        return 0;
    }
    Adafruit_LittleFS_Namespace::File file = g_fs.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ);
    if (!file) {
        return 0;
    }
    const size_t bytes = file.size();
    file.close();
    const size_t recordBytes = layoutOf(region).recordBytes;
    return recordBytes == 0 ? 0 : bytes / recordBytes;
}

StoreResult LittleFsBlockStore::append(StoreRegion region, const uint8_t *data, size_t len)
{
    if (!mounted_) {
        return StoreResult::IoError;
    }
    const RegionLayout layout = layoutOf(region);
    if (layout.recordBytes == 0 || len != layout.recordBytes) {
        return StoreResult::BadLength;
    }
    if (count(region) >= layout.maxRecords) {
        // Full, never overwriting. The ring semantics belong to the caller,
        // which is the only thing that knows which record is oldest.
        return StoreResult::Full;
    }

    Adafruit_LittleFS_Namespace::File file =
        g_fs.open(pathOf(region), Adafruit_LittleFS_Namespace::FILE_O_WRITE);
    if (!file) {
        return StoreResult::IoError;
    }
    file.seek(file.size());
    const size_t written = file.write(data, len);
    file.close();
    return written == len ? StoreResult::Ok : StoreResult::IoError;
}

StoreResult LittleFsBlockStore::read(StoreRegion region, size_t index, uint8_t *out,
                                     size_t len) const
{
    if (!mounted_) {
        return StoreResult::IoError;
    }
    const RegionLayout layout = layoutOf(region);
    if (layout.recordBytes == 0 || len != layout.recordBytes) {
        return StoreResult::BadLength;
    }
    const char *path = pathOf(region);
    if (path == nullptr || !g_fs.exists(path)) {
        return StoreResult::NotFound;
    }

    Adafruit_LittleFS_Namespace::File file = g_fs.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ);
    if (!file) {
        return StoreResult::IoError;
    }
    const size_t offset = index * layout.recordBytes;
    if (offset + len > file.size()) {
        file.close();
        return StoreResult::NotFound;
    }
    file.seek(offset);
    const size_t got = file.read(out, len);
    file.close();
    return got == len ? StoreResult::Ok : StoreResult::IoError;
}

StoreResult LittleFsBlockStore::write(StoreRegion region, size_t index, const uint8_t *data,
                                      size_t len)
{
    if (!mounted_) {
        return StoreResult::IoError;
    }
    const RegionLayout layout = layoutOf(region);
    if (layout.recordBytes == 0 || len != layout.recordBytes) {
        return StoreResult::BadLength;
    }
    if (index >= count(region)) {
        return StoreResult::NotFound;
    }

    Adafruit_LittleFS_Namespace::File file =
        g_fs.open(pathOf(region), Adafruit_LittleFS_Namespace::FILE_O_WRITE);
    if (!file) {
        return StoreResult::IoError;
    }
    file.seek(index * layout.recordBytes);
    const size_t written = file.write(data, len);
    file.close();
    return written == len ? StoreResult::Ok : StoreResult::IoError;
}

StoreResult LittleFsBlockStore::erase(StoreRegion region)
{
    if (!mounted_) {
        return StoreResult::IoError;
    }
    const char *path = pathOf(region);
    if (path == nullptr) {
        return StoreResult::IoError;
    }
    if (g_fs.exists(path) && !g_fs.remove(path)) {
        return StoreResult::IoError;
    }
    return StoreResult::Ok;
}

StoreResult LittleFsBlockStore::replaceAll(StoreRegion region, const uint8_t *data, size_t len)
{
    /*
     * Temporary file, sync, rename. The interface asks for this by name, and the
     * reason is in its own comment: erase() followed by append() has a window in
     * which the region holds nothing, and for the frame counter that window
     * costs the counter -- "a counter that came back as zero would reuse CCM
     * nonces against a peer that still remembered the old ones".
     *
     * LittleFS's rename is atomic. That is the whole mechanism.
     */
    if (!mounted_) {
        return StoreResult::IoError;
    }
    const RegionLayout layout = layoutOf(region);
    if (layout.recordBytes == 0 || len != layout.recordBytes) {
        return StoreResult::BadLength;
    }
    const char *path = pathOf(region);
    const char *temp = tempPathOf(region);
    if (path == nullptr || temp == nullptr) {
        return StoreResult::IoError;
    }

    if (g_fs.exists(temp)) {
        // Left behind by a power cut inside a previous replaceAll. The old file
        // is still intact, so the partial one is simply discarded.
        g_fs.remove(temp);
    }

    {
        Adafruit_LittleFS_Namespace::File file =
            g_fs.open(temp, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
        if (!file) {
            return StoreResult::IoError;
        }
        const size_t written = file.write(data, len);
        // flush() before close() so the rename below cannot beat the data onto
        // the chip.
        file.flush();
        file.close();
        if (written != len) {
            g_fs.remove(temp);
            return StoreResult::IoError;
        }
    }

    if (g_fs.exists(path) && !g_fs.remove(path)) {
        g_fs.remove(temp);
        return StoreResult::IoError;
    }
    if (!g_fs.rename(temp, path)) {
        return StoreResult::IoError;
    }
    return StoreResult::Ok;
}

StoreResult LittleFsBlockStore::sync(StoreRegion region)
{
    (void)region;
    if (!mounted_) {
        return StoreResult::IoError;
    }
    // Every write above closes its file, which flushes it. This waits for the
    // chip itself, so a reservation handed out after sync() really is on flash
    // (CLAUDE.md 2.1: "a reservation that is only in RAM is not a reservation").
    if (g_flash != nullptr) {
        g_flash->waitUntilReady();
    }
    return StoreResult::Ok;
}

} // namespace hal
