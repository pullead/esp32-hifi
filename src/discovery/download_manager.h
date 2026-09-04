// 本地音乐库 2.0 —— 下载管理（方案 §18/§19/§21/§32）
//
// 职责：把一个 URL 流式写到 SD，成功后原子 rename 到最终路径。
// **不管挑哪首、不管写索引** —— 那是上层（daily_discovery）的事。
//
// ⚠️ 三条硬约束：
//
//   1. **完整文件绝不进 RAM。** HTTP 流 → 小块缓冲 → SD，一路 chunk 到底。
//      这块板内部 SRAM 只剩十几 KB（见 docs/LOCAL_LIBRARY_V2_AUDIT.md §2）。
//   2. **先写 .part，校验通过才 rename。** 断电/断网只会留下临时文件，
//      不会让一个半截的 .mp3 混进曲库被当成正常歌曲。
//   3. **下载前先算空间。** 写满 SD 比下载失败严重得多。
//
// ⚠️ 关于 R6（下载写 SD 与播放读 SD 争用）：本模块**不自己判断该不该下载**，
// 由调用方决定时机。但它提供 chunk 之间的让步（见 kYieldEveryChunks），
// 让音频任务有机会插进来。Phase 1 已经吃过一次亏——把 SD 写放进播放启动路径
// 导致本地和电台都播不了（见 DEV_LOG §10）。

#pragma once

#include <stdint.h>
#include <stddef.h>

enum class DownloadResult : uint8_t {
    Ok = 0,
    NotConnected,     // 没联网
    HttpError,        // 非 200，或连接失败
    NoSpace,          // 空间不够（含 reserve）
    TooLarge,         // 超过单文件上限
    OpenFailed,       // .part 打不开
    WriteFailed,      // 写盘失败（多半是满了）
    Truncated,        // 收到的字节数与 Content-Length 对不上
    RenameFailed,     // 最后一步失败
    Stalled,          // 传输中途停住，超过 kStallTimeoutMs 没有新数据
};

struct DownloadStats {
    uint32_t bytesWritten = 0;
    uint32_t expectedBytes = 0;   // Content-Length；服务器不给就是 0
    uint32_t elapsedMs = 0;
    int      httpCode = 0;
};

const char* downloadResultName(DownloadResult r);

// ⚠️ 调用方必须知道：**单曲下载失败是常态，不是异常。**
// Jamendo 的 audiodownload_allowed=true 只说明"授权上允许下载"，不代表存储端
// 真的能给出文件。实测 2026-09-04：id=2034080 恒定 HTTP 500（PC 直连 curl 复现，
// 与板子无关），同批的 1593988 / 1932670 正常。
// 所以上层要按"试下一条"来设计，而不是把失败当成整轮同步的终止条件。

// 单文件上限。Jamendo 的 mp32 一首大约 3~10MB；20MB 足够宽松，
// 又能挡住"服务器返回了个奇怪的大文件"这种情况。
constexpr uint32_t kDownloadMaxBytes = 20u * 1024 * 1024;

// 把 url 下载到 finalPath。
//
// 流程：算空间 → 开 .part → 流式写 → 校验长度 → 删旧文件 → rename。
// 任何一步失败都会清掉 .part，**不会留下半截文件**。
//
// tmpPath 传 nullptr 时自动用 "/music/tmp/<finalPath 的文件名>.part"。
DownloadResult downloadToFile(const char* url, const char* finalPath,
                              const char* tmpPath, DownloadStats* stats);

// 开机清理残留的 .part（方案 §19）。
// 第一版直接删掉重下，不做 HTTP Range 续传。返回删掉的个数。
uint16_t downloadCleanupStalePartFiles();
