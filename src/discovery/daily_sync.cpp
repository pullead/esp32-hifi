#include "daily_sync.h"

#include "download_manager.h"
#include "../library/library_cleaner.h"
#include "../library/library_store.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kSecondsPerDay = 86400;

// 电台在播时最多等多久再放弃这一轮。10 分钟：够覆盖"听完一首/换台"这类
// 短暂占用，又不会让同步任务挂着几小时。
constexpr uint32_t kNetAudioWaitMaxMs = 10u * 60 * 1000;

// 由调用方注入，判断是否有网络供给的音频在播。
DownloadNetAudioFn s_netAudioFn = nullptr;

// 风格配额（2026-09-05 从"语言"改过来）。
//
// 为什么弃用语言维度：实测 Jamendo 的 lang 过滤很松，按元数据/标签筛，
// 返回的大多数并不是该语言的歌 —— lang=zh 的 6 条里只有 1 条真是中文
// （"凌晨三点的便利店 - 小风啊哈"），其余是 ProleteR、Prorock 之类。
//
// 风格标签则很干净：实测 tags=pop / rock / hiphop 各返回 5/5 全部可下载、
// 无警告。这是 Jamendo 真正擅长的维度。
struct GenreSlot {
    const char* tags;      // nullptr = 不限（回填档）
    uint8_t     quota;
    uint16_t*   offset;    // 指向 DailySyncState 里对应的 offset
};

// 把 epoch 格式化成 Jamendo 要的 "YYYY-MM-DD"
void formatDay(uint32_t epoch, char* out, size_t outSize) {
    if (!out || outSize < 11) return;
    const time_t t = static_cast<time_t>(epoch);
    struct tm ti;
    gmtime_r(&t, &ti);
    snprintf(out, outSize, "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
}

// 一条候选是否值得尝试
bool candidateUsable(const RemoteTrack& t) {
    if (!t.downloadAllowed) return false;   // 解析器已经滤过，这里是第二道
    if (!t.audioUrl[0]) return false;
    if (!t.providerTrackId[0]) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

void dailySyncSetNetAudioFn(DownloadNetAudioFn fn) { s_netAudioFn = fn; }

bool dailySyncOwnsTrack(const TrackRecord* records, uint16_t count,
                        uint8_t provider, const char* providerTrackId) {
    if (!records || !providerTrackId || !providerTrackId[0]) return false;
    for (uint16_t i = 0; i < count; ++i) {
        if (records[i].provider != provider) continue;
        if (strncmp(records[i].providerTrackId, providerTrackId,
                    sizeof(records[i].providerTrackId)) == 0) {
            return true;
        }
    }
    return false;
}

void dailySyncBuildPath(char* out, size_t outSize,
                        const char* providerName, const char* trackId) {
    if (!out || !outSize) return;
    snprintf(out, outSize, "/music/tracks/%s_%s.mp3",
             providerName ? providerName : "x",
             trackId ? trackId : "0");
}

bool dailySyncDue(const DailySyncState& state, uint32_t nowEpoch) {
    // ⚠️ RTC 没同步就不跑。否则 lastRunDay 恒为 0、今天也恒为 0，
    // **每次重启都会被当成新的一天**，反复下载。
    if (!nowEpoch) return false;
    return (nowEpoch / kSecondsPerDay) != state.lastRunDay;
}

// ---------------------------------------------------------------------------

void dailySyncRun(MusicProvider& provider,
                  TrackRecord* records, uint16_t* count, uint16_t capacity,
                  uint32_t nowEpoch, uint32_t playingLocalId,
                  const DailySyncConfig& cfg,
                  DailySyncState* state, DailySyncStats* out) {
    DailySyncStats local{};
    DailySyncStats& st = out ? *out : local;
    st = DailySyncStats{};

    if (!records || !count || !state) {
        snprintf(st.lastError, sizeof(st.lastError), "bad args");
        return;
    }
    if (!provider.available()) {
        snprintf(st.lastError, sizeof(st.lastError), "provider unavailable");
        return;
    }

    const uint32_t startMs = millis();

    // 配额切分。tracksPerDay=10 时是 pop4 / rock4 / hiphop4（受总数封顶），
    // 实际会在拿满 10 首时停下，剩余靠回填。
    //
    // ⚠️ 用 (n + 2) / 3 而不是 n * 3 / 10：后者在 tracksPerDay 较小时会算出 0，
    // 整档被静默跳过 —— 2026-09-05 的 3 首验证就是这么把配额路径整个漏掉的
    // （3 * 3 / 10 = 0），跑完才发现那条路根本没走到。
    const uint8_t per = (cfg.tracksPerDay + 2) / 3;
    GenreSlot slots[] = {
        {"pop",    per, &state->offsetPop},
        {"rock",   per, &state->offsetRock},
        {"hiphop", per, &state->offsetHiphop},
        // 回填档：不带风格。留着它是因为**间歇性空返回是常态**（约 15~30%），
        // 某一档撞上就会少歌，靠这档补齐。
        {nullptr, cfg.tracksPerDay, &state->offsetAny},
    };

    // 滚动的"近期"窗口。用 RTC 算 —— 这依赖 s_f_rtc 真的可用，而它在
    // 2026-09-05 之前因为 RTIME::hasValidTime() 的成员遮蔽 bug 恒为 false。
    char dateFrom[12] = {0};
    char dateTo[12] = {0};
    if (nowEpoch && cfg.recentDays) {
        formatDay(nowEpoch, dateTo, sizeof(dateTo));
        const uint32_t span = static_cast<uint32_t>(cfg.recentDays) * 86400u;
        formatDay(nowEpoch > span ? nowEpoch - span : 0, dateFrom, sizeof(dateFrom));
        printf("[SYNC] recent window %s .. %s\n", dateFrom, dateTo);
    }

    uint8_t limit = cfg.candidatesPerFetch;
    if (limit > kMaxTracksPerRequest) limit = kMaxTracksPerRequest;

    // 候选缓冲放 PSRAM：RemoteTrack 约 512B，20 条就是 10KB，
    // 任务栈留给 TLS 握手（见 jamendoProbeTask 的同样取舍）。
    RemoteTrack* cands = static_cast<RemoteTrack*>(ps_calloc(limit, sizeof(RemoteTrack)));
    if (!cands) {
        snprintf(st.lastError, sizeof(st.lastError), "ps_calloc failed");
        return;
    }

    // upsert 需要一张 seen 位图。这里不是扫描，位图内容用不上，
    // 但接口要求非空 —— 给一张丢弃用的。
    const uint16_t seenBytes = libraryStoreSeenBytes(capacity);
    uint8_t* seen = static_cast<uint8_t*>(ps_calloc(seenBytes, 1));
    if (!seen) {
        free(cands);
        snprintf(st.lastError, sizeof(st.lastError), "ps_calloc(seen) failed");
        return;
    }

    for (uint8_t s = 0; s < sizeof(slots) / sizeof(slots[0]); ++s) {
        const GenreSlot& slot = slots[s];
        uint8_t gotForSlot = 0;

        while (gotForSlot < slot.quota &&
               st.downloaded < cfg.tracksPerDay &&
               st.attempts < cfg.maxAttempts &&
               !st.stoppedNoSpace) {

            // —— 拉候选（带重试，见头文件约束 2）——
            uint8_t n = 0;
            for (uint8_t attempt = 0; attempt < cfg.fetchRetries; ++attempt) {
                DiscoveryRequest req;
                req.tags = slot.tags;
                req.limit = limit;
                req.offset = *slot.offset;
                // 周热度 + 日期窗口 = "近期热门"。单用排序会捞到十几年前的歌。
                req.order = "popularity_week";
                req.dateFrom = dateFrom[0] ? dateFrom : nullptr;
                req.dateTo = dateTo[0] ? dateTo : nullptr;
                n = provider.fetchCandidates(req, cands, limit);
                if (n) break;
                ++st.emptyFetches;
                vTaskDelay(pdMS_TO_TICKS(cfg.fetchRetryDelayMs));
            }
            if (!n) {
                // 这一档拉不到东西，换下一档，别死磕。
                printf("[SYNC] tags=%s offset=%u -> empty, moving on\n",
                       slot.tags ? slot.tags : "any", *slot.offset);
                break;
            }

            // offset 推进：**不管这批用没用上都要推**，否则下一轮还是这批。
            //
            // ⚠️ **用 lastRawCount() 判断分页到底，不能用 n。**
            // n 是**过滤后**的数量（只留可下载的）。2026-09-05 实测：请求 20 条、
            // API 给 20 条、其中 1 条不可下载 → n = 19，于是 "n < limit ⇒ 到底了"
            // 恒成立，offset 永远停在 0，每个风格的候选池被锁死在第一页 19 首，
            // offset 推进等于完全失效。
            //
            // 加了 datebetween 之后池子小很多（一年内 + 某个风格），
            // 不回绕的话 offset 会一路涨到池外，之后每次都拉空 —— 所以回绕要有，
            // 只是判据得对。
            const uint16_t raw = provider.lastRawCount();
            if (raw < limit) {
                printf("[SYNC] tags=%s pool end at offset=%u (raw=%u), wrap to 0\n",
                       slot.tags ? slot.tags : "any", *slot.offset, raw);
                *slot.offset = 0;
            } else {
                // 按**请求条数**推进，不是按过滤后的 n —— 否则被过滤掉的那些
                // 会在下一页重新出现，越翻越慢。
                *slot.offset = static_cast<uint16_t>(*slot.offset + limit);
            }

            bool progressed = false;
            for (uint8_t i = 0; i < n; ++i) {
                if (st.downloaded >= cfg.tracksPerDay) break;
                if (st.attempts >= cfg.maxAttempts) break;

                const RemoteTrack& t = cands[i];
                if (!candidateUsable(t)) continue;

                if (dailySyncOwnsTrack(records, *count, kProviderJamendo, t.providerTrackId)) {
                    ++st.skippedOwned;
                    continue;
                }
                if (*count >= capacity) {
                    snprintf(st.lastError, sizeof(st.lastError), "index full (%u)", capacity);
                    st.stoppedNoSpace = true;
                    break;
                }

                // —— 空间评估。不够就停，**不删任何东西**（头文件约束 5）——
                {
                    CleanerPlan plan{};
                    const uint64_t need = t.sizeHintBytes ? t.sizeHintBytes : (4ull * 1024 * 1024);
                    libraryCleanerAssess(records, *count, need, playingLocalId, &plan);
                    if (plan.sdReadable && plan.needsCleaning) {
                        snprintf(st.lastError, sizeof(st.lastError),
                                 "no space: short %luKB",
                                 static_cast<unsigned long>(plan.shortfallBytes / 1024));
                        st.stoppedNoSpace = true;
                        break;
                    }
                }

                // —— 电台在播就先别开新的下载 ——
                //
                // 2026-09-05 用户实测：后台下载时**电台明显卡顿，本地音乐不卡**。
                // download_manager 里的限速是安全网（针对下载途中电台才起播的
                // 情况），但更好的做法是**一开始就别撞上** —— 每日同步有 24 小时
                // 可用，用户的听感只有一次。
                //
                // 等不到就跳过这一档，不要把整轮卡死在这里。
                if (s_netAudioFn) {
                    const uint32_t waitStart = millis();
                    bool waited = false;
                    while (s_netAudioFn() && millis() - waitStart < kNetAudioWaitMaxMs) {
                        if (!waited) {
                            printf("[SYNC] network audio playing, deferring download\n");
                            waited = true;
                        }
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (s_netAudioFn()) {
                        // 还在播 —— 今天这一轮就到这儿，明天再说。
                        snprintf(st.lastError, sizeof(st.lastError),
                                 "deferred: network audio still playing");
                        st.deferredNetAudio = true;
                        break;
                    }
                }

                char path[160];
                dailySyncBuildPath(path, sizeof(path), provider.name(), t.providerTrackId);

                ++st.attempts;
                DownloadStats ds{};
                const DownloadResult r = downloadToFile(t.audioUrl, path, nullptr, &ds);
                printf("[SYNC] %s try#%u %s %luKB %lums thr=%lux rate=%lums net=%d \"%s\"\n",
                       slot.tags ? slot.tags : "any", st.attempts,
                       downloadResultName(r),
                       static_cast<unsigned long>(ds.bytesWritten / 1024),
                       static_cast<unsigned long>(ds.elapsedMs),
                       static_cast<unsigned long>(ds.throttleEvents),
                       static_cast<unsigned long>(ds.rateLimitedMs),
                       ds.sawNetAudio ? 1 : 0,
                       t.title);

                if (r != DownloadResult::Ok) {
                    // 单曲失败是常态 —— 记账、跳过、继续（头文件约束 1）。
                    ++st.failedDownload;
                    snprintf(st.lastError, sizeof(st.lastError), "%s on %s",
                             downloadResultName(r), t.providerTrackId);
                    continue;
                }

                // —— 并入索引 ——
                TrackRecord rec{};
                strlcpy(rec.path, path, sizeof(rec.path));
                strlcpy(rec.title, t.title, sizeof(rec.title));
                strlcpy(rec.artist, t.artist, sizeof(rec.artist));
                strlcpy(rec.album, t.album, sizeof(rec.album));
                strlcpy(rec.providerTrackId, t.providerTrackId, sizeof(rec.providerTrackId));
                rec.provider = kProviderJamendo;
                rec.fileSize = ds.bytesWritten;
                rec.durationSec = t.durationSec;
                // ⚠️ Discovery 标记必须在这里打上。它是 cleaner 判断"可淘汰"的
                // 唯一依据 —— 漏了的话这首歌会被当成用户自己拷进来的，永不淘汰。
                rec.flags = kTrackFlagDiscovery;

                const int32_t idx = libraryStoreUpsert(records, count, capacity,
                                                       rec, seen, nowEpoch);
                if (idx < 0) {
                    snprintf(st.lastError, sizeof(st.lastError), "upsert failed");
                    st.stoppedNoSpace = true;
                    break;
                }

                st.indexDirty = true;
                ++st.downloaded;
                ++gotForSlot;
                st.bytes += ds.bytesWritten;
                progressed = true;

                // 这里**不写事件日志**。事件日志是给"必须靠回放才能重建的统计"
                // 用的（播放次数、收藏），而"导入"已经由 importedAt 字段和索引
                // 本身记住了，回放一条导入事件什么也做不了 —— 只会给每首歌
                // 多一次 SD 写。

                // 让一拍，别把 SD 和网络连着占死。
                vTaskDelay(pdMS_TO_TICKS(500));
                break;   // 这一档拿到一首就重新评估配额
            }

            if (!progressed) {
                // 整批候选一首都没下成（全是已拥有 / 全是坏曲目）。
                // 继续 while 会用推进后的 offset 再拉一批，这是想要的行为；
                // 但要靠 maxAttempts 和 downloaded 兜住，不会无限转。
                if (st.attempts >= cfg.maxAttempts) break;
                if (st.skippedOwned > cfg.maxAttempts * 4) {
                    // 大面积重复，说明这一档已经被下完了，别再空转。
                    printf("[SYNC] tags=%s exhausted (owned=%u)\n",
                           slot.tags ? slot.tags : "any", st.skippedOwned);
                    break;
                }
            }
        }
    }

    free(seen);
    free(cands);

    // 只有真正跑完一轮才记日期。**中途因为没空间停下的也算跑过** ——
    // 否则会每次触发检查都重跑，把失败放大成循环。
    //
    // ⚠️ 但"因为电台在播而推迟"是例外：那不是失败，是主动让路。
    // 记了日期就等于今天不再尝试，用户听一下午电台就等于当天不更新曲库。
    // 调用方负责在冷却后重试。
    if (!st.deferredNetAudio) {
        state->lastRunDay = nowEpoch / kSecondsPerDay;
    }
    st.elapsedMs = millis() - startMs;

    printf("[SYNC] done: got=%u/%u attempts=%u owned=%u failed=%u empty=%u "
           "nospace=%d deferred=%d %luKB %lums\n",
           st.downloaded, cfg.tracksPerDay, st.attempts, st.skippedOwned,
           st.failedDownload, st.emptyFetches, st.stoppedNoSpace ? 1 : 0,
           st.deferredNetAudio ? 1 : 0,
           static_cast<unsigned long>(st.bytes / 1024),
           static_cast<unsigned long>(st.elapsedMs));
}
