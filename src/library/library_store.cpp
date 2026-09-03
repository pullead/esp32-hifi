#include "library_store.h"

#include <SD_MMC.h>
#include <stdio.h>
#include <string.h>

const char* const kLibraryIndexPath = "/music/library/tracks.idx";
const char* const kLibraryEventsPath = "/music/library/events.log";

namespace {

const char* const kIndexTempPath = "/music/library/tracks.idx.part";

// 一次读写多少条。太大占内存，太小 SD 事务开销高。
// 32 条 × ~400B ≈ 12KB，走栈上不行，所以按条读——SD_MMC 有自己的缓存，
// 逐条 read() 的开销可以接受（实测再调）。
void ensureLibraryDir() {
    if (!SD_MMC.exists("/music")) SD_MMC.mkdir("/music");
    if (!SD_MMC.exists("/music/library")) SD_MMC.mkdir("/music/library");
}

} // namespace

// ---------------------------------------------------------------------------

int32_t libraryStoreFind(const TrackRecord* records, uint16_t count, uint32_t localId) {
    if (!records || !localId) return -1;
    for (uint16_t i = 0; i < count; ++i) {
        if (records[i].localId == localId) return static_cast<int32_t>(i);
    }
    return -1;
}

void libraryStoreApplyEvent(TrackRecord* records, uint16_t count, const LibraryEvent& event) {
    const int32_t idx = libraryStoreFind(records, count, event.localId);
    if (idx < 0) return;
    TrackRecord& r = records[idx];

    switch (event.type) {
        case kEventPlayStarted:
            if (r.playCount < 0xFFFF) ++r.playCount;
            // timestamp 为 0 表示当时 RTC 还没同步（见审计 §2）。
            // 这种情况下只累加次数，不写一个错误的时间——lastPlayedAt 一旦被写成
            // 1970 年，淘汰算法会把这首歌判成"最久没听"，正好判反。
            if (event.timestamp) r.lastPlayedAt = event.timestamp;
            break;
        case kEventPlayCompleted:
            if (r.completedCount < 0xFFFF) ++r.completedCount;
            if (event.timestamp) r.lastPlayedAt = event.timestamp;
            break;
        case kEventSkipped:
            if (r.skipCount < 0xFFFF) ++r.skipCount;
            break;
        case kEventFavoriteOn:  r.flags |= kTrackFlagFavorite; break;
        case kEventFavoriteOff: r.flags &= ~kTrackFlagFavorite; break;
        case kEventKeepOn:      r.flags |= kTrackFlagKeep; break;
        case kEventKeepOff:     r.flags &= ~kTrackFlagKeep; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------

uint16_t libraryStoreLoad(TrackRecord* records, uint16_t capacity) {
    if (!records || !capacity) return 0;

    File f = SD_MMC.open(kLibraryIndexPath, "r");
    if (!f) return 0;

    LibraryFileHeader header{};
    if (f.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        f.close();
        return 0;
    }

    // 任何一项对不上都整个丢弃重建。重建的代价只是重扫一次 SD，
    // 而带着错位的索引跑下去会产生难查的怪问题。
    if (header.magic != kLibraryMagic || header.version != kLibraryFormatVersion ||
        header.recordSize != sizeof(TrackRecord)) {
        printf("[LIB] index header mismatch (magic=%08lx ver=%u recSize=%u, expect %u) -- rebuilding\n",
               static_cast<unsigned long>(header.magic), header.version, header.recordSize,
               static_cast<unsigned>(sizeof(TrackRecord)));
        f.close();
        return 0;
    }

    uint16_t n = header.recordCount > capacity ? capacity : static_cast<uint16_t>(header.recordCount);
    if (header.recordCount > capacity) {
        printf("[LIB] index has %lu records but capacity is %u -- truncating\n",
               static_cast<unsigned long>(header.recordCount), capacity);
    }
    uint16_t loaded = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (f.read(reinterpret_cast<uint8_t*>(&records[i]), sizeof(TrackRecord)) != sizeof(TrackRecord)) break;
        ++loaded;
    }
    f.close();

    // 回放事件日志。
    uint32_t replayed = 0;
    File ev = SD_MMC.open(kLibraryEventsPath, "r");
    if (ev) {
        LibraryEvent e{};
        while (ev.read(reinterpret_cast<uint8_t*>(&e), sizeof(e)) == sizeof(e)) {
            libraryStoreApplyEvent(records, loaded, e);
            ++replayed;
        }
        ev.close();
    }

    printf("[LIB] loaded %u records, replayed %lu events\n", loaded, static_cast<unsigned long>(replayed));
    return loaded;
}

bool libraryStoreSave(const TrackRecord* records, uint16_t count) {
    if (!records) return false;
    ensureLibraryDir();

    // 先写 .part 再 rename：写到一半断电只会毁掉临时文件，旧索引仍然完好。
    // 和下载用 .part 的理由完全一样（方案 §18）。
    SD_MMC.remove(kIndexTempPath);
    File f = SD_MMC.open(kIndexTempPath, "w");
    if (!f) {
        printf("[LIB] cannot open %s for write\n", kIndexTempPath);
        return false;
    }

    LibraryFileHeader header{};
    header.magic = kLibraryMagic;
    header.version = kLibraryFormatVersion;
    header.recordSize = static_cast<uint16_t>(sizeof(TrackRecord));
    header.recordCount = count;
    if (f.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        f.close();
        SD_MMC.remove(kIndexTempPath);
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        if (f.write(reinterpret_cast<const uint8_t*>(&records[i]), sizeof(TrackRecord)) != sizeof(TrackRecord)) {
            f.close();
            SD_MMC.remove(kIndexTempPath);
            printf("[LIB] write failed at record %u\n", i);
            return false;
        }
    }
    f.close();

    SD_MMC.remove(kLibraryIndexPath);
    if (!SD_MMC.rename(kIndexTempPath, kLibraryIndexPath)) {
        printf("[LIB] rename %s -> %s failed\n", kIndexTempPath, kLibraryIndexPath);
        return false;
    }

    // 索引已经把日志里的内容吸收进去了，日志可以清空（compact）。
    SD_MMC.remove(kLibraryEventsPath);
    printf("[LIB] saved %u records\n", count);
    return true;
}

bool libraryStoreAppendEvent(uint32_t localId, uint8_t type, uint32_t timestamp) {
    if (!localId) return false;
    ensureLibraryDir();
    File f = SD_MMC.open(kLibraryEventsPath, "a");
    if (!f) return false;
    LibraryEvent e{};
    e.localId = localId;
    e.timestamp = timestamp;
    e.type = type;
    const bool ok = f.write(reinterpret_cast<const uint8_t*>(&e), sizeof(e)) == sizeof(e);
    f.close();
    return ok;
}

// ---------------------------------------------------------------------------
// 扫描期对账
// ---------------------------------------------------------------------------

namespace {
inline void seenSet(uint8_t* bits, uint16_t idx) { bits[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7u)); }
inline bool seenGet(const uint8_t* bits, uint16_t idx) { return (bits[idx >> 3] >> (idx & 7u)) & 1u; }
} // namespace

int32_t libraryStoreUpsert(TrackRecord* records, uint16_t* count, uint16_t capacity,
                           const TrackRecord& scanned, uint8_t* seenBits, uint32_t nowEpoch) {
    if (!records || !count || !seenBits) return -1;

    const uint32_t id = libraryHashPath(scanned.path);
    const int32_t existing = libraryStoreFind(records, *count, id);

    if (existing >= 0) {
        TrackRecord& r = records[existing];
        // 元数据以本次扫描为准（ID3 可能被用户改过），但统计和 flags 必须留住。
        memcpy(r.path, scanned.path, sizeof(r.path));
        memcpy(r.title, scanned.title, sizeof(r.title));
        memcpy(r.artist, scanned.artist, sizeof(r.artist));
        memcpy(r.album, scanned.album, sizeof(r.album));
        r.hasArt = scanned.hasArt;
        if (scanned.fileSize) r.fileSize = scanned.fileSize;
        // 文件这次扫到了，说明它回来了
        r.flags &= static_cast<uint8_t>(~kTrackFlagMissing);
        if (!r.importedAt) r.importedAt = nowEpoch;
        seenSet(seenBits, static_cast<uint16_t>(existing));
        return existing;
    }

    if (*count >= capacity) return -1;
    const uint16_t idx = *count;
    records[idx] = scanned;
    records[idx].localId = id;
    records[idx].importedAt = nowEpoch;
    ++(*count);
    seenSet(seenBits, idx);
    return static_cast<int32_t>(idx);
}

uint16_t libraryStoreFinishScan(TrackRecord* records, uint16_t count, const uint8_t* seenBits) {
    if (!records || !seenBits) return 0;
    uint16_t marked = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (seenGet(seenBits, i)) continue;
        if (records[i].flags & kTrackFlagMissing) continue; // 早就标过了
        records[i].flags |= kTrackFlagMissing;
        ++marked;
    }
    return marked;
}
