// Jamendo 音源（方案 §5/§6/§31）
//
// 选它做第一版的理由：CC 授权、API 明确给出 `audiodownload_allowed`，
// 合法性清晰。
//
// ⚠️ 已知的内容限制（2026-09-03 与用户确认后接受）：Jamendo 以欧美独立音乐
// 为主，**中文和日文的可下载曲目非常少**。方案里"中3/日3/英3"的配额基本填不满，
// 回填会是常态而不是异常路径 —— 调用方必须把回填当一等公民处理，
// 而不是当错误分支。
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
