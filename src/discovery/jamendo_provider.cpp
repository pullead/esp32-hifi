#include "jamendo_provider.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {

// ---------------------------------------------------------------------------
// 极小的 JSON 扫描器
//
// 为什么不复用 main.cpp 里那三个 helper（jsonNumber/jsonStringField/
// jsonArrayItems）：它们是 static、在另一个编译单元里拿不到；而且它们是"在整个
// body 里找某个 key"的语义，对"results 数组里每个对象各取一次同名字段"这个需求
// 直接用会串台——第 2 个对象的 name 会被第 1 个的匹配挡住。
//
// 所以这里先把 results 数组切成一个个对象区间，再在**区间内**取字段。
// ---------------------------------------------------------------------------

// 跳过一个 JSON 字符串（p 指向开引号），返回闭引号之后的位置；失败返回 nullptr。
const char* skipString(const char* p, const char* end) {
    if (p >= end || *p != '"') return nullptr;
    ++p;
    while (p < end) {
        if (*p == '\\') { p += 2; continue; }  // 转义，整对跳过
        if (*p == '"') return p + 1;
        ++p;
    }
    return nullptr;
}

// 从 p（指向 '{'）开始找到配对的 '}'，返回它之后的位置。
// 必须跳过字符串内部的花括号，否则歌名里带 '}' 就会把对象切错。
const char* skipObject(const char* p, const char* end) {
    if (p >= end || *p != '{') return nullptr;
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skipString(p, end);
            if (!p) return nullptr;
            continue;
        }
        if (*p == '{') ++depth;
        else if (*p == '}') {
            --depth;
            if (depth == 0) return p + 1;
        }
        ++p;
    }
    return nullptr;
}

// 在 [begin,end) 里找 "key": 的值起始位置
const char* findValue(const char* begin, const char* end, const char* key) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const size_t plen = strlen(pattern);
    for (const char* p = begin; p + plen < end; ++p) {
        if (memcmp(p, pattern, plen) != 0) continue;
        const char* q = p + plen;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        if (q < end && *q == ':') {
            ++q;
            while (q < end && (*q == ' ' || *q == '\t')) ++q;
            return q;
        }
    }
    return nullptr;
}

// 取字符串字段，处理 \" \\ \/ \n 这几个常见转义（Jamendo 的 URL 里 \/ 很常见）
bool getString(const char* begin, const char* end, const char* key, char* out, size_t outSize) {
    if (!out || !outSize) return false;
    out[0] = '\0';
    const char* v = findValue(begin, end, key);
    if (!v || v >= end || *v != '"') return false;
    ++v;
    size_t n = 0;
    while (v < end && *v != '"' && n + 1 < outSize) {
        if (*v == '\\' && v + 1 < end) {
            ++v;
            switch (*v) {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': break;               // 丢掉，UI 上没意义
                case 'u': {                    // \uXXXX：本项目 UI 是 UTF-8，
                    // Jamendo 实际返回的是原始 UTF-8，几乎不会出现 \u。
                    // 真遇到就跳过这 4 位十六进制，不做转换——宁可少个字符，
                    // 也不要吐出半个损坏的多字节序列。
                    v += 4;
                    break;
                }
                default: out[n++] = *v; break;  // \" \\ \/ 都落这里
            }
            ++v;
            continue;
        }
        out[n++] = *v++;
    }
    out[n] = '\0';
    return true;
}

uint32_t getUInt(const char* begin, const char* end, const char* key) {
    const char* v = findValue(begin, end, key);
    if (!v || v >= end) return 0;
    if (*v == '"') ++v;                 // Jamendo 的 id 是字符串形式的数字
    return static_cast<uint32_t>(strtoul(v, nullptr, 10));
}

bool getBool(const char* begin, const char* end, const char* key) {
    const char* v = findValue(begin, end, key);
    if (!v || v + 4 > end) return false;
    return memcmp(v, "true", 4) == 0;
}

} // namespace

// ---------------------------------------------------------------------------

uint8_t jamendoParseTracks(const char* body, RemoteTrack* out, uint8_t maxOut) {
    if (!body || !out || !maxOut) return 0;
    const char* end = body + strlen(body);

    const char* results = findValue(body, end, "results");
    if (!results || results >= end || *results != '[') return 0;
    const char* p = results + 1;

    uint8_t n = 0;
    while (p < end && n < maxOut) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= end || *p != '{') break;   // ']' 或格式异常，结束
        const char* objEnd = skipObject(p, end);
        if (!objEnd) break;

        RemoteTrack t{};
        // ⚠️ 只收允许下载的。过滤放在这里，让"不允许下载的东西根本不会流入下游"
        // 成为结构上的保证（见 music_provider.h 的说明）。
        t.downloadAllowed = getBool(p, objEnd, "audiodownload_allowed");
        if (t.downloadAllowed) {
            const uint32_t id = getUInt(p, objEnd, "id");
            snprintf(t.providerTrackId, sizeof(t.providerTrackId), "%lu", static_cast<unsigned long>(id));
            getString(p, objEnd, "name", t.title, sizeof(t.title));
            getString(p, objEnd, "artist_name", t.artist, sizeof(t.artist));
            getString(p, objEnd, "album_name", t.album, sizeof(t.album));
            getString(p, objEnd, "audiodownload", t.audioUrl, sizeof(t.audioUrl));
            // 方案 §6：album track 的 image 等同于 album_image，single 的
            // album_image 可能为空。统一读 image 就能少一个分支。
            getString(p, objEnd, "image", t.coverUrl, sizeof(t.coverUrl));
            t.durationSec = getUInt(p, objEnd, "duration");
            // Jamendo 不给文件大小。按 MP3 VBR 约 160kbps 粗估，
            // 只用于下载前的空间预算，不要求准确。
            t.sizeHintBytes = t.durationSec * 20000u;

            if (id && t.audioUrl[0]) out[n++] = t;
        }
        p = objEnd;
    }
    return n;
}

// ---------------------------------------------------------------------------

void JamendoProvider::setClientId(const char* clientId) {
    if (!clientId) { m_clientId[0] = '\0'; return; }
    strlcpy(m_clientId, clientId, sizeof(m_clientId));
}

uint8_t JamendoProvider::fetchCandidates(const DiscoveryRequest& request,
                                         RemoteTrack* out, uint8_t maxOut) {
    m_lastError[0] = '\0';
    if (!available()) {
        strlcpy(m_lastError, "client_id not configured", sizeof(m_lastError));
        return 0;
    }
    if (!out || !maxOut) return 0;

    uint8_t limit = request.limit ? request.limit : kMaxTracksPerRequest;
    if (limit > kMaxTracksPerRequest) limit = kMaxTracksPerRequest;  // 见 music_provider.h
    if (limit > maxOut) limit = maxOut;

    char url[400];
    int len = snprintf(url, sizeof(url),
                       "https://api.jamendo.com/v3.0/tracks/?client_id=%s&format=json"
                       "&limit=%u&offset=%u&audiodownload_allowed=true"
                       "&audiodlformat=mp32&imagesize=200&include=musicinfo",
                       m_clientId, limit, request.offset);
    if (request.lang && request.lang[0] && len > 0 && len < static_cast<int>(sizeof(url))) {
        len += snprintf(url + len, sizeof(url) - len, "&lang=%s", request.lang);
    }
    if (request.order && request.order[0] && len > 0 && len < static_cast<int>(sizeof(url))) {
        snprintf(url + len, sizeof(url) - len, "&order=%s", request.order);
    }

    WiFiClientSecure client;
    client.setInsecure();  // 和天气、云音乐同样的取舍：不做证书固定
    HTTPClient http;
    if (!http.begin(client, url)) {
        strlcpy(m_lastError, "http.begin failed", sizeof(m_lastError));
        return 0;
    }
    http.setTimeout(12000);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(m_lastError, sizeof(m_lastError), "HTTP %d", code);
        http.end();
        return 0;
    }

    // 单次限 20 条正是为了让这个 String 保持在可接受的量级（审计 §5）。
    // CONFIG_SPIRAM_USE_MALLOC=1 会让大块回落到 PSRAM，但仍然不该无限制拿。
    const String body = http.getString();
    http.end();

    const uint8_t n = jamendoParseTracks(body.c_str(), out, limit);
    if (!n) snprintf(m_lastError, sizeof(m_lastError), "0 tracks from %u bytes", body.length());
    printf("[JAMENDO] lang=%s offset=%u -> %u tracks (%u bytes)\n",
           request.lang ? request.lang : "any", request.offset, n, body.length());
    return n;
}
