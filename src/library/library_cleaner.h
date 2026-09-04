// 本地音乐库 2.0 —— 空间管理与淘汰
//
// 对应方案 §15/§30 与 docs/LOCAL_LIBRARY_V2_AUDIT.md。
//
// ⚠️ 三条不可协商的安全规则（写在最前面，因为误删是不可逆的）：
//
//   1. **只淘汰带 kTrackFlagDiscovery 的曲目** —— 也就是本系统自己下载的。
//      用户自己拷进 SD 的歌**永远不删**。方案只说 Discovery 是"主要淘汰池"，
//      这里把它收紧成硬规则：删掉用户手动放进去的音乐是不可逆的伤害，
//      不值得为了几百 MB 冒这个险。
//   2. **Favorite / Keep 永不自动删除**，无论 score 多高。
//   3. **正在播放的那首不删。**
//
// 第一版只提供 dry-run（算出"会删哪些、能腾多少"并打印），不真的删除。
// 真实删除等算法在真机上看过几轮、确认没有误伤之后再开。

#pragma once

#include "library_types.h"

#include <stdint.h>

// 空间策略。方案 §3 建议 reserve = max(4GB, 总容量 8%)。
constexpr uint64_t kCleanerReserveMinBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint8_t  kCleanerReservePercent = 8;

struct CleanerPlan {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t reserveBytes = 0;    // 要求保留的空闲下限
    uint64_t incomingBytes = 0;   // 本次准备写入多少（下载批次的预估）
    uint64_t shortfallBytes = 0;  // 还差多少才能满足 reserve；0 = 不需要清理
    uint64_t reclaimableBytes = 0;// 所有合格候选加起来能腾出多少
    uint16_t candidateCount = 0;  // 合格候选总数（不是"要删几个"）
    uint16_t protectedCount = 0;  // 因 favorite/keep/正在播放 被保护的条数
    uint16_t nonDiscoveryCount = 0; // 用户自己的歌，压根不参与
    bool     needsCleaning = false;
    bool     sdReadable = false;  // SD 容量读取是否成功
};

// 读 SD 容量 + 统计淘汰候选。**不删除任何东西。**
//
// incomingBytes：本次准备下载的总字节数（含封面与临时文件余量）。
// 只做容量评估时传 0。
//
// ⚠️ records/count 必须传：CleanerPlan 里的 candidateCount / reclaimableBytes /
// protectedCount / nonDiscoveryCount 都要遍历记录才能得出。
// 初版这个函数只算容量、不看记录，结果调用方打印那几个字段时打出的是从没被
// 计算过的 0 —— 那比不打印更糟，会被当成"没有候选"这个结论。
void libraryCleanerAssess(const TrackRecord* records, uint16_t count,
                          uint64_t incomingBytes, uint32_t playingLocalId,
                          CleanerPlan* out);

// 淘汰分数：**越高越该删**。
//
// 权重是拍脑袋定的初值，必须靠 dry-run 在真实曲库上看几轮再调
// —— 这正是第一版只做 dry-run 的原因。
//
// nowEpoch 为 0（RTC 未同步）时，与时间相关的项全部跳过，只按播放/跳过次数算。
// 这比用一个错误的"现在"要安全：时间算错会让整个排序失去意义。
int32_t libraryCleanerScore(const TrackRecord& r, uint32_t nowEpoch);

// 是否**允许**自动删除。三条安全规则都在这里。
bool libraryCleanerIsEvictable(const TrackRecord& r, uint32_t playingLocalId);

// Dry-run：算出计划并把候选按 score 从高到低打印出来，**不删除任何文件**。
//
// candidateOut/maxCandidates 可选：填入按 score 降序排列的下标，
// 供将来真实删除时使用；传 nullptr 表示只打印。
// 返回填入的候选数。
uint16_t libraryCleanerDryRun(const TrackRecord* records, uint16_t count,
                              uint64_t incomingBytes, uint32_t nowEpoch,
                              uint32_t playingLocalId,
                              uint16_t* candidateOut, uint16_t maxCandidates);
