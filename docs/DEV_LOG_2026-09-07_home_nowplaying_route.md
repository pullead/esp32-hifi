# Dev Log — 2026-09-07：首页「正在播放」在重启后进错页面

> **状态：已实现，⚠️ 尚未上机验证。** 代码已编译烧录，但没有实际走一遍
> 「播放 → 停止 → 重启 → 点首页正在播放」的验证流程。
>
> 分支 `codex/usb-dac-no-sound-fix-20260906`（顺手修的，和 USB 声卡功能无关）。

---

## 1. 现象

重启后，在**还没播放任何东西**的状态下点击首页的「正在播放」区域，进入的是一个
**早已废弃的旧版通用播放页**，而不是本地音乐 / 电台 / 在线音乐各自的播放页。

## 2. 根因

`HifiUi::onHomeNowPlayingAction()` 的 `else` 分支：

```cpp
if (state.source == PlayerSource::Radio) show(Page::Radio);
else if (state.source == PlayerSource::Sd) show(Page::LocalNowPlaying);
else if (state.source == PlayerSource::CloudMusic) show(Page::CloudNowPlaying);
else show(Page::NowPlaying);   // ← 被抛弃的通用页
```

原注释自己写明了 "falls back to the old generic skeleton, same as before this tap
existed" —— 当初三个专用播放页加进来时，**"什么都没在播"** 这个分支被留给了旧页面。
而**重启后正是这个状态**，所以每次重启后点进去看到的都是它。

不是回归，是一开始就没覆盖到的分支。

## 3. 修复：记住上一次播放过的音源

### 3.1 为什么要新加持久化字段

`s_settings` 里已经有两个"上次"字段，但都不够用：

| 已有字段 | 问题 |
|---|---|
| `lastconnectedhost`（电台） | 只记内容**不记顺序** |
| `lastconnectedfile`（本地） | 同上——两者可能同时非空，无法判断最后一次是哪种 |
| （云音乐） | **完全没有对应字段** |

所以单独存一个 `last_source`（NVS key，值就是 `PlayerSource` 枚举：
0=None 1=Radio 2=Sd 3=Dlna 4=CloudMusic）。

### 3.2 写在哪：`PlayerService::tick()`

**关键点：不能写在各个 `play*()` 方法里。**

云音乐的 `source` 是 `playerCoreReadSnapshot()` 从 `main.cpp` 的
`s_cloudMusicPlaying` 推出来的，**根本不经过 `PlayerService` 的任何 `play*()`
方法**（`playRadioUrl` / `playSdFile` 都不会被云音乐路径调用）。写在那些方法里
会漏掉云音乐这一整类。

`tick()` 里紧跟 `playerCoreReadSnapshot()` 之后是唯一能全覆盖的位置。

`playerCoreSetLastSource()` 内部有"值没变就不写"的判断 —— 这个函数每 60ms 会被
调一次，无脑写会毫无必要地磨损 flash。

### 3.3 路由

```cpp
PlayerSource source = state.source;
if (source == PlayerSource::None) source = playerService.lastSource();

if      (source == PlayerSource::Radio)      show(Page::Radio);
else if (source == PlayerSource::Sd)         show(Page::LocalNowPlaying);
else if (source == PlayerSource::CloudMusic) show(Page::CloudNowPlaying);
else                                          show(Page::Sd);  // 见下
```

**全新设备、从来没播过任何东西**时进本地音乐列表（`Page::Sd` =
`buildLocalMusic()`），让用户直接挑一首 —— 比进一个空的播放页有用。
这一条是实现时自己定的，需求里没提。

## 4. 改动的文件

| 文件 | 改了什么 |
|---|---|
| `src/main.cpp` | `last_source` NVS key + `playerCoreLastSource()` / `playerCoreSetLastSource()`（带缓存与"没变就不写"） |
| `src/ui/player_service.cpp` | `tick()` 里落盘；`PlayerService::lastSource()` |
| `src/ui/player_service.h` | `lastSource()` 声明 |
| `src/ui/hifi_ui.cpp` | `onHomeNowPlayingAction()` 的路由 |

## 5. 待验证

- [ ] 本地音乐：播一首 → 停 → 重启 → 点首页正在播放 → 应进 Local Now Playing
- [ ] 电台：同上 → 应进 Radio
- [ ] 在线音乐：同上 → 应进 Cloud Now Playing
- [ ] 全新状态（NVS 里没有 `last_source`）→ 应进本地音乐列表

## 6. 顺带发现：`Page::NowPlaying` 成了死代码

改完之后 `Page::NowPlaying` **完全不可达** —— 查过，它此前只有
`onHomeNowPlayingAction()` 那一个入口。相关的 `buildMediaPage(false)` /
`refreshMediaPage()` 也随之成为死代码。

**本次没有清理**：一来超出这次修复的范围，二来 `buildMediaPage()` 可能和别的页面
共用（未核实）。要清的话应当先查清依赖。

（这个仓库里已经有过一次类似的坑：`buildUsbStorage()` 后半段是死代码，往那里加
东西界面上完全没反应，见 `DEV_LOG_2026-09-06_usb_dac_no_sound.md` §4.6。
死代码不清理是有真实成本的。）
