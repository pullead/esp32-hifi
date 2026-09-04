// 本地音乐库 2.0 —— 音源抽象
//
// 方案 §5 要求第一版就把接口定下来，不要把曲库绑死在一个网站。第一版实现是
// Jamendo（CC 授权、API 明确给出 audiodownload_allowed），将来可以再加
// NAS / WebDAV / 其他明确允许离线保存的来源。
//
// ⚠️ 只允许接**明确许可离线下载**的音源。商业流媒体的完整音频不在此列
// （方案 §37 的红线）。

#pragma once

#include <stdint.h>

// 一条候选曲目。**尚未下载**，只是"可以去下载的东西"。
//
// 尺寸约 512 字节。20 条就是 10KB —— **不要放在栈上**，调用方应当用
// ps_malloc 分配在 PSRAM（这块板内部 SRAM 只剩十几 KB，见
// docs/LOCAL_LIBRARY_V2_AUDIT.md §2）。
struct RemoteTrack {
    char     providerTrackId[24]{};
    char     title[64]{};
    char     artist[48]{};
    char     album[48]{};
    char     audioUrl[192]{};   // 直接可下载的音频地址
    char     coverUrl[128]{};
    uint32_t durationSec = 0;
    uint32_t sizeHintBytes = 0; // provider 给不出时为 0，由时长粗估
    bool     downloadAllowed = false;
};

struct DiscoveryRequest {
    // 语言。nullptr / "" = 不限。
    // ⚠️ 这是**目标**不是硬条件：Jamendo 的中日文可下载曲目很少，
    // 请求 zh/ja 经常返回不足甚至为空，调用方必须准备好回填（方案 §4）。
    const char* lang = nullptr;

    // 单次请求条数。**硬上限 kMaxTracksPerRequest**。
    // 理由见审计 §5：现有 JSON 解析是把整个响应装进内存再扫，条数一多就会
    // 撞上内部 SRAM 的天花板。要更多候选就多请求几次、换 offset。
    uint8_t limit = 20;

    // 分页偏移。每天用不同的 offset 才不会天天拿到同一批（方案 §12）。
    uint16_t offset = 0;

    // 排序方式，直接透传给 provider。nullptr = 用 provider 的默认值。
    const char* order = nullptr;
};

constexpr uint8_t kMaxTracksPerRequest = 20;

class MusicProvider {
  public:
    virtual ~MusicProvider() = default;

    // 供日志与 TrackRecord::provider 使用的短名
    virtual const char* name() const = 0;

    // 配置是否齐全（比如 client_id 有没有配）。false 时不要调 fetchCandidates。
    virtual bool available() const = 0;

    // 拉候选。返回实际填入的条数。
    //
    // 实现必须保证：**只返回 downloadAllowed == true 的曲目**。
    // 过滤放在 provider 里而不是调用方，是为了让"不允许下载的东西根本不会流入
    // 下游"成为结构上的保证，而不是依赖每个调用方都记得检查。
    virtual uint8_t fetchCandidates(const DiscoveryRequest& request,
                                    RemoteTrack* out, uint8_t maxOut) = 0;
};
