# 本地音乐库 2.0 — Phase 0 仓库审计

> 对应方案：`ESP32_HiFi_本地音乐库2.0_自动发现与曲库轮换_ClaudeCode实施方案.docx`
> 审计分支：`codex/usb-dac-no-sound-fix-20260906`
> 日期：2026-09-07
>
> **本阶段只审计，未改动任何业务代码。**

---

## 1. 当前本地曲库的完整数据流

```
开机
 └─ SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)      main.cpp:6002
    SD_MMC.begin("/sdcard", true /*1-bit*/, false, freq)   main.cpp:6003
       │
       ▼
 localLibraryScan()  →  scanMusicDir("/", 0)               main.cpp:2221
       │  递归最深 6 层，从 SD **根目录**开始
       │  只接受 .mp3（endsWithIcase，main.cpp:2196）
       │  每首 parseId3Tags() 读 TIT2/TP1/TAL + 探测 APIC/PIC
       ▼
 s_localTracks[]  (ps_calloc → PSRAM)                      main.cpp:2220
 上限 kMaxLocalTracks（Phase 1 已由 300 改为 2000）          main.cpp:1614
       │
       ▼
 playerCoreLocalTrack(index, LocalTrackItem*)  ← UI 通过 PlayerService 读
       │
       ▼
 playerCorePlaySdFile(path, positionSeconds)               main.cpp:1377
       └─ connecttoFS("SD_MMC", path)                      main.cpp:1240
             └─ audio.connecttoFS(SD_MMC, filename, ...)   main.cpp:1257
```

### `LocalTrackItem`（player_service.h:177）

```cpp
struct LocalTrackItem {
    char path[160]; char title[64]; char artist[48]; char album[48];
    bool hasArt;
};   // ≈ 322 字节
```

---

## 2. 关键事实（已核实）

| 项 | 实际情况 | 对方案的影响 |
|---|---|---|
| **曲库上限** | ~~300~~ → **2000**（Phase 1 已改） | 实测分配成功：PSRAM 少了约 564KB，正合 `(2000-300)×322` |
| **记录大小** | ≈322 B，`ps_calloc` 到 PSRAM | 2000 首 = 644KB；5000 首 = 1.6MB |
| **PSRAM 余量** | 正常模式启动后约 **2.58 MB** 空闲 | 2000 首舒服，5000 首紧张 |
| **内部 SRAM 余量** | 扩容后 LVGL 起来仅约 **14.9 KB**，最大块 7680 B；**DMA 可用仅 7155 B** | 🔴 真正的瓶颈是 DMA 内存，不是 TLS（见 §9.1 更正） |
| **扫描格式** | **只收 `.mp3`**（播放器本身支持 mp3/aac/m4a/wav/flac/opus/ogg） | 与 Jamendo `mp32` 正好一致，第一版无冲突 |
| **扫描起点** | SD **根目录**递归，不是 `/music/` | 方案的 `/music/tracks/` 会被自动扫到，无需改扫描器 |
| **SD 总线** | **SD_MMC 独立引脚（1-bit）**，与驱动 LCD 的 SPI2 **完全分离** | ✅ 方案担心的"SD 与 UI 抢 SPI"在本硬件上**不存在** |
| **SD 容量 API** | `SD_MMC.cardSize()` / `SD_MMC.usedBytes()` 已在用（main.cpp:1775/1820） | ✅ 直接复用，不用自己写 |
| **数据库** | `managed_components/` 里**没有 SQLite**；有 `joltwallet__littlefs` | 见 §4 结论 |
| **JSON** | **没有 ArduinoJson**。手写扫描：`jsonNumber()` / `jsonStringField()` / `jsonArrayItems()`，签名都是 `const String&` | 已定 A 方案，见 §5 |
| **HTTP/TLS** | `HTTPClient` + `WiFiClientSecure`，`setInsecure()`（无证书校验） | 复用，见 §6 |
| **歌词** | `.lrc` 同名旁挂文件（main.cpp:2696） | 下载的歌暂无歌词，可后续接 lrclib |
| **封面** | ID3 内嵌 APIC/PIC，**只认 JPEG**（无 PNG 解码器，main.cpp:2079） | ⚠️ Jamendo 封面必须存成 **JPEG** |
| **NVS** | `Preferences pref`，`pref.begin("Pref")`（main.cpp:5634） | 复用存 sync_state |
| **时间有效性** | `s_f_rtc = rtc.hasValidTime()`（main.cpp:5551/6804） | ✅ 方案要求的"NTP 未同步不写日期"有现成判据 |

---

## 3. 每日任务该挂在哪

### ⚠️ `onWifiNetworkReady()` **一次开机只触发一次**

```cpp
static void onWifiNetworkReady() {
    if (s_lvglNetworkReady) return;   // ← 守卫，永远只跑一次
    setRTC(...); webSrv.begin(); MDNS.begin(); ftpSrv.begin();
    xTaskCreatePinnedToCore(weatherTask, ...);
    s_lvglNetworkReady = true;
}
```

它是 weather task 的挂载点，看起来很适合，**但只挂这里是错的**：设备连续开机好几天时，
第二天永远不会触发新一天的同步。

**正确做法：两处都挂**

1. `onWifiNetworkReady()` —— 开机首次联网的即时触发
2. `loopLvglRuntime()` 的 `s_f_1sec` 块 —— 每秒检查一次
   `WiFi.isConnected() && s_f_rtc && today != lastSuccessfulDate`

第 2 处才是真正的"每日"保证。同步锁（方案 §10）由 `syncState.running` 提供。

---

## 4. 数据库结论：**不用 SQLite，用「定长索引 + 追加日志」**

方案说"SQLite 首选"，但结合本项目实际依赖，结论相反：

- `managed_components/` 里没有 SQLite，要新引入组件
- **内部 SRAM 只剩约 15 KB**，SQLite 的 page cache 无处安放
- 跑在 FAT 上、SD 卡随时可能断电，事务安全性并不比自己控制更好
- 方案自己的原则："不要为了架构好看强行引入一个会显著增加固件体积或
  internal SRAM 的数据库" —— 按这条，结论就该是不用

### 推荐结构

```
/music/library/
├─ tracks.idx      定长记录数组，一条一首，按 index 随机访问
├─ events.log      追加写：play / skip / favorite，永不改写历史
└─ sync_state.json 小文件，整体重写无压力
```

理由：

- **定长记录** → 不需要查询引擎。算 `delete_score` 是在 PSRAM 里对几千条排序，微秒级
- **追加写** → 断电最多丢最后一条，不会损坏整个库。比在 FAT 上做事务简单且可靠
- **零新依赖**，不增加固件体积，不碰内部 SRAM

`events.log` 定期 compact 进 `tracks.idx`（开机时做一次即可）。

---

## 5. JSON 方案（已定：A）

沿用现有手写扫描（`jsonNumber` / `jsonStringField` / `jsonArrayItems`），
**限制单次请求条数到 20 条左右，分多次拿**。

⚠️ 注意现有三个 helper 的签名都是 `const String& body` —— 整个响应先进内存。
Arduino `String` 走普通 heap，但 `CONFIG_SPIRAM_USE_MALLOC=1` 会让大块回落到
PSRAM，所以压力小于最初估计（见 §9.1）。仍然建议限量。
20 条 Jamendo track 的 JSON 约 20~40KB，是可接受的量级；50~100 条不行。

---

## 6. Downloader 该用哪套 HTTP API

复用现有的 `HTTPClient` + `WiFiClientSecure`（`setInsecure()`），
和天气、云音乐同一套（main.cpp:1556 / 2928）。

**必须流式写盘**，不能 `http.getString()`：

```cpp
WiFiClient* stream = http.getStreamPtr();
while (http.connected() && remaining) {
    int n = stream->readBytes(buf, sizeof(buf));   // buf 建议 2~4KB
    partFile.write(buf, n);
}
```

---

## 7. Favorite / play history 挂哪些播放器事件

| 需要记录 | 现有钩子 | 位置 |
|---|---|---|
| 播放开始 | `playerCorePlaySdFile()` | main.cpp:1377 |
| 播放完成 | `s_eofCount` 递增（`PlayerSnapshot::eofCount`） | main.cpp:199 / 5112 |
| 播放进度（判 80%） | `PlayerSnapshot::positionSeconds` / `durationSeconds` | 已有 |
| 跳过 | UI 的 next/prev + 当前 position | hifi_ui.cpp 的 transport action |
| Favorite | **需要新增 UI**（方案 Phase 6） | — |

`eofCount` 已经被 `HifiUi::refresh()` 用来做本地曲目自动续播（hifi_ui.cpp:6950 附近），
**那里就是"一首播完了"最可靠的判据**，play_count 应该挂同一处。

---

## 8. 推荐新增文件

基本采用方案 §25，但按上面的结论调整：

```
src/library/
├─ library_store.h/.cpp     tracks.idx + events.log 读写、compact
├─ music_library.h/.cpp     查询接口（all / favorites / recent / discovery）
├─ library_cleaner.h/.cpp   容量检测 + delete_score + dry-run
└─ play_history.h/.cpp      播放事件 → events.log

src/discovery/
├─ music_provider.h         MusicProvider / RemoteTrack / DiscoveryRequest
├─ jamendo_provider.h/.cpp
├─ download_manager.h/.cpp  .part 流式下载 + rename
└─ daily_discovery.h/.cpp   状态机 + 每日调度
```

**不新增 `cover_cache`**：封面沿用现有 ID3 内嵌 + `.lrc` 式旁挂的思路即可，
第一版把 Jamendo 封面存成 `covers/album_<id>.jpg` 并在 `TrackRecord` 里记路径。

---

## 9. 十二个最大风险

| # | 风险 | 说明 / 缓解 |
|---|---|---|
| **R1** | **DMA 可用内部内存只剩约 7KB**（`dma_free=7155`，最大块 6656） | 🔴 最高。下载时 WiFi 收包 + SD 写入 + I2S 播放三者同时抢 DMA。**只能实测**，见下方更正 |
| **R2** | `kMaxLocalTracks = 300` 硬上限 | Phase 1 第一件事就是扩容，否则后面全是空谈 |
| **R3** | **USB MSC 模式会让索引失效** | 🔴 方案完全没想到。用户在电脑上删/加歌后必须对账。见 §10 |
| **R4** | 中日文配额几乎必然填不满 | 已确认接受。但"回填"是常态不是异常，必须一等公民对待 |
| **R5** | `String` 装整个 HTTP 响应 | 靠限制单次 20 条控制。`String` 走普通 heap，在 `SPIRAM_USE_MALLOC` 下会回落到 PSRAM，压力小于最初估计 |
| **R6** | 下载写 SD 与播放读 SD 争用 | SD_MMC 是 1-bit 模式，带宽本就不宽。必须实测有没有爆音 |
| **R7** | 断电留下 `.part` | 开机扫 `/music/tmp/*.part` 直接删。方案已覆盖 |
| **R8** | 封面只认 JPEG（无 PNG 解码器） | Jamendo 请求 JPEG；拿到 PNG 直接跳过封面，不阻塞入库 |
| **R9** | `onWifiNetworkReady` 只触发一次 | 见 §3，必须同时挂 1 秒 tick |
| **R10** | 时间无效时写错日期 | 用现成的 `s_f_rtc` 判据 |
| **R11** | PSRAM 被曲库吃掉影响 LVGL | LVGL 96KB 池在 PSRAM。2000 首占 644KB，余量够；5000 首要重新评估 |
| **R12** | Jamendo client_id 泄漏 | 存 NVS，**不进仓库**。参照 `platformio_override.ini` 已有的凭据隔离做法 |

---

### 9.1 ⚠️ R1 的更正（2026-09-07 当天）

初版 R1 写的是「内部 SRAM 只剩 23KB，而 TLS 会话要 ~40KB」。**这条是错的**，已作废。

查 `sdkconfig.h` 实际配置：

```
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC     1        <- mbedTLS 已经从 PSRAM 分配
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN     16384    <- 这两个 16KB 大缓冲在 PSRAM
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN    16348
CONFIG_SPIRAM_USE_MALLOC              1        <- malloc 可回落 PSRAM
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP  1        <- WiFi/lwIP 缓冲也走 PSRAM
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL   16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 32768
```

TLS 的大缓冲本来就在 PSRAM —— 这正是现有天气和云音乐 HTTPS 能跑起来的原因。
写初版时是按「TLS 默认吃内部 SRAM」的通常情况推的，**没查本项目的实际配置**。

**但瓶颈只是换了位置，没有消失：**

- `SPIRAM_MALLOC_ALWAYSINTERNAL = 16384` —— 小于 16KB 的分配**仍然优先走内部**，
  所以不是"TLS 全在 PSRAM"
- `internal_free = 14915` 已经低于 `RESERVE_INTERNAL = 32768`，malloc 现在基本
  把能推的都推去 PSRAM 了
- **真正紧张的是 DMA 可用内存：`dma_free = 7155`，最大块 6656。**
  这个不能放 PSRAM —— WiFi 收发描述符、SD_MMC 传输、I2S 播放都要它，
  而下载场景恰好三者叠加

**缓解手段（评估过，本次未采用）：**

把 `kRowsPerBuffer` 从 16 降到 8 可以释放 10240 B 的 DMA 内部内存（缓冲从
2×10240 变成 2×5120），几乎是当前 DMA 余量的三倍。而且脏区域治理之后
flush/s 只有 16~21，翻倍到 30~40 在 4.6% CPU 的基数上无感 —— 这个取舍在
9/5 之前不成立，现在成立了。

⚠️ **但 2026-09-07 决定不做**：保留显示侧的余量。如果 Phase 4 实测证明 DMA
确实不够，再回来考虑这一条。

⚠️ 另外要记住：`kRowsPerBuffer` **不能在运行时动态调整**。它是 `constexpr`，
缓冲在 `begin()` 里一次性分配并注册给 LVGL；而且显示链路是**异步 DMA**，
`free()` 一块 DMA 可能正在读的缓冲会花屏甚至崩。想"下载时临时降低"必须先
等所有在途传输完成、停掉刷新、换缓冲再恢复 —— 为省 10KB 引入这套状态机不划算。

---

## 10. 方案漏掉的：USB MSC 对账

本项目可以把 SD 挂给电脑当 U 盘（`Page::UsbStorage`）。用户在电脑上删歌、加歌、
改名之后，`tracks.idx` 就与实际文件不一致。

**必须新增一步对账**，时机二选一（建议都做）：

1. 开机扫描时：记录指向的文件不存在 → 标记 `MISSING`；发现未索引的新 `.mp3` → 补录
2. 退出 U 盘模式重启后：同上（U 盘模式退出本来就会重启，所以 1 已覆盖）

`usbStorageBlocksSdAppAccess()`（main.cpp:2178 等处已在用）是现成的互斥判据，
扫描/下载都必须尊重它。

---

## 11. 另一处修正

方案说「USB DAC 模式可下载」—— **做不到**。本项目在声卡模式下压根不启动 WiFi
（`setupLvglRuntime()` 的声卡分支直接 return，跳过 WiFi/播放器/音乐扫描）。
声卡模式下整个 Daily Discovery 都不存在。

---

## 12. Phase 1 编码清单

> 目标：本地 Library Index + Play History。**不联网、不下载、不删除、不改用户文件名**。

- [ ] `kMaxLocalTracks` 300 → **2000**，并实测扫描耗时与 PSRAM 占用
- [ ] 定义 `TrackRecord`（方案 §8 的字段）与 `tracks.idx` 定长格式
- [ ] `library_store`：读/写/追加/compact；开机加载到 PSRAM
- [ ] 扫描时与 `tracks.idx` 对账（新增/缺失），**不移动不改名用户已有文件**
- [ ] `stable local_id`：建议 `hash(path)`，改名即视为新曲（第一版可接受）
- [ ] `play_history`：挂 `playerCorePlaySdFile()`（开始）、`s_eofCount`（完成）、
      transport next/prev（跳过），写 `events.log`
- [ ] 查询接口：`all` / `favorites` / `recent` / `discovery`
- [ ] `favorite` / `keep` 字段与读写 API（UI 留到 Phase 6）
- [ ] 实测 300 / 1000 / 2000 首三档的**启动耗时**与 **PSRAM/内部 SRAM 峰值**
- [ ] 写 dev log，列真机测试项

**Phase 1 完成后停止，不进入 Phase 2。**
