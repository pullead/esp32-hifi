# Dev Log – 2026-08-06 晚 ~ 2026-08-07 凌晨（家里电脑）：在线音乐真机 bug 修复 + 显示链路性能优化 + 暂停/恢复状态 bug

> 时间跨度：2026-08-06 约 20:50 起 ~ 2026-08-07 约 01:00（JST），单次连续会话。
> 起点分支：`codex/ncm-cloud-music-handoff-20260806`（公司电脑 8/6 交接分支 HEAD `376a176`）。
> 本文件是这次会话的合并交接日志，唯一权威阅读入口。

## 交接信息

- **交接分支（新）**：`codex/ncm-cloud-music-handoff-20260807`（从本分支 HEAD 分出，todo：push 时创建）
- 开发分支：本次会话全部在 `codex/ncm-cloud-music-investigate` 上进行
- 板子：已烧录本次会话的最终固件版本，当前运行正常，真机验证过（本地音乐/电台/在线音乐播放、翻页、暂停恢复、滑动列表）
- 网关部署：本次没有改动 `services/ncm-gateway`，Render 侧配置沿用 8/6 状态不变

## 今天验证/修复的真机问题（用户实测反馈）

### 1. 语言分类点了没反应
排查了 `onCloudCategoryAction`/`onCloudLanguageAction`/`cloudMusicHotPlaylistsStart`/URL 编码整条链路，代码层面没找到问题；同时复测网关 `esp32-ncm-gateway-hifi` 和上游 `api-enhanced-mkoz` 均恢复 200 正常（此前怀疑的双服务 502 问题目前是好的）。**结论未定论**——没有 `DEVICE_API_KEY` 没法带鉴权测接口，需要用户现场复现描述具体表现，或提供 key 才能继续深挖。

### 2. 列表只有 10 条左右，不能下滑加载更多
确认是**未实现的功能**，不是 bug：`kCloudSearchMaxResults=10`／`kCloudHotPlaylistMax=8`／`kCloudPlaylistTrackMax=20` 等常量都是一次性固定 `limit`，没有任何列表绑定滚动事件触发分页加载；网关侧 `offset`/`has_more` 早就支持，纯粹是固件没接。工作量较大，本次未做，留待后续。

### 3. 进在线音乐 UI 有点卡（已修复）
根因：`playerCoreCloudMusicWakeStart()` 每次进"在线音乐"页面都无条件重新做一次 TLS 健康检查握手，即使服务早就 Ready。改成 5 分钟内跳过重复唤醒（`src/main.cpp` `kCloudReadySkipWindowMs`），窗口远小于 Render 免费层 ~15 分钟休眠阈值，不会引入双冷启动 502 问题。

### 4. 主页"正在播放"点进去是旧版播放页（已修复）
`onHomeNowPlayingAction` 的路由分支漏了 `PlayerSource::CloudMusic`，落进最后的 `else` 跳到废弃的老版通用播放页（也是截图里方框乱码的那个页面，因为那个页面的标签从没适配过 CJK 字体）。已加 `CloudMusic` 分支指向 `Page::CloudNowPlaying`（`hifi_ui.cpp:6657` 附近）。

### 5. 在线播放拖动进度条一卡一卡（已修复）
云音乐进度条本来就设计成只读（CDN 分段续播不稳定），但滑块控件本身默认仍可交互，用户拖动时视觉跟手、下一次 ~100ms 刷新又被拽回真实位置，反复拉扯。给这个 slider 清掉 `LV_OBJ_FLAG_CLICKABLE`（`buildCloudNowPlaying()`），变成真正只读展示。

### 6. 在线音乐播放时经常突然重启 / 封面经常不显示（部分排查，见下方"超频"与"封面缓存"两节）

## Flash / PSRAM / LCD SPI 超频排查（大头，反复实测）

用户要求把 Flash/PSRAM/LCD SPI 都尽量提速，逐项实测，记录结论：

- **Flash QIO 80MHz**：编译报错，`mspi_timing_tuning_configs.h` 静态断言 `"FLASH and PSRAM Mode configuration are not supported"`。根因：ESP32-S3 的 Flash 和 Octal PSRAM 共用同一个 MSPI 核心时钟寄存器；Octal PSRAM 必然跑 DTR 模式 → 强制 `CORE_CLOCK_DIV=2`；IDF 源码里只写了"Flash 120MHz STR + DIV=2"这一条推导路径，**QIO(STR) 80MHz 在这个芯片架构下根本不存在**，不是校准表缺失，是压根没这条代码路径。
- **Flash QIO 120MHz**：SDK 层面逻辑上支持（唯一有效的 STR 高速档位），源码能编译过，但卡在 `esptool.py` 生成镜像那一步：`--flash-freq` 参数只接受 `80m/60m/48m/40m/30m/26m/24m/20m/16m/15m/12m`，**120MHz 在工具链镜像头格式里根本编不出来**，比 SDK 限制更硬。已放弃。
- **Flash QIO 40MHz**：实测烧录验证稳定，最终采用。
- **PSRAM Octal 120MHz**：用户已知情决定保留——这是乐鑫官方 Kconfig 帮助文本标注的 **experimental** 功能，原话：温度比开机时漂移约20摄氏度会导致 PSRAM 访问随机崩溃。这条配置在这次会话开始前就已经在仓库里（`sdkconfig.esp32s3_OTA` 未被本次改动触碰过这部分），只是 platformio.ini 注释一直误写着"40MHz 稳定"，跟实际不符——留意后续如果出现新的随机重启，先怀疑这里。
- **LCD SPI 100MHz**：真正生效的旋钮不是 `settings.h` 的 `TFT_FREQUENCY`（那条路径在 `MWR_LVGL_UI` 打开时被 `setup()` 提前 return 挡住，是死代码），而是 `waveshare_lvgl_port.cpp` 里硬编码的 `kTftSpiHz` 常量——发现这个之前，此前几轮"改 LCD 到 80MHz"其实从未真正生效过。修正后先测 80MHz、再测 100MHz，均实测稳定（真机长时间播放+页面切换+手势操作，全程 0 次崩溃/重启），用户决定保留 100MHz。

**最终稳定基线**：ESP32-S3 240MHz / Flash QIO 40MHz / PSRAM Octal 120MHz(experimental，用户已知情) / LCD SPI 100MHz。

## 显示链路架构分析 + 优化

只读分析先行，用户确认后再动手，遵循"先分析、小改动高收益、不牺牲稳定性"的原则：

- LVGL 是标准 **PARTIAL 双缓冲**模式（未开 `full_refresh`/`direct_mode`）。
- **关键发现：这条链路完全没有用 DMA**。走的是 Arduino_GFX（`Arduino_HWSPI`）+ Arduino `SPIClass`，一路跟到底层 `spiWriteNL()`（`esp32-hal-spi.c`）是纯寄存器轮询实现：每 64 字节填一次 FIFO、`cmd.usr=1` 启动、`while(cmd.usr)` 原地忙等，全程同步阻塞，没有 `spi_device_queue_trans`、没有 DMA descriptor、没有完成回调。`flush()` 同步等传输完才调 `lv_disp_flush_ready()`，双缓冲的"渲染下一帧/发送上一帧"并行完全没吃到。
- 实测串口 `[PERF]` 数据（自建的每秒 flush 次数 + `lv_timer_handler()` 累计耗时统计，见下）：静止主页 CPU 占用 ~30-37%，单次最长阻塞可达 20-30ms（页面切换时观测到过 600ms+ 的极端尖峰）。
- 架构级修复方案（换 `esp_lcd_panel_io_spi` + DMA 异步 + 完成中断回调）评估过工作量：约 2-4 个工作日，本次未做，只做了低风险部分：
  - **Draw buffer 20 → 40 → 80 行**（PSRAM 分配，`kRowsPerBuffer`），减少 flush() 调用次数，PSRAM 占用增加到约 100KB（充裕，2.5MB+ 剩余），内部 RAM 不受影响。
  - **16 个未绑核的后台任务全部 `xTaskCreate` → `xTaskCreatePinnedToCore(..., 0)`**，跟音频解码任务一起放 Core 0，让出 Core 1 给 LVGL/loop()（涉及 `cloudMusicControllerTask`/`cloudThumbSyncTask`/`weatherTask`/`wifiScanTask` 等，具体名单见 `src/main.cpp` diff）。
  - 加了临时串口性能统计（`waveshare_lvgl_port.cpp` `tick()` 里的 `[PERF]` 打印，每秒一行：flush 次数、`lv_timer_handler` 调用次数与累计耗时占比、单次最长耗时）——**LVGL 自带的屏幕角标 `LV_USE_PERF_MONITOR` 在这个项目的接入方式下不准**（FPS 报的是刷新周期理论倒数，CPU 一直卡 0% 是空闲回调没接上），已确认无用并关闭，改看串口日志。
- 资源加载/缓存复查（只读，未发现需要动的地方）：
  - UI 侧图片清一色 `LV_IMG_CF_TRUE_COLOR`（RGB565 raw），没有运行时解码压缩格式；且**项目里根本没有内置图标/背景图片文件**——图标都是 LVGL 内建符号字体画的，背景是程序化图形，没有"可转 RGB565"的对象。
  - IRAM_ATTR：查了但不建议加，当前瓶颈是 SPI 同步阻塞轮询等待（I/O bound），不是取指缓存 miss（compute/cache bound），没有证据支持这里有收益。
  - PSRAM 缓存：歌单/搜索结果早就在 PSRAM（此前内存危机修复时迁移过）；封面解码结果**没有缓存**，同一首歌来回切会重复 JPEG 解码——这个找到并修了，见下节。

## 云音乐封面重复解码 → 加了 LRU 缓存

`clearCoverArt()` 每次切歌/切源都会 `free()` 掉当前解码结果，JPEG 原文件本身在 SD 有缓存不重复下载，但**解码这一步没缓存**，来回切同一首歌会重复解码。新增 4 槽位 PSRAM LRU 缓存（`m_cloudCoverCache`，`hifi_ui.h`/`hifi_ui.cpp` 的 `cloudCoverCacheLookup`/`cloudCoverCacheInsert`）：跟当前显示中的 `m_coverArtPixels` **完全独立、各自持有内存**（命中缓存是整块 memcpy 一份新的，不共享指针），避免任何一边因为 `clearCoverArt()` 而悬空。收益是次要优化，不是当前瓶颈大头，但风险低、实现简单，已实测跑通。

## 滑动列表跟手度差 + 误触成点击（已修复）

- `m_touchDriver.scroll_limit`：16 → 8（LVGL 默认是10）。这个值是"手指移动超过多少像素才判定为滑动而非点击"的阈值，16 比默认值还大，轻/快速的一划手指还没挪够就抬起，容易被误判成点击——正好对应用户反馈的"明明只是滑动却触发了点击"。
- `LV_INDEV_DEF_READ_PERIOD`：30ms → 15ms，触摸采样频率翻倍（I2C 读取本身很便宜，加倍轮询开销可接受）。注意：这个只能缓解，治不了根——真正的卡顿大头还是上面说的 SPI 同步阻塞（偶发 20-80ms 单次卡顿期间，触摸采样也会被一起卡住）。

## 暂停/恢复后频谱卡住约一分钟（本次会话最大的真实 bug，已修复）

**现象**：本地音乐/电台/在线音乐播放中暂停再恢复，点阵频谱不动，大约一分钟后才自己恢复。

**排查过程**：加了临时诊断日志（`[PAUSEDBG]`）追踪 `audio.isRunning()`/频谱数据/播放位置的时间线，第一版诊断代码本身写错了触发条件（自己的 bug），修正后没来得及等到完整数据，但已经从两次 toggle 记录里发现 `audio.isRunning()` 在按键后立刻正确变为 1——顺着这条线直接在源码里定位到真正问题。

**根因**：`Audio::pauseResume()`（`ESP32-audioI2S` 库）的返回值语义是 `true=现在暂停 / false=现在恢复播放`，但项目自己的封装函数 `audioPauseResumeAndUpdateState()`（`src/main.cpp`）写成了：

```cpp
if (accepted) s_f_pauseResume = !audio.isRunning();
```

**只有"刚暂停"（`accepted` 为 true）时才会更新 `s_f_pauseResume`，"刚恢复播放"（`accepted` 为 false）时这一行整个被跳过**，导致 `s_f_pauseResume` 卡在暂停状态不会被改回来，即使 `audio.isRunning()` 已经立刻正确变为 true。而 `playerCoreReadSnapshot()` 判断 `transport` 时，`s_f_pauseResume` 的检查排在 `audio.isRunning()` 之前，于是即使音频已经在正常解码、频谱也在正常计算，UI 读到的状态一直还是 `Paused`，频谱动画因为"判断没在播放"就不画。不是频谱计算本身卡住，是这个状态标志位在恢复播放时没跟着同步——本地/电台/在线三种源都会中招是因为这段代码是三者共用的。

修复（`main.cpp`）：改成每次切换后无条件用 `audio.isRunning()` 同步，不再依赖那个有歧义的返回值：

```cpp
static bool audioPauseResumeAndUpdateState() {
    const bool accepted = audio.pauseResume();
    s_f_pauseResume = !audio.isRunning();
    return accepted;
}
```

用户实测确认已解决。

## 已知边界 / 下一步建议

1. **语言分类点击无反应**：未定论，需要更多现场信息（具体表现、或 `DEVICE_API_KEY`）才能继续。
2. **列表分页/下拉加载更多**：确认未实现，四个云音乐列表都是固定 `limit` 一次性拉取，网关侧 `offset` 已支持，工作量中等。
3. **显示链路 DMA 异步改造**：架构级优化，评估约 2-4 个工作日，收益最大但本次未做（只做了 buffer 加大 + 任务绑核这两项低风险部分）。
4. **PSRAM Octal 120MHz 的稳定性**：官方标注 experimental，用户已知情保留，需要持续观察机身发热后是否出现随机重启（这条是本次超频排查之外、本来就已经在仓库里的既有配置，不是今天新引入的）。
5. 本次加的临时串口 `[PERF]` 统计代码保留在 `waveshare_lvgl_port.cpp` 里（成本很低，默认就是每秒一行打印），后续需要时可以直接用，不需要重新插桩。
