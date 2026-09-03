# Dev Log — 2026-09-03：本地音乐库 2.0 Phase 1（索引与持久化）

> **状态：编译通过，硬件验证不完整。** 曲库扩容那部分有实测数据；
> `library_store` 模块**从未在板子上确认过它真的建立/读回了索引**。
> 详见 §5。
>
> 分支：`codex/local-library-v2-20260903`
> 上游：`codex/usb-dac-no-sound-fix-20260906`

---

## 0. ⚠️ 先说日期

**今天的真实日期是 2026-09-03**（系统时钟与所有 git 提交时间一致）。

但本仓库里已有的分支名和文档名用的是 **09-04 / 09-05 / 09-06 / 09-07**，
全部**超前于真实时钟**：

| 名称 | 标注日期 | 实际创建日期 |
|---|---|---|
| `codex/esp-lcd-dma-handoff-20260904` | 09-04 | 2026-09-03 |
| `codex/usb-audio-dac-20260905` | 09-05 | 2026-09-03 |
| `codex/usb-dac-no-sound-fix-20260906` | 09-06 | 2026-09-03（家里那台机器） |
| `DEV_LOG_2026-09-07_home_nowplaying_route.md` | 09-07 | 2026-09-03 |

成因：按会话推进顺序递增地写日期，没有核对系统时钟。

**本分支起改用真实日期。**旧的不去重命名——改名会让所有交叉引用失效，
代价大于收益。以后按 `git log --date` 为准，不要信文件名。

---

## 1. 本轮做了什么

对应 `docs/LOCAL_LIBRARY_V2_AUDIT.md` §12 的 Phase 1 清单，完成了前三项。

### 1.1 曲库上限 300 → 2000 ✅ 有实测

`kMaxLocalTracks` 从 300 提到 2000。这是审计里的 **R2**——方案假设 1000~5000 首，
而现有代码扫到 300 就停。

`LocalTrackItem` 约 322 B、`ps_calloc` 到 PSRAM：

| 规模 | PSRAM |
|---|---|
| 300（原） | 97 KB |
| **2000（现）** | **644 KB** |
| 5000 | 1.6 MB ← 对 2.58MB 的余量太紧，不取 |

**实测**（`after_lvgl_init_and_home_screen`）：

```
psram_free     2588680 -> 2024960     少 564KB，正合 (2000-300)×322  ✅ 分配成功
internal_free    22639 ->   14915
dma_free         15595 ->    7155
```

顺带修了一个**静默失败**：原来是 `if (s_localTracks) scanMusicDir(...)`，
`ps_calloc` 失败会直接跳过整个扫描 —— 用户看到"一首歌都没有"，日志里却什么
都没有。扩到 644KB 后这个失败路径概率明显上升，现在会打日志并**逐级减半退让**
（2000 → 1000 → 500 → 256 → 128），并新增 `s_localTrackCapacity` 记录实际拿到
的容量，扫描的边界检查改用它而不是编译期常量。

### 1.2 新模块 `src/library/` ⚠️ 未验证

| 文件 | 职责 |
|---|---|
| `library_types.h` | `TrackRecord` / `LibraryEvent` / 标志位 / 文件头 |
| `library_store.h/.cpp` | 加载、存盘、追加事件、扫描期对账 |

### 1.3 曲库状态周期打印 ⚠️ 编译通过但没烧进去

每 10 秒打一行 `[LIB][STATE] tracks=.. missing=.. fav=.. played=..`。
加它的原因见 §4。

---

## 2. 设计决策与理由

### 2.1 `TrackRecord` 刻意与 `LocalTrackItem` 前几个字段同名同序

`main.cpp` 里有 **20 多处**直接写 `s_localTracks[i].path` / `.title` / `.hasArt`。
让新结构保持同名同序，那些代码**一行都不用改** —— 实际只动了 3 处：

1. 数组类型 `LocalTrackItem*` → `TrackRecord*`
2. `parseId3Tags()` 的参数类型
3. `playerCoreLocalTrack()` 改成逐字段拷贝（对外 API 仍给 `LocalTrackItem`，
   UI 侧零改动）

### 2.2 两个文件：`tracks.idx` + `events.log`

不用 SQLite 的理由见审计 §4。这里说为什么还要拆成两个文件：

800KB 的索引**每播完一首就全量重写**，既慢又磨损 SD，而且**重写途中断电会毁掉
整个索引**。追加日志最多丢最后一条。开机时把日志回放到内存，再整体落盘一次并
清空日志（compact）。

存盘用 **`.part` + rename** —— 和下载用 `.part` 完全同一个理由。

### 2.3 对账用「就地 upsert + seen 位图」，不是两份数组

最初的设计是"扫描结果"和"已有索引"各一份再合并，但 2000 条 × ~400B **两份就是
1.6MB PSRAM**，太浪费。改成：

1. 先 `libraryStoreLoad()` 把已有索引读进数组（顺带回放事件日志）
2. 扫描时逐条 `libraryStoreUpsert()`
3. 扫完 `libraryStoreFinishScan()` 把没碰过的标 `missing`

"有没有被碰过"用外部位图（2000 位 = 250 字节，放扫描任务栈上）。
**没有借用 `flags` 里的位** —— flags 要持久化，借位就得记得存盘前清掉，容易忘。

### 2.4 USB MSC 场景（审计 R3）已处理

索引里有、SD 上找不到的记录 → 打 `kTrackFlagMissing`，**不删除**。

理由：用户在 U 盘模式下用电脑删歌之后，直接抹掉记录会连带丢掉 favorite 和播放
历史。打标记则保留了这些；文件要是再拷回来，`upsert` 会自动清掉 missing。

### 2.5 RTC 未同步时不写时间戳

`lastPlayedAt` 一旦被写成 1970 年，淘汰算法会把这首歌判成"最久没听"——**正好
判反**。所以 `s_f_rtc` 为假时 `nowEpoch` 传 0，`applyEvent`/`upsert` 只累加计数、
不动时间戳。

### 2.6 格式失配就整个重建

文件头里存 `recordSize`，加载时与 `sizeof(TrackRecord)` 比对。magic / version /
recordSize 任何一项对不上就**整个丢弃重建**。

重建的代价只是重扫一次 SD（几十秒），而带着错位的索引跑下去会产生极难查的怪
问题。所以**加字段优先吃 `reserved[8]`**，实在不够就让它自然失配重建。

---

## 3. 改动的文件

| 文件 | 改了什么 |
|---|---|
| `src/library/library_types.h` | **新增** |
| `src/library/library_store.h` | **新增** |
| `src/library/library_store.cpp` | **新增** |
| `src/main.cpp` | `kMaxLocalTracks` 300→2000；`s_localTracks` 改 `TrackRecord*`；分配退让 + 失败日志；扫描期 upsert/对账/存盘；`[MUSIC][PERF]` 与 `[LIB][STATE]` 埋点 |

`src/CMakeLists.txt` **不用改** —— 它是 `FILE(GLOB_RECURSE ${CMAKE_SOURCE_DIR}/src/*.*)`，
子目录会被自动扫进去（已确认 `library_store.cpp.o` 正常生成）。

---

## 4. 又踩了一次同样的坑：开机瞬间的 printf 抓不到

`library_store` 的日志（`[LIB] loaded/saved ...`）只在扫描结束时打一次，
而扫描发生在开机后约 2 秒。**这块板 app 启动时 USB 会重新枚举**：带复位抓取会
断线，不带复位抓取又错过开头。这一轮试了六七种方式（延时开口、重试循环、
esptool 触发复位后立刻开口、显式拉 DTR/RTS）——大多数时候读到 **0 字符**。

⚠️ **这条教训 esp_lcd 迁移那轮就总结过**（`DEV_LOG_2026-09-04` §5：
"诊断信息必须做成周期性打印"），这次做曲库索引又原样踩了一遍。

补救：在 1 秒 tick 里每 10 秒打一行 `[LIB][STATE]`。**但这一版没烧进去**（见 §5）。

**下次新增任何子系统，诊断从第一版就做成周期性的**，不要只在初始化时打一次。

---

## 5. 验证状态 —— 请重点看这一节

| 项 | 状态 |
|---|---|
| 曲库扩容到 2000 | ✅ **实测**：PSRAM 差值 564KB 正合预期 |
| 分配失败退让路径 | ⬜ 未触发过（分配一直成功） |
| `library_store` 编译 | ✅ `library_store.cpp.o` 正常生成 |
| **板子带新代码能开机** | 🟡 **间接证据**：烧录成功后按 RESET，串口读到 1900 字符正常输出，说明没崩 |
| **索引真的被建立/读回** | ❌ **完全没验证** —— `[LIB]` 那几行一次都没抓到 |
| 重启后统计能保留 | ❌ 未验证 |
| USB MSC 删歌后标 missing | ❌ 未验证 |
| 播放事件写入 | ❌ **还没做**（Phase 1 剩余项） |

**当前板子上跑的固件**：含 `library_store` 集成，**不含**每 10 秒的
`[LIB][STATE]` 打印（那一版编译成功但**烧录失败** ——
`A fatal error occurred: Failed to connect to ESP32-S3: No serial data received`）。

### 5.1 下次接手的第一件事

1. 板子接上，确认串口可用，烧录本分支最新固件
2. 等 10 秒，抓 `[LIB][STATE]` 那行。**预期**：
   `tracks=54/2000 missing=0 fav=0 played=0 rec_size=<约400>`
3. 按 RESET 再看一次 —— 数字应当一致（证明是**读回**而不是每次重建）
4. **更直接的验证**：进 U 盘模式，在电脑上看 `/music/library/tracks.idx`
   是否存在、大小是否 ≈ `32 + 54 × rec_size`

第 4 条是最省事的确认方式，本轮没做只是因为要切模式。

---

## 6. Phase 1 剩余项

- [ ] `play_history`：挂播放开始（`playerCorePlaySdFile`）、完成（`s_eofCount`）、
      跳过（transport next/prev），写 `events.log`
- [ ] 查询接口：`all` / `favorites` / `recent` / `discovery`
- [ ] `favorite` / `keep` 的读写 API（UI 留到 Phase 6）
- [ ] 实测 300 / 1000 / 2000 三档的启动耗时（本机 SD 只有 54 首，
      2000 首只能靠 `per_track` 外推）
- [ ] `[MUSIC][PERF]` 那行的实测数据（同样因为抓不到日志，一直没拿到）

---

## 7. 本轮之外、当天完成并已单独记录的工作

| 内容 | 日志 |
|---|---|
| USB 声卡 Windows 认不到（`wMaxPacketSize` 3→4）**已实测修复** | `DEV_LOG_2026-09-06_usb_dac_no_sound.md` §9 |
| iPhone 音量滑块无效（主机音量真正乘进 PCM）未实测 | 同上 §9.3 |
| 退出声卡模式改善为"只需拔插一次" | 同上 §9.6 |
| 首页「正在播放」重启后进错页面 | `DEV_LOG_2026-09-07_home_nowplaying_route.md` |
| 本地音乐库 2.0 Phase 0 审计 | `LOCAL_LIBRARY_V2_AUDIT.md` |
