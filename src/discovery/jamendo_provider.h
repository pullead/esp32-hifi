// Jamendo 音源（方案 §5/§6/§31）
//
// 选它做第一版的理由：CC 授权、API 明确给出 `audiodownload_allowed`，
// 合法性清晰。
//
// ⚠️ 已知的内容限制（2026-09-05 实测修正）：
//
//   1. **lang 过滤很松。** lang=zh / lang=ja 都能正常返回结果，但返回的大多数
//      并不是该语言的歌 —— lang=zh 的 6 条里只有 1 条真是中文。
//      （原先记的是"中日可下载曲目非常少"，那个说法不准确。）
//
//   2. **API 会间歇性返回空数组。** 实测约 15~30% 的请求返回
//      results_count=0、status=success、无警告，**与参数无关**（不带 lang 连打
//      6 次出现 1 次空；lang=ja 连打 3 次出现 2 次空）。
//      所以一次拉空绝不能当成"没有内容"——必须重试。
//      调用方仍要把回填当一等公民处理。
//
// client_id 从 NVS 读，**不进仓库**（审计 R12）。

#pragma once

#include "music_provider.h"

class JamendoProvider : public MusicProvider {
  public:
    // clientId 由调用方从 NVS 取出后传入；空串表示未配置。
    void setClientId(const char* clientId);

    const char* name() const override { return "jamendo"; }
    bool available() const override { return m_clientId[0] != '\0'; }

    uint8_t fetchCandidates(const DiscoveryRequest& request,
                            RemoteTrack* out, uint8_t maxOut) override;

    // 最近一次失败的原因，用于 UI/日志。没失败过返回空串。
    const char* lastError() const { return m_lastError; }

  private:
    char m_clientId[40]{};
    char m_lastError[64]{};
};

// 解析 Jamendo 的 tracks 响应体。**独立出来是为了能不联网就测**：
// 喂一段抓下来的真实 JSON 进去就能验证解析逻辑，不用等 client_id、
// 也不用受网络波动影响。fetchCandidates() 内部就是调它。
//
// 返回填入的条数。只填 audiodownload_allowed == true 的曲目。
uint8_t jamendoParseTracks(const char* body, RemoteTrack* out, uint8_t maxOut);
