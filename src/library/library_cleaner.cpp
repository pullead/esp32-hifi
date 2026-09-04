#include "library_cleaner.h"

#include <SD_MMC.h>
#include <stdio.h>

namespace {

constexpr uint32_t kSecondsPerDay = 24u * 60u * 60u;

uint32_t daysSince(uint32_t epoch, uint32_t nowEpoch) {
    if (!epoch || !nowEpoch || nowEpoch <= epoch) return 0;
    return (nowEpoch - epoch) / kSecondsPerDay;
}

uint32_t clampU32(uint32_t v, uint32_t hi) { return v > hi ? hi : v; }

} // namespace

// ---------------------------------------------------------------------------

void libraryCleanerAssess(const TrackRecord* records, uint16_t count,
                          uint64_t incomingBytes, uint32_t playingLocalId,
                          CleanerPlan* out) {
    if (!out) return;
    *out = CleanerPlan{};
    out->incomingBytes = incomingBytes;

    // 先统计曲目侧。即使 SD 读不到容量，这几个数仍然有意义。
    if (records) {
        for (uint16_t i = 0; i < count; ++i) {
            const TrackRecord& r = records[i];
            if (!(r.flags & kTrackFlagDiscovery)) { ++out->nonDiscoveryCount; continue; }
            if (!libraryCleanerIsEvictable(r, playingLocalId)) { ++out->protectedCount; continue; }
            out->reclaimableBytes += r.fileSize;
            ++out->candidateCount;
        }
    }

    const uint64_t total = SD_MMC.cardSize();
    if (!total) return;  // SD 没挂上或读不到，sdReadable 保持 false
    const uint64_t used = SD_MMC.usedBytes();

    out->sdReadable = true;
    out->totalBytes = total;
    out->usedBytes = used;
    out->freeBytes = used <= total ? total - used : 0;

    // reserve = max(4GB, 总容量 8%)。**不写死 64GB** —— 方案 §37 明确禁止，
    // 换张卡就会全错。
    const uint64_t pct = total / 100ull * kCleanerReservePercent;
    out->reserveBytes = pct > kCleanerReserveMinBytes ? pct : kCleanerReserveMinBytes;

    // 写入 incomingBytes 之后还能剩多少
    const uint64_t freeAfter = out->freeBytes > incomingBytes ? out->freeBytes - incomingBytes : 0;
    if (freeAfter < out->reserveBytes) {
        out->shortfallBytes = out->reserveBytes - freeAfter;
        out->needsCleaning = true;
    }
}

// ---------------------------------------------------------------------------

int32_t libraryCleanerScore(const TrackRecord& r, uint32_t nowEpoch) {
    int32_t score = 0;

    // —— 播放行为：听得多就留，跳得多就删 ——
    if (r.playCount == 0) {
        score += 40;                                   // 从来没播过，最该删
    } else {
        score -= static_cast<int32_t>(clampU32(r.playCount * 5u, 30u));
        // 完整听完比"开了个头"更能说明喜欢，所以单独再减一次
        score -= static_cast<int32_t>(clampU32(r.completedCount * 5u, 30u));
    }
    score += static_cast<int32_t>(clampU32(r.skipCount * 8u, 40u));

    // —— 时间：RTC 没同步时（nowEpoch==0）整段跳过 ——
    // 用一个错误的"现在"去算天数会让整个排序失去意义，不如不算。
    if (nowEpoch) {
        if (r.lastPlayedAt) {
            // 多久没听了，最多 +30
            score += static_cast<int32_t>(clampU32(daysSince(r.lastPlayedAt, nowEpoch), 60u) / 2u);
        } else {
            score += 30;  // 入库以来一次都没听过
        }
        // 入库多久，权重最低（老不等于该删，只是轻微倾向）
        score += static_cast<int32_t>(clampU32(daysSince(r.importedAt, nowEpoch), 180u) / 12u);
    }

    return score;
}

bool libraryCleanerIsEvictable(const TrackRecord& r, uint32_t playingLocalId) {
    // 规则 1：只动本系统自己下载的。用户自己拷进来的歌永远不碰。
    if (!(r.flags & kTrackFlagDiscovery)) return false;
    // 规则 2：Favorite / Keep 永不自动删除
    if (r.flags & (kTrackFlagFavorite | kTrackFlagKeep)) return false;
    // 规则 3：正在播放的不删
    if (playingLocalId && r.localId == playingLocalId) return false;
    // 文件本来就不在了，删不了也不用删（但它仍占索引，由别处处理）
    if (r.flags & kTrackFlagMissing) return false;
    return true;
}

// ---------------------------------------------------------------------------

uint16_t libraryCleanerDryRun(const TrackRecord* records, uint16_t count,
                              uint64_t incomingBytes, uint32_t nowEpoch,
                              uint32_t playingLocalId,
                              uint16_t* candidateOut, uint16_t maxCandidates) {
    CleanerPlan plan{};
    libraryCleanerAssess(records, count, incomingBytes, playingLocalId, &plan);

    if (!plan.sdReadable) {
        printf("[CLEAN] SD capacity unreadable -- skipping\n");
        return 0;
    }

    // 计数已由 assess 完成，这里只收集候选下标
    uint16_t n = 0;
    if (candidateOut) {
        for (uint16_t i = 0; i < count && n < maxCandidates; ++i) {
            if (libraryCleanerIsEvictable(records[i], playingLocalId)) candidateOut[n++] = i;
        }
    }

    printf("[CLEAN] sd total=%lluMB used=%lluMB free=%lluMB reserve=%lluMB incoming=%lluKB\n",
           plan.totalBytes / (1024 * 1024), plan.usedBytes / (1024 * 1024),
           plan.freeBytes / (1024 * 1024), plan.reserveBytes / (1024 * 1024),
           plan.incomingBytes / 1024);
    printf("[CLEAN] needs_cleaning=%d shortfall=%lluMB | candidates=%u reclaimable=%lluMB "
           "protected=%u user_owned=%u\n",
           plan.needsCleaning ? 1 : 0, plan.shortfallBytes / (1024 * 1024),
           plan.candidateCount, plan.reclaimableBytes / (1024 * 1024),
           plan.protectedCount, plan.nonDiscoveryCount);

    if (!candidateOut || !n) return n;

    // 按 score 降序排（插入排序：候选数最多几百，够用且不占额外内存）
    for (uint16_t a = 1; a < n; ++a) {
        const uint16_t key = candidateOut[a];
        const int32_t keyScore = libraryCleanerScore(records[key], nowEpoch);
        int16_t b = static_cast<int16_t>(a) - 1;
        while (b >= 0 && libraryCleanerScore(records[candidateOut[b]], nowEpoch) < keyScore) {
            candidateOut[b + 1] = candidateOut[b];
            --b;
        }
        candidateOut[b + 1] = key;
    }

    // 只打印会被真正删到的那些：累计到够填上 shortfall 为止。
    // 这样能直观看出"为了腾出 X MB 会牺牲哪几首"，而不是列出全部候选。
    uint64_t acc = 0;
    const uint16_t show = n < 10 ? n : 10;
    printf("[CLEAN] --- dry run, top %u by score (NOTHING IS DELETED) ---\n", show);
    for (uint16_t i = 0; i < show; ++i) {
        const TrackRecord& r = records[candidateOut[i]];
        acc += r.fileSize;
        printf("[CLEAN]  %2u score=%-4ld %6luKB play=%u done=%u skip=%u \"%s\"\n",
               i + 1, static_cast<long>(libraryCleanerScore(r, nowEpoch)),
               static_cast<unsigned long>(r.fileSize / 1024),
               r.playCount, r.completedCount, r.skipCount, r.title);
        if (plan.needsCleaning && acc >= plan.shortfallBytes) {
            printf("[CLEAN]  ^ 到这里就够腾出 %lluMB 了\n", plan.shortfallBytes / (1024 * 1024));
            break;
        }
    }
    return n;
}
