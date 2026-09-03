// 本地音乐库 2.0 —— 索引持久化
//
// 职责边界：本模块**只负责 tracks.idx / events.log 的读写与回放**，
// 不扫描 SD、不碰播放器。扫描仍然由 main.cpp 的 scanMusicDir() 做，
// 扫完之后调 libraryStoreReconcile() 把结果与已有索引对账。
//
// ⚠️ 本文件**不能** include common.h：那里定义的是 audio/pref/webSrv 等全局
// 对象的**本体**而非 extern 声明，被设计成只由 main.cpp 一个编译单元包含，
// 第二个 TU 包进来会在链接期出十几个 multiple definition
// （2026-09-05 usb_dac.cpp 已经踩过一次，见 DEV_LOG_2026-09-06 §4.2）。

#pragma once

#include "library_types.h"

#include <stddef.h>

// 索引文件路径。放 /music/library/ 下，和方案 §7 的目录约定一致。
// 注意 scanMusicDir() 是从 SD 根目录递归扫 .mp3 的，这些文件不是 .mp3，
// 不会被误当成曲目。
extern const char* const kLibraryIndexPath;
extern const char* const kLibraryEventsPath;

// 从 SD 读回索引，并把 events.log 回放上去。
//
// records/capacity 由调用方（main.cpp）提供——数组本身归它所有，本模块只填充。
// 返回读到的记录数；文件不存在、magic/version/recordSize 对不上，或读取失败，
// 都返回 0（调用方据此走"全新扫描"路径，代价只是重扫一次 SD）。
uint16_t libraryStoreLoad(TrackRecord* records, uint16_t capacity);

// 把内存中的记录整体写回 tracks.idx，并清空 events.log。
// 先写 .part 再 rename，避免写到一半断电毁掉旧索引。
bool libraryStoreSave(const TrackRecord* records, uint16_t count);

// 追加一条事件。**不重写索引**——这正是分成两个文件的意义所在。
// RTC 未同步时 timestamp 传 0，回放时只更新计数、不更新时间戳。
bool libraryStoreAppendEvent(uint32_t localId, uint8_t type, uint32_t timestamp);

// 把一条事件应用到内存记录上。libraryStoreLoad() 回放日志时用它，
// 运行时产生事件后也应立即调用，这样内存状态和日志始终一致。
void libraryStoreApplyEvent(TrackRecord* records, uint16_t count, const LibraryEvent& event);

// 按 localId 找下标，找不到返回 -1。
int32_t libraryStoreFind(const TrackRecord* records, uint16_t count, uint32_t localId);

// —— 扫描期的对账 API ——
//
// 设计要点：**只用一份数组**。原本想的是"扫描结果"和"已有索引"各一份再合并，
// 但 2000 条 × ~400B 两份就是 1.6MB PSRAM，太浪费。改成就地 upsert：
// 先把已有索引 load 进数组，扫描时逐条 upsert，扫完把没被碰过的标记为 missing。
//
// "有没有被碰过"用一个外部位图记录（2000 位 = 250 字节，放调用方栈上即可），
// 而不是借用 flags 里的位——flags 是要持久化的，借位就得记得在存盘前清掉，
// 容易忘。

// 把一条扫描到的曲目并入索引。
//
//   - 已存在（localId 相同）→ 更新元数据（ID3 可能被改过），
//     但**保留统计与 flags**：那是用户行为的产物，重扫一次 SD 不该清零。
//     同时清掉 missing——文件这次扫到了说明它回来了。
//   - 不存在 → 追加到末尾，importedAt 设为 nowEpoch
//
// seen 位图对应下标会被置位。count 会在追加时递增。
// 返回该曲目在数组中的下标；容量满了返回 -1。
int32_t libraryStoreUpsert(TrackRecord* records, uint16_t* count, uint16_t capacity,
                           const TrackRecord& scanned, uint8_t* seenBits, uint32_t nowEpoch);

// 扫描结束后调用：把 seen 位图里没被置位的记录标记为 kTrackFlagMissing。
//
// **这是 USB MSC 场景的关键一步**——用户在电脑上删了歌之后，索引里会留下指向
// 不存在文件的记录（见 docs/LOCAL_LIBRARY_V2_AUDIT.md §10）。这里选择**打标记
// 而不是删除**，是为了留住 favorite 和播放历史：文件要是再拷回来，统计不至于
// 清零。返回本次新标记为 missing 的条数。
uint16_t libraryStoreFinishScan(TrackRecord* records, uint16_t count, const uint8_t* seenBits);

// seen 位图需要的字节数
inline uint16_t libraryStoreSeenBytes(uint16_t capacity) { return (capacity + 7u) / 8u; }
