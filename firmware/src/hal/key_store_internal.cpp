#include "key_store_internal.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <InternalFileSystem.h>
#pragma GCC diagnostic pop

#include <string.h>

namespace hal {
namespace {

using namespace Adafruit_LittleFS_Namespace;

/*
 * One file per slot, and a magic byte in front of each.
 *
 * The magic is not paranoia about corruption -- it is what makes "no key" and "a
 * key of sixteen zero bytes" different states. An all-zero key read back as
 * valid would encrypt every frame with a key an attacker can guess in one try,
 * and nothing about the frame would look wrong.
 */
constexpr uint8_t kMagic = 0x4B; // 'K'
constexpr size_t kRecordBytes = 1 + 1 + kKeyBytes; // magic, netId, key

} // namespace

const char *InternalKeyStore::pathOf(uint8_t slot)
{
    switch (slot) {
    case 0:
        return "/netkey0";
    case 1:
        return "/netkey1";
    default:
        return nullptr;
    }
}

bool InternalKeyStore::begin()
{
    mounted_ = InternalFS.begin();
    return mounted_;
}

KeyResult InternalKeyStore::store(uint8_t slot, uint8_t netId, const uint8_t key[kKeyBytes])
{
    const char *path = pathOf(slot);
    if (path == nullptr) {
        return KeyResult::BadSlot;
    }
    if (!mounted_) {
        return KeyResult::IoError;
    }

    uint8_t record[kRecordBytes];
    record[0] = kMagic;
    record[1] = netId;
    memcpy(record + 2, key, kKeyBytes);

    // Remove before writing: LittleFS opens for append by default, and a second
    // provisioning would otherwise leave two records in one file with the stale
    // one first.
    InternalFS.remove(path);

    File file = InternalFS.open(path, FILE_O_WRITE);
    if (!file) {
        memset(record, 0, sizeof(record));
        return KeyResult::IoError;
    }
    const size_t written = file.write(record, sizeof(record));
    file.close();

    // The key was on this stack. Leaving it there is a copy nobody accounted
    // for, in RAM a fault handler or a later stack frame could print.
    memset(record, 0, sizeof(record));

    return written == sizeof(record) ? KeyResult::Ok : KeyResult::IoError;
}

KeyResult InternalKeyStore::load(uint8_t slot, uint8_t *netIdOut, uint8_t keyOut[kKeyBytes]) const
{
    const char *path = pathOf(slot);
    if (path == nullptr) {
        return KeyResult::BadSlot;
    }
    if (!mounted_ || !InternalFS.exists(path)) {
        return KeyResult::NotFound;
    }

    File file = InternalFS.open(path, FILE_O_READ);
    if (!file) {
        return KeyResult::IoError;
    }
    uint8_t record[kRecordBytes];
    const size_t read = file.read(record, sizeof(record));
    file.close();

    if (read != sizeof(record) || record[0] != kMagic) {
        memset(record, 0, sizeof(record));
        return KeyResult::NotFound;
    }
    if (netIdOut != nullptr) {
        *netIdOut = record[1];
    }
    memcpy(keyOut, record + 2, kKeyBytes);
    memset(record, 0, sizeof(record));
    return KeyResult::Ok;
}

bool InternalKeyStore::hasKey(uint8_t slot) const
{
    const char *path = pathOf(slot);
    return mounted_ && path != nullptr && InternalFS.exists(path);
}

bool InternalKeyStore::hasAnyKey() const
{
    return hasKey(0) || hasKey(1);
}

KeyResult InternalKeyStore::eraseAll()
{
    if (!mounted_) {
        return KeyResult::IoError;
    }
    bool ok = true;
    for (uint8_t slot = 0; slot < kKeySlots; ++slot) {
        const char *path = pathOf(slot);
        if (path != nullptr && InternalFS.exists(path)) {
            ok = InternalFS.remove(path) && ok;
        }
    }
    return ok ? KeyResult::Ok : KeyResult::IoError;
}

} // namespace hal
