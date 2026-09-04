// 本地音乐库 2.0 —— 每日发现同步（方案 Phase 5）
//
// 职责：一天跑一轮，按语言配额挑候选、去重、下载、并入索引。
// **不负责决定什么时候跑**（由调用方按 dailySyncDue() 判断），
// **不负责持久化**（索引存盘和状态存 NVS 都由调用方做）。
//
// ⚠️ 这个模块的设计被前面几轮实测直接约束，不是照方案空想的：
//
//   1. **单曲下载失败是常态。** Jamendo 的 audiodownload_allowed=true 只是
//      授权标志，不代表存储端给得出文件 —— id=2034080 连续四轮稳定 500，
//      同批别的曲目正常。所以失败要跳过并记账，绝不能终止整轮。
//
//   2. **provider 会间歇性返回空数组。** 同一个 URL 有时 5 条有时 0 条，
//      已观测到两种形态。一次拉空 ≠ 今天没歌，要重试、要换 offset。
//
//   3. **下载很慢**（实测约 23KB/s，一首 3.4MB 要 150~210 秒）。
//      10 首就是 25~35 分钟。所以这一轮必须是长时间后台任务，
//      而且要能被随时中断而不留下半截状态。
//
//   4. **边播边下是安全的**（本地和电台都实测过零饥饿，节流已生效），
//      所以**不做**"只在没播放时才下载"这种保守设计。
//
//   5. **不删除任何文件。** 空间不够就停下并记账。真实删除属于
//      library_cleaner 的后续工作，需要单独的验证和用户确认，
//      不能顺手塞进每日同步里。

#pragma once

#include <stdint.h>
#include <stddef.h>   // size_t（dailySyncBuildPath 的 outSize）

#include "../library/library_types.h"
#include "music_provider.h"

struct DailySyncConfig {
    uint8_t  tracksPerDay = 10;      // 一轮目标下载数
    // 尝试预算。**明显大于 tracksPerDay** —— 失败和重复是常态（见上面 1、2），
    // 预算太紧会让一轮同步在几首坏曲目上就耗光。
    uint8_t  maxAttempts = 30;
    uint8_t  candidatesPerFetch = 20;  // 受 kMaxTracksPerRequest 限制
    uint8_t  fetchRetries = 3;         // 拉到空数组时重试几次
    uint16_t fetchRetryDelayMs = 5000;
};

// 跨轮次要记住的东西。由调用方存 NVS 并在下一轮传回来。
//
// offset 逐轮推进是为了**不要每天都下同样几首热门歌**。
// 不存的话每天 offset 都从 0 开始，第二天拿到的候选和第一天几乎一样，
// 全部命中"已拥有"然后空跑一轮。
struct DailySyncState {
    uint32_t lastRunDay = 0;   // nowEpoch / 86400，判断今天跑没跑过
    uint16_t offsetZh = 0;
    uint16_t offsetJa = 0;
    uint16_t offsetEn = 0;
    uint16_t offsetAny = 0;
};

struct DailySyncStats {
    uint8_t  downloaded = 0;
    uint8_t  attempts = 0;          // 实际发起过的下载次数
    uint8_t  skippedOwned = 0;      // 候选已经在库里
    uint8_t  failedDownload = 0;
    uint8_t  emptyFetches = 0;      // provider 返回空数组的次数（含重试）
    bool     stoppedNoSpace = false;
    bool     indexDirty = false;    // 有新曲进索引，调用方需要存盘
    uint32_t bytes = 0;
    uint32_t elapsedMs = 0;
    char     lastError[80]{};
};

// 今天还没跑过就返回 true。nowEpoch 为 0（RTC 未同步）时返回 false ——
// **不知道今天是几号就不要跑**，否则每次重启都会当成新的一天再下一轮。
bool dailySyncDue(const DailySyncState& state, uint32_t nowEpoch);

// 跑一轮。**阻塞几十分钟**，必须放在自己的任务里调用。
//
// records/count 是主程序那份索引，新曲会就地追加（需要 capacity 有余量）。
// playingLocalId 传当前正在播放的曲目 id（没有就传 0），用于空间评估时保护它。
//
// state 会被就地更新（offset 推进、lastRunDay 置位），调用方负责存回 NVS。
void dailySyncRun(MusicProvider& provider,
                  TrackRecord* records, uint16_t* count, uint16_t capacity,
                  uint32_t nowEpoch, uint32_t playingLocalId,
                  const DailySyncConfig& cfg,
                  DailySyncState* state, DailySyncStats* out);

// 库里是否已经有这条 provider 曲目（按 provider + providerTrackId 比对）。
// 单独暴露出来是为了能不联网就测。
bool dailySyncOwnsTrack(const TrackRecord* records, uint16_t count,
                        uint8_t provider, const char* providerTrackId);

// 下载后的落地路径，形如 /music/tracks/jamendo_1593988.mp3。
// 单独暴露同样是为了可测：路径规则错了会导致去重失效、重复下载。
void dailySyncBuildPath(char* out, size_t outSize,
                        const char* providerName, const char* trackId);
