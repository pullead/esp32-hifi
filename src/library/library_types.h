// 本地音乐库 2.0 —— 数据模型
//
// 设计依据见 docs/LOCAL_LIBRARY_V2_AUDIT.md §4：**不用 SQLite**，用「定长索引
// tracks.idx + 追加日志 events.log」。理由是这块板内部 SRAM 只剩十几 KB，
// SQLite 的 page cache 无处安放，而定长记录根本不需要查询引擎——算
// delete_score 就是在 PSRAM 里对几千条排序，微秒级。
//
// ⚠️ 文件里存的是**原始字节**。改动 TrackRecord 的任何字段都会让旧索引全部
// 错位。防护做法：文件头里记着 `recordSize`，加载时与 `sizeof(TrackRecord)`
// 比对，不一致就整个丢弃重建（重建的代价只是重扫一次 SD，可以接受）。
// 所以**加字段优先吃 reserved**，实在不够再让它自然失配重建。

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// 标志位
// ---------------------------------------------------------------------------
enum TrackFlag : uint8_t {
    kTrackFlagNone = 0,
    // 用户明确喜欢。**永不自动删除**（方案 §14）
    kTrackFlagFavorite = 1 << 0,
    // 用户希望保留，优先级低于 Favorite。默认也不自动删除
    kTrackFlagKeep = 1 << 1,
    // 每日自动发现下载的曲目，是主要淘汰池
    kTrackFlagDiscovery = 1 << 2,
    // 索引里有、SD 上却找不到文件。
    // 典型成因：用户在 U 盘模式下用电脑删了歌（见审计 §10）。
    // 保留记录而不是直接抹掉，是为了留住 favorite / 播放历史——
    // 文件要是再回来（比如拷回去），这些统计不至于清零。
    kTrackFlagMissing = 1 << 3,
};

enum TrackProvider : uint8_t {
    kProviderLocal = 0,   // 用户自己拷进 SD 的歌
    kProviderJamendo = 1,
};

// ---------------------------------------------------------------------------
// 曲目记录
// ---------------------------------------------------------------------------
struct TrackRecord {
    // —— 以下字段与旧的 LocalTrackItem **同名同序**。
    //    main.cpp 里有 20 多处直接写 s_localTracks[i].path / .title / .hasArt，
    //    保持同名是为了让那些代码一行都不用改。——
    char path[160]{};
    char title[64]{};
    char artist[48]{};
    char album[48]{};
    bool hasArt = false;

    // —— 本地音乐库 2.0 新增 ——
    uint32_t localId = 0;        // FNV-1a(path)，跨重启稳定；改名即视为新曲
    uint32_t fileSize = 0;
    uint32_t durationSec = 0;    // 暂未填充（需要解码器配合），预留
    uint32_t importedAt = 0;     // epoch，首次进入索引的时间
    uint32_t lastPlayedAt = 0;   // epoch
    uint16_t playCount = 0;
    uint16_t completedCount = 0; // 完整播放次数（不是仅仅开始播）
    uint16_t skipCount = 0;
    uint8_t  flags = kTrackFlagNone;
    uint8_t  provider = kProviderLocal;
    char     providerTrackId[24]{};
    uint8_t  reserved[8]{};      // 加字段优先吃这里，避免升版本重建

    bool favorite() const { return flags & kTrackFlagFavorite; }
    bool keep() const { return flags & kTrackFlagKeep; }
    bool missing() const { return flags & kTrackFlagMissing; }
};

// ---------------------------------------------------------------------------
// 事件日志
//
// 为什么单独有一份追加日志、而不是每次播放都重写 tracks.idx：
// 800KB 的索引每首歌播完都全量重写，既慢又是对 SD 的无谓磨损，而且**重写过程
// 中断电会毁掉整个索引**。追加写最多丢最后一条记录。
// 开机时把 events.log 回放到内存里的记录上，再整体落盘一次并清空日志（compact）。
// ---------------------------------------------------------------------------
enum LibraryEventType : uint8_t {
    kEventPlayStarted = 1,
    kEventPlayCompleted = 2,   // 播到结尾
    kEventSkipped = 3,         // 未播完就切走
    kEventFavoriteOn = 4,
    kEventFavoriteOff = 5,
    kEventKeepOn = 6,
    kEventKeepOff = 7,
};

struct LibraryEvent {
    uint32_t localId = 0;
    uint32_t timestamp = 0;    // epoch；RTC 未同步时为 0，回放时只更新计数不更新时间
    uint8_t  type = 0;
    uint8_t  reserved[3]{};
};

// ---------------------------------------------------------------------------
// 文件头
// ---------------------------------------------------------------------------
constexpr uint32_t kLibraryMagic = 0x4257524Cu;  // 'LRWB'
// v2（2026-09-05）：libraryHashPath 改为不区分大小写，所有 localId 都变了。
// 必须升版本 —— 否则旧索引里那些按旧哈希算的 id 会和新算法算出的对不上，
// 表现为"全部曲目都 missing 且全部重新导入"，而且**现有的重复记录会留在里面**。
// 升版本让加载器整个重建，顺带把重复清掉。代价是播放统计清零（当时 fav=0、
// played=13，可接受）。
constexpr uint16_t kLibraryFormatVersion = 2;

struct LibraryFileHeader {
    uint32_t magic = kLibraryMagic;
    uint16_t version = kLibraryFormatVersion;
    uint16_t recordSize = 0;   // 加载时必须等于 sizeof(TrackRecord)，否则整个重建
    uint32_t recordCount = 0;
    uint32_t reserved[5]{};
};

// FNV-1a 32 位。用它从路径算稳定 id：同一个文件在多次重启之间 id 不变，
// 而重命名会得到新 id（第一版接受这个行为——改名等同于换了一首歌）。
//
// ⚠️ **必须不区分大小写。** 2026-09-05 实测踩到：
//   daily_sync 按 "/music/tracks/..." 写入并入库
//   扫描器的 entry.path() 返回 "/Music/tracks/..."（磁盘上真实是大写 M）
// FAT 查找时不区分大小写，所以下载和播放都正常；但两个字符串的哈希不同，
// 于是**同一个文件在索引里存了两份**：一份被判 missing，另一份被当新曲追加。
// 后者 provider=local 且没有 Discovery 标记 —— Cleaner 会当成用户自己的歌
// 永不淘汰，整个曲库轮换的前提就废了。
//
// 修的是哈希而不是"把 /music 改成 /Music"：后者只是把硬编码换个方向，
// 换张卡或换个写法就复发。
inline uint32_t libraryHashPath(const char* path) {
    uint32_t h = 2166136261u;
    if (!path) return h;
    for (const char* p = path; *p; ++p) {
        uint8_t c = static_cast<uint8_t>(*p);
        if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c - 'A' + 'a');
        h ^= c;
        h *= 16777619u;
    }
    return h;
}
