# 2026-07-31 Audio Settings / EQ 开发日志

## 基线

- 分支：`codex/audio-settings-on-radio-home`
- 基于：`origin/codex/radio-nowplaying-home-redesign`
- 未恢复 `.gitignore` 中的 `backups/` 规则，保持新分支现状。

## 本次完成

- 设置页新增 `音频` 入口。
- 新增音频设置分层页面：
  - `音频`
  - `解码与输出`
  - `当前输出`
  - `输出策略`
  - `EQ 与音效`
  - `单频段精调`
  - `音效`
  - `DAC / 耳放`
- 音频设置页不使用全局状态栏，改用 24px 音频专用顶栏，给 320x170 主内容留空间。
- EQ 第一版按真实可用范围实现：
  - 低音 / 中音 / 高音：`audio.setTone()`
  - 左右平衡：`audio.setBalance()`
  - 预设：平直、人声、流行、摇滚、低音
- 快速抽屉 EQ 与完整 EQ 页面共用同一份 `AudioToneSettings` 状态。
- 滑杆改为薄轨道，并加入 80ms 应用节流、900ms 延迟保存。
- 持久化沿用 `/settings.json` 现有字段：
  - `toneLP`
  - `toneBP`
  - `toneHP`
  - `balance`
- 首页底部 `解码` 入口从旧占位页改接到新的 `解码与输出` 页面。

## 后端改动

- `src/ui/player_service.h`
  - 新增 `AudioToneSettings`
  - 新增 `toneSettings()` / `setToneSettings()` / `saveToneSettings()`
- `src/ui/player_service.cpp`
  - 将 LVGL UI 与 `main.cpp` 的 tone/balance 桥接成稳定接口。
- `src/main.cpp`
  - 新增 `playerCoreToneSettings()`
  - 新增 `playerCoreSetToneSettings()`
  - 新增 `playerCoreSaveSettings()`
  - `playerCoreSetTone()` 保留为兼容路径，但内部也走统一 tone settings。

## 验证

- `git diff --check` 通过；只有 Windows CRLF 提示。
- 单目标编译通过：
  - `.pio\build\esp32s3\src\ui\hifi_ui.cpp.o`
  - `.pio\build\esp32s3\src\ui\player_service.cpp.o`
  - `.pio\build\esp32s3\src\main.cpp.o`
- `pio run -e esp32s3 -t buildprog -j4` 完整生成固件成功：
  - RAM: 26.6% (`87176 / 327680`)
  - Flash: 80.5% (`5063670 / 6291456`)
  - `firmware.bin`: 5,064,064 bytes
- 默认 `pio run -e esp32s3 -j4` 已经生成 `firmware.elf` / `firmware.bin`，但最后在 ESP-IDF size-report 阶段失败：
  - `esp_idf_size: error: unrecognized arguments: --ng`
  - 这不是源码编译错误，也不是链接错误。
  - 当前 Windows 环境还同时存在 `PATH` / `Path` 两个进程环境变量，部分 PowerShell/.NET API 会报重复键；PlatformIO 的 `.espidf-5.4.2` helper Python 也指向了不存在的 Python312 路径。后续 Windows 构建优先用 `buildprog`。
- 已确认没有残留 PlatformIO / xtensa 编译进程。

## 回家电脑接手建议

- 先在家里电脑完整跑一次 `esp32s3` build，确认链接和固件产物。公司 Windows 电脑建议使用：
  - `.\scripts\build_windows.ps1 -Environment esp32s3 -Jobs 4`
- 上板重点验证：
  - 设置页进入 `音频`
  - `解码与输出` 页面布局是否适配 320x170
  - EQ 四个滑杆是否触控顺手
  - 五个预设是否立刻改变声音
  - Balance 是否左右声道可感知变化
  - 重启后 `toneLP/toneBP/toneHP/balance` 是否保留
- 如果要继续细化 UI，优先改布局尺寸和文案，不要再拆出新的 NVS 设置系统。
