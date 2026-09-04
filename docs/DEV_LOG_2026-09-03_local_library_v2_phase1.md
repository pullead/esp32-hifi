# Dev Log — 2026-09-03：本地音乐库 2.0 Phase 1（索引与持久化）

> **状态：Phase 1 核心链路已全部在硬件上验证通过。**
> 索引建立 → 持久化 → 播放事件写入 → 跨重启保留，四环闭合。
> 剩余项（favorite UI 触发、missing 场景、启动耗时）见 §6。
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
| 曲库扩容到 2000 | ✅ 实测 |
| `library_store` 编译 | ✅ |
| 板子带新代码能开机 | ✅ 实测 |
| **索引建立** | ✅ **实测** `tracks=54/2000`，与 SD 上曲目数一致 |
| **重启后读回而非重建** | ✅ **实测** `loaded_at_boot=54` |
| missing 标记 | ✅ 逻辑就位（`missing=0`，尚未构造删歌场景验证） |
| 分配失败退让路径 | ⬜ 未触发过 |
| USB MSC 删歌后标 missing | ⬜ 未构造场景验证 |
| **播放事件写入** | ✅ **实测** `played` 0 → 2 |
| **事件跨重启保留** | ✅ **实测** 重启后仍为 2（走的是 events.log 回放） |
| favorite / keep API | 🟡 已实现，**无 UI 可触发**，未验证 |

### 5.0 实测数据（2026-09-03）

```
[LIB][STATE] tracks=54/2000 loaded_at_boot=54 missing=0 fav=0 played=0              rec=384B idx=750KB psram_free=1824KB
```

**`loaded_at_boot=54` 是这一阶段最关键的一个数**：它区分了"索引真的持久化了"
和"每次开机都重扫一遍"——两种情况下 `tracks` 都是 54，光看总数分不出来。
为此专门加了这个计数器。

几个订正过的数字：

| | 估算 | 实测 |
|---|---|---|
| `sizeof(TrackRecord)` | 322 B（按 LocalTrackItem 推的） | **384 B** |
| 2000 首索引占用 | 644 KB | **750 KB** |
| 剩余 PSRAM | — | 约 1.8 MB |

`TrackRecord` 比 `LocalTrackItem` 大 62 字节（多了 localId / 统计 / flags /
providerTrackId / reserved），所以索引比原估算大 106KB。余量仍然充裕。

⚠️ 观察到 `psram_free` 在 1057~1862 KB 之间波动，说明有大块 PSRAM 在被反复
申请释放（大概率是封面解码）。目前不构成问题，但 Phase 4 加下载缓冲时要留意。

### 5.0b 播放事件实测（2026-09-03）

播一首本地音乐（播完后自动续播了下一首）：

```
播放前： played=0
播放后： played=2
重启后： played=2      <- 关键
```

**"重启后仍是 2"证明的是追加日志的回放路径**，不是内存状态：

1. 上次扫描存盘时索引里写的是 `played=0`，同时 `events.log` 被清空（compact）
2. 播放产生的 2 条事件追加进 `events.log`
3. 重启时 `libraryStoreLoad()` 先读索引（`played=0`），再回放 `events.log` → 变成 2

如果回放没生效，重启后会掉回 0。所以这一个数同时验证了写入和回放两侧。

⚠️ 注意 `evt_eof` **对网络电台也会触发**。这里没有误记，是因为
`s_playingLocalId` 只在 `playerCorePlaySdFile()` 里被设置，电台播放时它是 0。

### 5.1 验证方法（已跑通，留作后续参考）

关键在于**周期性打印**而不是抓开机日志：`[LIB][STATE]` 每 10 秒一行，
随时连串口都能读到。抓开机瞬间的 printf 在这块板上基本不可行（§4）。

判据：
- `tracks=N/2000` 与 SD 上实际曲目数一致 → 扫描正常
- `loaded_at_boot=N`（非 0）→ **索引是读回来的，持久化成功**
- `loaded_at_boot=0` 而 `tracks=N` → 说明每次都在重建，持久化失败

---

## 6. Phase 1 剩余项

- [x] ~~`play_history`：挂播放开始 / 完成 / 跳过，写 `events.log`~~ ✅ 已实测
- [x] ~~查询接口~~ `playerCoreLibraryCount(kind)` / `playerCoreLibraryIndexOf(kind, nth)`，
      kind: 0=全部 1=收藏 2=最近播放 3=自动发现 4=可播放（非 missing）
- [x] ~~`favorite` / `keep` 读写 API~~ 已实现，但**没有 UI 能触发，因此未验证**
      （UI 按计划留到 Phase 6）
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
