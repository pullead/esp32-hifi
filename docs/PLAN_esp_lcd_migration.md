# 方案：显示链路迁移到官方 esp_lcd（异步 DMA）

> 状态：**方案，未实施**。2026-09-03 调研完成。
> 目标：消除显示链路的同步阻塞传输，这是当前流畅度的最大瓶颈。

## 0. 一句话结论

**用 `esp_lcd` 直接替换 `Arduino_GFX`，但不要引入 `esp_lvgl_port`。**
性能收益 100% 来自 esp_lcd 的异步 DMA；esp_lvgl_port 只提供任务/加锁封装，
却会和本项目 vendored 的 LVGL 冲突（见 §2）。

## 1. 现状与瓶颈（已实测确认）

当前链路：`LVGL → Arduino_GFX(Arduino_HWSPI) → Arduino SPIClass → 寄存器轮询`

`waveshare_lvgl_port.cpp` 的 `flush()`：
```cpp
gfx->draw16bitRGBBitmap(...);   // 同步阻塞，CPU 逐 64 字节填 FIFO + 忙等
lv_disp_flush_ready(driver);    // 传输完才通知 LVGL
```
LVGL 的双缓冲（"渲染下一块 / 传输上一块"并行）**完全没吃到**。

实测基线（2026-09-03，25 秒采样，稳定）：

| 指标 | 数值 |
|---|---|
| CPU 占用（`lv_timer_handler`） | **35.5%** |
| 单次最长阻塞 | **20.0ms** |
| 主循环迭代/秒 | 440 |
| flush/秒 | 165 |

> 注：更早的 44.4% / 27.6ms / 240 是旧固件（`376a176-dirty`）的数据，
> 08-07 提交（LCD SPI 频率修复 + 任务绑核 Core 0）已经把它改善到上面这组。
> **做对比实验时用 35.5% / 20.0ms 这一组作为基线。**

## 2. 为什么不用 esp_lvgl_port

调研结论（查过官方组件页）：`esp_lvgl_port` 本身**同时支持 LVGL 8 和 9**
（通过 `esp_lvgl_port_compatibility.h` 做类型兼容），所以"必须先升 LVGL 9"
这个顾虑**不成立**。

但对本项目仍然不建议引入，原因是依赖冲突：

- 本项目把 **LVGL 8.3.11 vendored 在 `lib/lvgl/`**（gitignored，各机器自行拉取），
  并且用的是定制的 `src/lv_conf.h`——里面有关键的项目专属设置，比如
  `LV_ATTRIBUTE_LARGE_RAM_ARRAY = EXT_RAM_BSS_ATTR`（把 96KB 内存池搬进 PSRAM，
  这是之前解决内部 RAM 危机的核心手段）、定制 CJK 字体等。
- `esp_lvgl_port` 在自己的 `idf_component.yml` 里声明 `lvgl/lvgl` 依赖，会从组件
  仓库再拉一份 LVGL → **两份 LVGL 同时存在 → 符号重复 → 链接失败**。
- 绕过办法（override 依赖版本 / 强行让它复用 vendored 版本）存在但脆弱，而且
  本项目已经有一个能正常工作、结构清晰的手写集成层（`WaveshareLvglPort`）。

**收益/风险不成比例**：esp_lvgl_port 提供的是 LVGL 任务管理、互斥锁、tick 处理，
这些本项目都已经有了；它**不提供**任何额外的传输性能。

## 3. 已验证的技术前提（本地 IDF 5.4.2 实查，非推测）

| 项 | 结论 |
|---|---|
| ST7789 驱动 | ✅ 内置：`esp_lcd/src/esp_lcd_panel_st7789.c`，无需第三方组件 |
| 异步完成回调 | ✅ `esp_lcd_panel_io_register_event_callbacks()` + `on_color_trans_done` |
| 回调也可在 io_config 里直接给 | ✅ `esp_lcd_panel_io_spi_config_t.on_color_trans_done` |
| 传输队列 | ✅ `trans_queue_depth` 字段 |
| 面板偏移（170×320 需要 35px） | ✅ `esp_lcd_panel_set_gap(panel, x_gap, y_gap)` |
| 反色（本面板需 INVON） | ✅ `esp_lcd_panel_invert_color(panel, true)` |
| 横屏（swap/mirror） | ✅ `esp_lcd_panel_swap_xy()` / `esp_lcd_panel_mirror()` |

SPI 总线占用：SD 走 SDMMC（39/40/41）、触摸走 I2C（47/48），
**SPI 总线上只有 LCD**，不存在共享冲突。

参考实现：微雪出厂例程就是用 esp_lcd 写的
（`waveshare-ESP32-S3-LCD-1.9/02_Example/ESP-IDF/08_FactoryProgram/main/main.c`），
仓库里还留着 `src/waveshare/esp_lcd_sh8601.c/.h`（当前未被使用的模板残留，
`docs/LVGL_MIGRATION.md` 已注明面板实际是 ST7789 而非 SH8601）。

## 4. 改动范围

**只动 `src/ui/waveshare_lvgl_port.cpp` / `.h` 一个模块。**
`hifi_ui.cpp`（7000 行 UI 逻辑）、`player_service`、`main.cpp` 全部不用改——
LVGL 的 `flush_cb` 接口不变，变的只是它内部怎么把像素送出去。

### 4.1 初始化（替换 `initPanel()`）

```
spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO)
  ↓
esp_lcd_new_panel_io_spi(host, &io_config, &io_handle)
    io_config.on_color_trans_done = <回调>   ← 关键
    io_config.trans_queue_depth   = 10
    io_config.pclk_hz             = 100MHz  （沿用当前 kTftSpiHz）
    io_config.lcd_cmd_bits/param_bits = 8
  ↓
esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle)
    panel_config.reset_gpio_num = 9
    panel_config.bits_per_pixel = 16
    panel_config.rgb_ele_order  = <见 §6 待验证项>
  ↓
esp_lcd_panel_reset() → esp_lcd_panel_init()
esp_lcd_panel_invert_color(panel, true)      ← 本面板需要 INVON
esp_lcd_panel_swap_xy(panel, true)           ← 横屏
esp_lcd_panel_mirror(panel, ?, ?)            ← 见 §6
esp_lcd_panel_set_gap(panel, ?, ?)           ← 见 §6（35px 偏移）
esp_lcd_panel_disp_on_off(panel, true)
```

背光（GPIO14，低电平点亮，当前用 LEDC PWM 调亮度）**保持现有实现不动**，
和 esp_lcd 无关。

### 4.2 flush 改成异步（核心收益点）

```cpp
// 现在（同步阻塞）
void flush(drv, area, colorMap) {
    gfx->draw16bitRGBBitmap(...);   // 阻塞直到传完
    lv_disp_flush_ready(drv);
}

// 改后（异步）
void flush(drv, area, colorMap) {
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2+1, y2+1, colorMap);
    // 立刻返回，不调 lv_disp_flush_ready
}

// DMA 传输完成中断回调里才通知 LVGL
static bool onColorTransDone(io, edata, user_ctx) {
    lv_disp_flush_ready((lv_disp_drv_t*)user_ctx);
    return false;
}
```

**这一步是全部性能收益的来源**：CPU 提交完传输就去渲染下一块 buffer，
真正吃到 LVGL 双缓冲的并行度。

### 4.3 保持不变

- 触摸读取（CST816 走 I2C）、手势识别逻辑
- 绘图缓冲仍在 PSRAM（**但见 §6 的 DMA 可达性验证**）
- `kRowsPerBuffer = 80`
- `[PERF]` 统计代码（正好用来量化本次改动）

## 5. 实施步骤（每步都可独立验证/回滚）

1. **建分支**，别在主线上做。
2. 先只做初始化替换，flush 仍保持同步（`esp_lcd_panel_draw_bitmap` 后立刻
   `lv_disp_flush_ready`）。目标：**画面正常、方向正确、颜色正确、无偏移**。
   这一步把"驱动能不能跑通"和"异步对不对"两个问题分开。
3. 确认显示正常后，再改成异步（去掉 flush 里的 `flush_ready`，挂回调）。
4. 抓 25 秒 `[PERF]` 和 §1 基线对比。
5. 长时间跑（播放本地音乐 + 电台 + 在线音乐、翻页、滑动列表），确认无花屏、
   无撕裂、无重启。
6. 稳定后再考虑删掉 `Arduino_GFX` 依赖和 `src/waveshare/esp_lcd_sh8601.*` 残留。

## 6. 风险与待验证项（重点）

### 高风险：PSRAM 缓冲的 DMA 可达性
当前两个绘图缓冲都在 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`）。
Arduino_HWSPI 是 CPU 读取，所以没问题；**换成真 DMA 后，SPI DMA 必须能直接读
PSRAM**。ESP32-S3 支持，但涉及 cache 写回一致性。

→ 若出现花屏/错位，优先试：把缓冲改回内部 RAM（`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`），
或减小 `kRowsPerBuffer` 以降低内部 RAM 占用。**注意内部 RAM 极紧**（见下）。

### 高风险：内部 RAM 预算
今天（2026-09-03）刚踩过：把缓存从 16KB/32KB 加到 32KB/64KB（多占 48KB 内部
SRAM），固件能编能烧，但**板子刷完从 USB 彻底消失，需物理干预恢复**。
`esp_wifi_init()` 约需 35KB，一直在和音频 I2S DMA 抢内部 RAM。

→ 任何会增加内部 RAM 占用的改动都要**小步、单项、每步实测能开机**。

### 中风险：偏移 / 方向 / 镜像
面板物理是 170×320 竖屏，逻辑用 320×170 横屏，竖屏下偏移是 (35, 0)。
`swap_xy` 之后 gap 的轴向映射会变，**大概率要写成 `set_gap(panel, 0, 35)`，
但必须实测确认**（历史上这个偏移问题反复踩过坑）。
同理 `mirror()` 的两个参数、`rgb_ele_order` 都需要实测。

→ 这就是为什么 §5 要把"驱动跑通"单独作为一步。

### 中风险：SPI host 选择
`src/common.h:100` 有一条注释："Waveshare 把面板接到 SPI3_HOST（Arduino HSPI），
用 FSPI/SPI2 会点亮背光但面板收不到数据"——但那条是**旧的 tftLib 路径**的经验。
当前 LVGL 路径用的是 `Arduino_HWSPI` 默认的全局 `SPI` 对象。

→ 迁移前先确认当前实际跑在哪个 host 上，用同一个，别想当然。

### 低风险：撕裂
异步之后，LVGL 可能在上一块还在传输时就开始写另一个 buffer。LVGL 的双缓冲
机制本身会处理（`lv_disp_flush_ready` 之前不会复用该 buffer），但要实测确认
没有撕裂。

## 7. 预期收益

主要收益是**把传输时间从 CPU 占用里摘出去**。当前 35.5% CPU 里，相当一部分是
`spiWriteNL()` 的忙等。异步之后这部分变成 DMA 后台搬运，CPU 可以去渲染下一块。

预期：CPU 占用明显下降、单次最长阻塞（当前 20ms）显著缩短、触摸响应更跟手。
`flush/s` 预计变化不大（画的东西没变）。

**具体数字不预测**，用 §1 基线实测对比。

## 8. 回滚

改动集中在单个模块 + 单个分支，`git checkout` 即可。
板子级回滚：`backup/board_backup_20260903/full_16mb.bin`（完整 16MB，
`esptool verify-flash` 逐字节校验过），恢复步骤见该目录 README。

## 9. 另一条更便宜的路（可先做/并行做）

与本方案独立、**完全不碰内存预算和硬件配置**：消除 `refresh()` 里的冗余重绘。

`hifi_ui.cpp` 的 `refresh()` 每 60ms 跑一次，里面约 22 处 `lv_label_set_text()`，
**只有 2 处做了变化检测**。而 LVGL 8.3 的 `lv_label_set_text()` 第一行就是无条件
`lv_obj_invalidate(obj)`（已查源码确认），之后还有 `lv_mem_free` + `lv_mem_alloc`
（在已搬去 PSRAM 的 96KB 池里做，更慢）。

最浪费的是状态栏：不受页面分支保护，**每个页面每 60ms** 都无条件刷新时间、
编解码器、音量百分比、5 个音量条颜色等约 13 个对象，而时间一分钟才变一次。
等于每秒约 16 次把 320×20 整条状态栏标脏重传。

修法：缓存上次值，不变就不调 setter（项目里已有现成范式：`m_homeTitleLastText`）。
直接减少的是"需要通过阻塞 SPI 传输的脏区域面积"，正好作用在同一个瓶颈上。
