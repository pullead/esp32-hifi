# Dev Log — 2026-09-03：显示链路迁移 esp_lcd（进行中，未完成）

> ⚠️ **已被取代（2026-09-04）——请读 `docs/DEV_LOG_2026-09-04_esp_lcd_done.md`。**
> 本文是迁移中途「屏幕还在花屏」时的交接快照，保留仅供追溯。
> 其中 §1 那张性能对比表（CPU 35.5%→24%、最长阻塞 20.0ms→14ms）**不要引用**——那组数字是在画面错误的状态下测的，与迁移前渲染的内容不一致，不具备可比性。


> **状态：WIP，屏幕当前是花屏。** 分支 `feat/esp-lcd-dma`。
> 方案见 `docs/PLAN_esp_lcd_migration.md`。明天从本文件 §4「明天从这里开始」继续。

## 1. 已经做到哪一步

`Arduino_GFX(Arduino_HWSPI)` → 官方 `esp_lcd` + 异步 DMA，改动集中在
`src/ui/waveshare_lvgl_port.cpp` 一个文件（`hifi_ui.cpp` / `player_service` /
`main.cpp` 全部未动）。

**初始化和异步链路已经跑通**，诊断确认：
```
stage=8  init_err=ESP_OK  flushes=444  trans_done=441
```
`trans_done` 紧跟 `flushes`，说明 DMA 完成回调正常触发、LVGL 双缓冲的并行度
真正吃到了。

**性能提升实测（同一块板、同样 25 秒采样）：**

| 指标 | 迁移前(Arduino_GFX) | 迁移后(esp_lcd 异步) | 变化 |
|---|---|---|---|
| CPU 占用 | 35.5% | **~24%** | ↓32% |
| 单次最长阻塞 | 20.0ms | **~14ms** | ↓30% |
| flush/秒 | 165 | 148 | 基本持平 |

**但画面是花屏**，所以这些数字只能说明"传输链路在高效运转"，不能说明迁移成功。

## 2. 已确认的根因/结论（不用重查）

### 2.1 `pclk_hz=100MHz` 会让 esp_lcd 直接拒绝（已修）
`esp_lcd_new_panel_io_spi()` 返回 `ESP_ERR_INVALID_ARG`，`begin()` 提前返回，
显示驱动根本没建起来（表现为 `flush/s=0`、屏幕不刷新、CPU 0.2%）。
ESP32-S3 的 GPSPI 硬件上限就是 **80MHz**。已改为 80MHz。

**连带纠正一条既有错误结论**：08-07 日志里"LCD SPI 100MHz 实测稳定"是假象。
Arduino 的 `SPIClass` 只按分频取最接近档位、**不做上限校验**，所以那时候实际
一直跑在 80MHz，从没到过 100MHz。换成会严格校验的 esp_lcd 才暴露出来。
→ 100 改回 80 **不损失任何实际速度**。

### 2.2 绘图缓冲在 PSRAM，DMA 不可达（**未修，头号嫌疑**）
```
bufA_psram=1  bufA_dma=0
draw_err=2  last_err=ESP_ERR_NO_MEM
```
这正是方案 §6 预判的头号风险，现已证实。

之前用 Arduino_HWSPI 时无所谓（它是 CPU 逐字节读、自带 bounce buffer）；
换成真 DMA 后，esp_lcd 每次都要把 PSRAM 数据中转到内部 DMA 缓冲，既有额外
开销，又会在内部 RAM 紧张时失败——开机时已经实际失败了 2 次。

**花屏很可能就是这个引起的**（传输失败 / 中转竞争 / cache 一致性）。

### 2.3 已排除
- ~~PSRAM 缓冲导致 `draw_bitmap` 被整体拒绝~~ —— 不是，绝大多数传输是成功的。
- ~~回调没触发~~ —— `trans_done` 正常增长。

## 3. 未验证项（花屏也可能来自这里）

这三个参数是我按"竖屏 170×320、偏移 (35,0)、横屏后轴向交换"**推导**的，
**从未上机验证过**：
```cpp
esp_lcd_panel_swap_xy(s_panel, true);
esp_lcd_panel_mirror(s_panel, true, false);
esp_lcd_panel_set_gap(s_panel, 0, 35);
```
历史上这个 35px 偏移反复踩过坑。

## 4. 明天从这里开始

**关键：先区分花屏是「参数错」还是「DMA 缓冲错」。两者症状不同：**

| 症状 | 指向 |
|---|---|
| 画面结构完整但位置错/方向错/整体偏移/颜色反 | §3 的三个参数 |
| 画面撕裂、随机噪点、条纹、局部错块、时好时坏 | §2.2 的 DMA 缓冲 |

建议顺序：

1. **先看一眼花屏的具体形态**（拍张照最直接），按上表分流。
2. **优先试 §2.2**：把绘图缓冲从 PSRAM 换成内部 DMA 可达内存。
   - 内部 RAM 极紧（今天已因多占 48KB 让板子从 USB 消失过一次，见
     `platformio.ini` 里的记录），**不能直接把 100KB 全搬进去**。
   - 做法：大幅减小 `kRowsPerBuffer`（80 → 10~20），用
     `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` 分配。20 行时两个缓冲约
     25KB，是之前验证过能用的量级。
   - 改完看 `bufA_dma` 是否变成 1、`draw_err` 是否归零。
3. 若缓冲修好后仍花屏，再逐个试 §3 的三个参数（一次只改一个）。
4. 全部正常后：删掉 `[LCDDBG]` 诊断、删掉 Arduino_GFX 依赖和
   `src/waveshare/esp_lcd_sh8601.*` 残留。

## 5. 板子当前状态 / 回退方法

- 板子上烧的是本分支的 WIP 固件，**屏幕花屏**，但系统在跑（音频/WiFi 应该正常）。
- **完整回退**：`backup/board_backup_20260903/full_16mb.bin`
  （完整 16MB，`esptool verify-flash` 逐字节校验过），步骤见该目录 README。
- **只回退固件**：`git checkout codex/ncm-cloud-music-handoff-20260807` 重新编译烧录。

### 烧录/调试注意（今天踩过的）
- 这块板 app 启动时 USB 会重新枚举，**初始化阶段的 printf 抓不到**——带复位
  抓取会断线，不带复位抓取会错过开头。所以诊断信息必须做成**周期性打印**
  （`[LCDDBG]` 就是为此加的）。
- LVGL 卡死时 esptool 可能连不上（app 占着 USB）。恢复方法：**全程按住 BOOT
  键的同时拔插 USB**，插好后再松开。注意"USB Composite Device"存在并**不**
  代表 app 在跑——ROM 的 USB-Serial/JTAG 本身就是复合设备，别用这个判断。
- 读 flash 失败时优先加 `--no-stub`，不是降波特率。

## 6. 一个流程教训

第一版我**没有检查 `esp_lcd_panel_draw_bitmap()` 的返回值**就直接烧录了。
结果一个本该立刻暴露的配置错误（100MHz）变成了静默卡死，多花了两轮烧录和
一次物理干预才定位。以后引入新的外设 API，**返回值检查和诊断要和第一版代码
一起写**，不要等出问题再补。
