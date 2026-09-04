#include "download_manager.h"

#include "../library/library_cleaner.h"   // CleanerPlan / libraryCleanerAssess

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

namespace {

// 搬运缓冲。2KB 是权衡：
//   - 太小 → SD 写次数多、每次都有事务开销
//   - 太大 → 占内部 SRAM（只剩十几 KB），而且单次写盘时间变长，
//            音频那边更容易饿着
constexpr size_t kChunkBytes = 2048;

// 每搬这么多块就让一次 CPU。音频任务在 core 0 且优先级更高，本来就会抢占，
// 但主动让步能减少长时间独占 SD 的窗口 —— 这是 R6 的缓解手段之一。
constexpr uint8_t kYieldEveryChunks = 8;

// 停滞超时：距上次收到数据超过这么久就放弃。
// 20 秒对家用宽带下一首 3~10MB 的 mp3 足够宽松，又不会让后台任务挂死。
constexpr uint32_t kStallTimeoutMs = 20000;

// 每写这么多就打一行进度。没有它的话，一次慢下载和一次卡死在串口上
// 长得一模一样 —— 这正是这次排查多花一轮的原因。
constexpr uint32_t kProgressEveryBytes = 512u * 1024;

void ensureDir(const char* path) {
    if (!SD_MMC.exists(path)) SD_MMC.mkdir(path);
}

const char* baseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

} // namespace

const char* downloadResultName(DownloadResult r) {
    switch (r) {
        case DownloadResult::Ok:           return "ok";
        case DownloadResult::NotConnected: return "not_connected";
        case DownloadResult::HttpError:    return "http_error";
        case DownloadResult::NoSpace:      return "no_space";
        case DownloadResult::TooLarge:     return "too_large";
        case DownloadResult::OpenFailed:   return "open_failed";
        case DownloadResult::WriteFailed:  return "write_failed";
        case DownloadResult::Truncated:    return "truncated";
        case DownloadResult::RenameFailed: return "rename_failed";
        case DownloadResult::Stalled:      return "stalled";
    }
    return "?";
}

// ---------------------------------------------------------------------------

DownloadResult downloadToFile(const char* url, const char* finalPath,
                              const char* tmpPath, DownloadStats* stats) {
    DownloadStats local{};
    DownloadStats& st = stats ? *stats : local;
    st = DownloadStats{};

    if (!url || !url[0] || !finalPath || !finalPath[0]) return DownloadResult::OpenFailed;
    if (!WiFi.isConnected()) return DownloadResult::NotConnected;

    char tmpBuf[192];
    if (!tmpPath || !tmpPath[0]) {
        snprintf(tmpBuf, sizeof(tmpBuf), "/music/tmp/%s.part", baseName(finalPath));
        tmpPath = tmpBuf;
    }

    ensureDir("/music");
    ensureDir("/music/tmp");
    // finalPath 的父目录也要在。只处理一级（/music/tracks 这种），够用。
    {
        char dir[192];
        strlcpy(dir, finalPath, sizeof(dir));
        char* slash = strrchr(dir, '/');
        if (slash && slash != dir) { *slash = '\0'; ensureDir(dir); }
    }

    const uint32_t startMs = millis();

    WiFiClientSecure client;
    client.setInsecure();   // 和天气 / 云音乐 / Jamendo 一致：不做证书固定
    HTTPClient http;
    if (!http.begin(client, url)) {
        st.elapsedMs = millis() - startMs;
        return DownloadResult::HttpError;
    }
    http.setTimeout(15000);
    // Jamendo 的下载地址会 302 到 CDN
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    st.httpCode = http.GET();
    if (st.httpCode != HTTP_CODE_OK) {
        http.end();
        st.elapsedMs = millis() - startMs;
        return DownloadResult::HttpError;
    }

    const int contentLength = http.getSize();
    if (contentLength > 0) st.expectedBytes = static_cast<uint32_t>(contentLength);

    if (st.expectedBytes > kDownloadMaxBytes) {
        http.end();
        st.elapsedMs = millis() - startMs;
        return DownloadResult::TooLarge;
    }

    // 空间检查。**下载前算，不是写满了才发现** —— 写满 SD 比下载失败严重得多。
    // 用已知长度；服务器不给 Content-Length 时按上限的一半保守估。
    {
        CleanerPlan plan{};
        const uint64_t need = st.expectedBytes ? st.expectedBytes : (kDownloadMaxBytes / 2);
        libraryCleanerAssess(nullptr, 0, need, 0, &plan);
        if (plan.sdReadable && plan.needsCleaning) {
            http.end();
            st.elapsedMs = millis() - startMs;
            return DownloadResult::NoSpace;
        }
    }

    SD_MMC.remove(tmpPath);   // 上次残留的
    File out = SD_MMC.open(tmpPath, "w");
    if (!out) {
        http.end();
        st.elapsedMs = millis() - startMs;
        return DownloadResult::OpenFailed;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[kChunkBytes];
    uint8_t chunkCounter = 0;
    DownloadResult result = DownloadResult::Ok;
    uint32_t lastProgressMs = millis();
    uint32_t nextProgressMark = 0;

    // 传输是否"干净地结束了"。区分两种退出方式：
    //   - 服务器主动关闭 / 收满 Content-Length  → cleanEof = true
    //   - 我们因为卡住而放弃                     → cleanEof 保持 false
    // 这个区别在下面的校验里是决定性的：服务器不给 Content-Length 时，
    // **只有干净结束才能认为文件是完整的**。
    bool cleanEof = false;

    while (http.connected()) {
        if (st.expectedBytes && st.bytesWritten >= st.expectedBytes) { cleanEof = true; break; }

        const size_t avail = stream->available();
        if (!avail) {
            if (!stream->connected()) { cleanEof = true; break; }
            vTaskDelay(pdMS_TO_TICKS(5));
            // ⚠️ 停滞超时。**必须按"距上次收到数据多久"算，不能按"距开始多久
            // 且一个字节都没收到"算** —— 后者是我第一版的写法，有两个漏洞，
            // 2026-09-04 实测都踩到了（板子挂在一首 3MB 的歌上 200 秒不返回）：
            //   1. 传到一半断流：已经写了字节，旧判断永远不成立，无限空转。
            //   2. 服务器不给 Content-Length（chunked/keep-alive）：
            //      expectedBytes=0，上面那条"收满就 break"永远不触发，
            //      **哪怕文件已经下完了也会一直挂着等**。
            // 这是后台每日同步要跑的代码，无限挂起不可接受。
            if (millis() - lastProgressMs > kStallTimeoutMs) {
                result = DownloadResult::Stalled;
                break;
            }
            continue;
        }

        const size_t want = avail > kChunkBytes ? kChunkBytes : avail;
        const int got = stream->readBytes(buf, want);
        if (got <= 0) break;

        if (st.bytesWritten + static_cast<uint32_t>(got) > kDownloadMaxBytes) {
            result = DownloadResult::TooLarge;
            break;
        }
        if (out.write(buf, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
            result = DownloadResult::WriteFailed;   // 多半是写满了
            break;
        }
        st.bytesWritten += static_cast<uint32_t>(got);
        lastProgressMs = millis();

        if (st.bytesWritten / kProgressEveryBytes != nextProgressMark) {
            nextProgressMark = st.bytesWritten / kProgressEveryBytes;
            printf("[DL] %luKB/%luKB %lums\n",
                   static_cast<unsigned long>(st.bytesWritten / 1024),
                   static_cast<unsigned long>(st.expectedBytes / 1024),
                   static_cast<unsigned long>(millis() - startMs));
        }

        // 主动让步，别长时间独占 SD（R6）
        if (++chunkCounter >= kYieldEveryChunks) {
            chunkCounter = 0;
            vTaskDelay(1);
        }
    }

    out.close();
    http.end();
    st.elapsedMs = millis() - startMs;

    // 长度校验。服务器给了 Content-Length 就必须对得上——**半截文件绝不能
    // 混进曲库**被当成正常歌曲（方案 §18）。
    if (result == DownloadResult::Ok) {
        if (!st.bytesWritten) {
            result = DownloadResult::Truncated;
        } else if (st.expectedBytes) {
            // 给了长度就必须严格对上
            if (st.bytesWritten != st.expectedBytes) result = DownloadResult::Truncated;
        } else if (!cleanEof) {
            // 没给长度、又不是干净结束 —— 无法证明文件完整，就当它不完整。
            // 宁可白下一次，也不能让半截 mp3 混进曲库（见 .h 硬约束 2）。
            result = DownloadResult::Truncated;
        }
    }

    if (result != DownloadResult::Ok) {
        SD_MMC.remove(tmpPath);
        return result;
    }

    SD_MMC.remove(finalPath);   // 覆盖旧的（正常流程里不该存在）
    if (!SD_MMC.rename(tmpPath, finalPath)) {
        SD_MMC.remove(tmpPath);
        return DownloadResult::RenameFailed;
    }
    return DownloadResult::Ok;
}

// ---------------------------------------------------------------------------

uint16_t downloadCleanupStalePartFiles() {
    if (!SD_MMC.exists("/music/tmp")) return 0;
    File dir = SD_MMC.open("/music/tmp");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    // 先收集再删：边遍历边删目录项，行为依实现而定，不可靠。
    char victims[8][192];
    uint16_t n = 0;
    for (;;) {
        File e = dir.openNextFile();
        if (!e) break;
        const char* name = e.name();
        const size_t len = name ? strlen(name) : 0;
        if (!e.isDirectory() && len > 5 && strcmp(name + len - 5, ".part") == 0 && n < 8) {
            snprintf(victims[n], sizeof(victims[n]), "/music/tmp/%s", name);
            ++n;
        }
        e.close();
    }
    dir.close();

    for (uint16_t i = 0; i < n; ++i) {
        SD_MMC.remove(victims[i]);
        printf("[DL] removed stale %s\n", victims[i]);
    }
    return n;
}
