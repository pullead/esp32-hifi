# Dev Log — 2026-09-04：显示链路迁移到官方 esp_lcd + 异步 DMA（已完成，真机验证通过）

> **状态：完成。** 屏幕方向、位置、颜色、图片全部正常，触摸正常，零丢帧。
> 本文取代 `docs/DEV_LOG_2026-09-03_esp_lcd_wip.md`（那份是中途花屏状态的交接，
> 里面 §2/§3 的待办已全部解决，§4 的排查路线已走完）。
> 方案原文见 `docs/PLAN_esp_lcd_migration.md`。

---

## 1. 这次做了什么

把 LVGL 的显示输出从 `Arduino_GFX(Arduino_HWSPI)` 换成 ESP-IDF 官方的
`esp_lcd` 组件，并启用**异步 DMA**：

| | 迁移前 | 迁移后 |
|---|---|---|
| 驱动 | Arduino_GFX + Arduino_HWSPI | `esp_lcd_panel_io_spi` + `esp_lcd_new_panel_st7789` |
| flush 语义 | **同步阻塞**，CPU 等到整块传完才返回 | **提交即返回**，DMA 后台搬运 |
| 通知 LVGL | flush() 末尾直接调 `lv_disp_flush_ready()` | SPI 传输完成中断里调 |
| 双缓冲 | 名义双缓冲，实际串行 | 真正并行：渲染下一块 / 传输上一块 |
| 字节序 | Arduino_GFX 逐像素拆 msb/lsb | LVGL 直接产出大端，DMA 零拷贝直传 |

代码改动**集中在一个文件** `src/ui/waveshare_lvgl_port.cpp`，
`hifi_ui.cpp` / `player_service` / `main.cpp` 全部没动。

### 关于 esp_lvgl_port —— 评估后放弃

原计划里写的是 `esp_lcd + esp_lvgl_port`，最后**只用了 esp_lcd**。
原因：`esp_lvgl_port` 通过组件管理器会拉进它自己依赖的一份 LVGL，
和本项目 `lib/lvgl/`（vendored、gitignored 的 8.3.11）冲突，
两份 LVGL 同时存在会直接链接失败。
而 `esp_lvgl_port` 真正提供的东西（创建 LVGL task、加锁、注册 flush 回调）
本项目已经有等价实现，收益不足以抵消换掉整套 LVGL 版本的风险。

---

## 2. 逐文件改动说明

### 2.1 `src/ui/waveshare_lvgl_port.cpp`（主体）

**新增初始化**（`initPanel()`）：

```cpp
spi_bus_config_t busCfg = {};        // sclk=10, mosi=13, miso/quad = -1
busCfg.max_transfer_sz = kWidth * kRowsPerBuffer * sizeof(lv_color_t);
spi_bus_initialize(SPI2_HOST, &busCfg, SPI_DMA_CH_AUTO);

esp_lcd_panel_io_spi_config_t ioCfg = {};   // cs=12, dc=11, spi_mode=0
                                            // pclk_hz=80MHz, trans_queue_depth=10
esp_lcd_new_panel_io_spi(SPI2_HOST, &ioCfg, &s_panelIo);

esp_lcd_panel_dev_config_t panelCfg = {};   // reset=9, RGB order, 16bpp
esp_lcd_new_panel_st7789(s_panelIo, &panelCfg, &s_panel);

esp_lcd_panel_reset(s_panel);
esp_lcd_panel_init(s_panel);
esp_lcd_panel_invert_color(s_panel, true);
esp_lcd_panel_swap_xy(s_panel, true);
esp_lcd_panel_mirror(s_panel, false, true);   // 注意参数顺序，见 §3.3
esp_lcd_panel_set_gap(s_panel, 0, 35);
esp_lcd_panel_disp_on_off(s_panel, true);
```

**SPI host 选择**：用 `SPI2_HOST`。
注意 `common.h:100` 那条「必须用 SPI3、FSPI 收不到数据」的注释是**旧 tftLib
路径**的经验，不适用于这条 LVGL 路径。Arduino 的 `FSPI=0` 对应的就是 SPI2 总线，
之前能出画面的 `Arduino_HWSPI` 实际一直跑在 SPI2 上。
**IDF 的枚举编号和 Arduino 不同**（`SPI2_HOST` 在 IDF 里是 1），别按 Arduino 的
`FSPI=0` 去填。

**异步完成回调**：

```cpp
bool IRAM_ATTR onColorTransDone(esp_lcd_panel_io_handle_t,
                                esp_lcd_panel_io_event_data_t*, void* userCtx) {
    ++s_transDoneCount;
    auto* driver = static_cast<lv_disp_drv_t*>(userCtx);
    if (driver) lv_disp_flush_ready(driver);
    return false;   // 没有唤醒更高优先级任务
}
```

在 `lv_disp_drv_register()` **之后**用
`esp_lcd_panel_io_register_event_callbacks()` 注册，`userCtx` 传 `&m_displayDriver`。
函数体必须极小且不能阻塞（`lv_disp_flush_ready()` 只翻一个标志位，在 ISR 里安全，
这也是 esp_lcd 官方 LVGL 示例的写法）。

**flush 变成异步**：

```cpp
++s_flushCount;
const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                                area->x2 + 1, area->y2 + 1, colorMap);
if (err != ESP_OK) { s_lastDrawErr = err; ++s_drawErrCount; lv_disp_flush_ready(driver); }
// 成功路径**不**调 lv_disp_flush_ready —— 由 ISR 回调负责
```

失败时必须补一次 `lv_disp_flush_ready()`，否则 LVGL 会永远等这一块，整个 UI 卡死。

**绘图缓冲改为内部 DMA 内存**：

```cpp
const size_t bufferBytes = ((kWidth * kRowsPerBuffer * sizeof(lv_color_t)) + 63u) & ~(size_t)63u;
m_bufferA = heap_caps_aligned_alloc(64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
m_bufferB = heap_caps_aligned_alloc(64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
```

**常量调整**：

- `kRowsPerBuffer`：`80` → `16`。配合缓冲从 PSRAM 移回内部 RAM。
  80 行 ×2 在 PSRAM 里无所谓，换成内部 RAM 就是 100KB，这块板绝对给不起；
  16 行 ×2 = 20KB。
  **这不会增加传输总字节数**——覆盖同一块脏区域的数据量不变，只是拆成更多次
  `flush()`；而现在 flush 是异步的，多几次调用的开销远小于以前同步阻塞时代。
- `kTftSpiHz`：`100MHz` → `80MHz`。见 §3.1。

**保留 `Arduino_GFX* s_gfx = nullptr;`**：
剩下四个使用者全部有 null 保护、现在是 no-op，暂时保留（而不是连同 Arduino_GFX
依赖一起删掉）只是为了让回退保持「改一个文件」的成本。代价：

- `drawFallbackHome()`：开机前的占位画面不再绘制。无害，LVGL 一秒内就接管。
- `runSelfTest()`：本来就是死代码（`kLcdSelfTest == false`）。
- `applyDisplayRotation()` / `pollImuOrientation()`：本来就已失效（IMU 自动旋转
  2026-07-24 按要求移除），方向现在在 init 时由 `swap_xy`/`mirror` 固定。

`s_bus` 已彻底删除，丢掉 `Arduino_HWSPI` 后没有任何东西引用它。

### 2.2 `src/lv_conf.h`

`LV_COLOR_16_SWAP` `0` → `1`。

ST7789 走 SPI 期望**大端 RGB565**（高字节先发），而 LVGL 在 `SWAP=0` 时按 ESP32
的原生小端存。以前这个不匹配被 Arduino_GFX 悄悄补上了——它的 `writePixels()`
逐像素拆成 msb/lsb 再写出去；而 `esp_lcd_panel_draw_bitmap()` 把缓冲区字节
**原样**交给 DMA，不做任何交换。于是迁移后每个像素两个字节都是反的，**整屏花屏**。

打开 SWAP 让 LVGL 直接产出大端数据，DMA 零拷贝直传，比在 flush() 里逐像素交换
省一整遍内存读写。

> ⚠️ **这会改变 LVGL 全局的 `lv_color_t` 内存布局。**
> 任何**绕过 LVGL 直接写原始 RGB565 像素**的代码都得跟着交换。
> 本项目里就是图片解码那条路径（见 2.3、2.4）。
> 判断特征：如果只有图片颜色不对、UI 其它部分正常，就是这里漏了。

### 2.3 `lib/tftLib/tft_base.cpp`

在 `decodeJpgFromMemory()` 入口、`JDEC jdec;` 之前加一行：

```cpp
JPEG_setSwapBytes(true);
```

这个函数的四个调用方（本地封面、电台 logo、云音乐封面等）都把结果直接当
`LV_IMG_CF_TRUE_COLOR` 交给 LVGL，所以输出字节序必须跟着 §2.2 走。
**放在函数入口而不是四个调用点**，这样新增调用方不会忘。
不加的话表现是：UI 全对，但每张解码出来的图都是噪点。

### 2.4 `docs/cassette-design/png_to_rgb565.py` + 两个生成的资源文件

脚本原本按低字节先输出，改为高字节先（大端）：

```python
out.append((v >> 8) & 0xFF)
out.append(v & 0xFF)
```

并重新生成了烘焙资源：

- `src/ui/images/cassette_body_data.c`（320×170）
- `src/ui/images/cassette_reel_data.c`（60×60）

这两个 `.c` 的巨大 diff（约 11500 行）**全是重新生成的像素数据**，不是手写改动。

### 2.5 `platformio.ini`

只加了一段注释，记录 **2026-09-03 缓存实验已回退**，防止后人再踩。见 §4.1。

---

## 3. 修复过程中踩的四个坑（根因已确认，不用重查）

### 3.1 `pclk_hz=100MHz` 被 esp_lcd 直接拒绝 → 屏幕完全不刷新

`esp_lcd_new_panel_io_spi()` 返回 `ESP_ERR_INVALID_ARG`，`begin()` 提前返回，
显示驱动根本没建起来。表现：`flush/s=0`、屏幕不刷新、CPU 占用 0.2%。

**ESP32-S3 的 GPSPI 硬件上限就是 80MHz。**

> **连带推翻一条既有错误结论**：08-07 日志里「LCD SPI 100MHz 实测稳定」是假象。
> Arduino 的 `SPIClass` 只按分频系数取最接近的可用档位、**不做上限校验**，
> 所以那时候实际一直跑在 80MHz，从来没真到过 100MHz——换成会严格校验的
> esp_lcd 之后才暴露。
> 也就是说这里从 100 改回 80 **不损失任何实际速度**，只是把配置写成了硬件
> 真正能做到的值。

### 3.2 绘图缓冲在 PSRAM → DMA 不可达 → 丢传输

诊断输出：`bufA_psram=1 bufA_dma=0 draw_err=2 last_err=ESP_ERR_NO_MEM`

**根因**：`spi_master.c` 遇到非 DMA 可达的 tx buffer 时，会走 bounce-buffer 路径
——每次传输临时 `heap_caps_aligned_alloc(MALLOC_CAP_DMA)` 一块内部内存再 memcpy；
内部 RAM 紧张时分配失败就 `goto clean_up` 返回 `ESP_ERR_NO_MEM`，这一块屏幕区域
就保持旧内容不动。80 行的缓冲意味着每次要临时抠 51KB 内部内存，开机时实际失败过 2 次。

**修复**：缓冲改用 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`，同时把
`kRowsPerBuffer` 从 80 降到 16 把内存占用压到 20KB。

> ⚠️ **一个中途走过的弯路**：我先试过「把 PSRAM 缓冲做 64 字节对齐」，**完全无效**。
> `esp_ptr_dma_capable()` 的语义是「**内部** DMA 可达内存」，对 PSRAM
> **无条件返回 false**，跟对齐没有任何关系。别再往这个方向试。

### 3.3 `esp_lcd_panel_mirror()` 参数写反 → 方向/位置错 + 触摸失效

第一版按「竖屏 170×320、偏移 (35,0)、横屏后轴向交换」**推导**出的
`mirror(true, false)` 是错的，正确值是 `mirror(false, true)`。

第二次没有再靠猜，而是回去读 Arduino_GFX 源码里这块屏对应的 `MADCTL` 的
MX/MY/MV 位，反推出 esp_lcd 的 `swap_xy`/`mirror` 该填什么。
**新外设的方向参数不要试错，去读上一版驱动实际写进 MADCTL 的值。**

触摸跟着失效是连带现象——触摸坐标映射依赖显示方向。

### 3.4 图片花屏（UI 正常）

就是 §2.2 那个警告说的情况，修复见 §2.3 + §2.4。

---

## 4. 已确认走不通的方向（不要重试）

### 4.1 加大 CPU 缓存 —— 会让板子从 USB 彻底消失

试过指令缓存 16KB→32KB、数据缓存 32KB→64KB、并关闭 ESP-IDF 断言。
三项配置确认生效（编译期 `sdkconfig.h` 里是 `0x8000`/`0x10000`/`LEVEL 0`），
固件也正常编译烧录成功，但板子刷完后**从 USB 上彻底消失**——连按 BOOT+RESET
进 ROM 下载模式都认不到，需要物理干预才恢复。

缓存那 48KB 是直接从内部 SRAM 划走的，而这块板内部 RAM 本来就紧
（`setupLvglRuntime()` 的注释：WiFi 的 `esp_wifi_init()` 要约 35KB，
一直在跟音频 I2S DMA 抢）。一次拿走 48KB 太激进。

以后若重试：一次只动一项，优先只把指令缓存加到 32KB（多占 16KB），
先确认能开机、WiFi 能连、能跑一段时间，再考虑数据缓存。

> ⚠️ **配置陷阱**：`sdkconfig.defaults` 里写死了这些同名 key，
> **光改 `custom_sdkconfig` 会被静默忽略**（这次先踩了这个坑，白测了一轮）。
> 每次改完务必查 `.pio/build/<env>/config/sdkconfig.h` 确认真的生效。

### 4.2 `esp_lvgl_port` —— 见 §1 末尾

---

## 5. 关于性能：本次**不做**任何提升声称

`docs/DEV_LOG_2026-09-03_esp_lcd_wip.md` §1 里那张对比表
（CPU 35.5%→24%、最长阻塞 20.0ms→14ms）**不要引用**。

原因：那组「迁移后」的数字是在**屏幕还在花屏、图片资源还没修好**的状态下测的，
渲染的内容和迁移前根本不是同一个东西，不具备可比性。

迁移完成后曾计划做一次严格对比（引入与内容/缓冲尺寸无关的 `bytes/s`、
`us_per_kb` 指标，并扫 `kRowsPerBuffer`），**该计划已按要求放弃**，
测量用的临时插桩也已撤销，当前分支不含任何测量代码。

**能确定的只有一条**（这是功能正确性，不是性能声称）：

```
flushes=17758  trans_done=17758  draw_err=0
```

提交次数与 DMA 完成次数**完全相等、零丢帧**，异步链路在正确工作。

要做性能结论的话，必须重新测一次干净的 A/B——两边都在图像正确的前提下跑。

---

## 6. 真机验证状态

- 编译：SUCCESS，RAM 30.2%（98828 / 327680 B），Flash 30.8%（5170158 / 16777216 B）
- 烧录：COM5 @ 0x10000，`Hash of data verified`
- 运行：SD 扫描正常（`[MUSIC] scan done, 54 tracks`），
  内部空闲 ≈155KB、DMA 可用 ≈148KB、PSRAM 空闲 ≈3.0MB
- 显示：方向 / 位置 / 颜色 / 图片全部正常；触摸正常

> WiFi 在本机报 `no matching wifi found` 是**环境问题不是回归**——
> `platformio_override.ini` 里配的是家里的 SSID。

---

## 7. 环境 / 流程注意事项（这台 Windows 机器）

1. **必须用原生 PowerShell 跑 PlatformIO**，不能用 Git Bash——
   idf_tools 会报 "MSYS/Mingw is not supported"。
2. **路径不能有空格**。已建 junction：
   `C:\mwr-src` → `...\esp32-s3 HIFIPlayer\ESP32-MiniWebRadio-src`。
   所有编译都在 `C:\mwr-src` 下跑，加 `-j 2`。
3. **`platformio_override.ini` 是 gitignored、含真实 WiFi 密码，永远不要提交。**
   它会**整体替换** `[common].build_flags`——曾因此漏掉 `MWR_LVGL_UI` /
   `LV_CONF_INCLUDE_SIMPLE`，导致所有 `#if MWR_LVGL_UI` 块被静默关掉，
   报了个看起来像代码 bug 的 `playerCoreWifiAddNetwork was not declared`。
   该文件头部注释里有漂移自检片段。
4. **app 启动时 USB 会重新枚举，初始化阶段的 printf 抓不到**——
   带复位抓取会断线，不带复位抓取会错过开头。
   诊断信息必须做成**周期性打印**。
5. **LVGL 卡死时 esptool 可能连不上**（app 占着 USB）。恢复：
   全程按住 BOOT 键的同时拔插 USB，插好后再松开。
   ⚠️「USB Composite Device」存在**不**代表 app 在跑——
   ROM 的 USB-Serial/JTAG 本身就是复合设备，别用这个判断。
6. **读 flash 失败时优先加 `--no-stub`**，不是降波特率。

### 完整回退手段

`backup/board_backup_20260903/full_16mb.bin`——完整 16MB、`esptool verify-flash`
逐字节校验过，步骤见该目录 README。
**该目录在仓库之外、不会被提交**（整片 flash dump 含 NVS 运行时数据，可能有 WiFi 凭据）。

---

## 8. 一个流程教训

第一版我**没有检查 `esp_lcd_panel_draw_bitmap()` 的返回值**就直接烧录了。
结果一个本该立刻暴露的配置错误（100MHz 超上限）变成了**静默卡死**，
多花了两轮烧录和一次物理干预才定位。

**以后引入新的外设 API，返回值检查和诊断要和第一版代码一起写**，不要等出问题再补。

---

## 9. 遗留项 / 下一步候选

- [x] ~~删掉 `Arduino_GFX` 依赖~~ —— **已完成 2026-09-05，见 §10。**
- [ ] ~~删掉 `src/waveshare/esp_lcd_sh8601.*` 残留~~ —— **这条是错的，已撤销。**
      那两个文件**不是残留**：`lib/tftLib/tft_spi.h:18` include 它，
      `tft_spi.cpp` 实际调用 `esp_lcd_new_panel_sh8601()` 一整套；而 tftLib 本身
      在运行时是活的（`main.cpp` 有 4 处 `getTFT().decodeJpgFromMemory()` 在解
      本地/云音乐封面）。删掉会直接编译失败。
- [ ] 重新做一次干净的 A/B 性能对比 + 扫 `kRowsPerBuffer`（见 §5）
- [ ] `refresh()` 里的冗余 invalidate 优化
- [ ] 08-07 日志的未完项：lrclib.net 模糊匹配、SYLT `tsFormat==1`

---

## 10. 2026-09-05 追加：移除 Arduino_GFX 依赖

### 改了什么

- `src/ui/waveshare_lvgl_port.cpp`
  - 删 `#include <Arduino_GFX_Library.h>`、删 `Arduino_GFX* s_gfx = nullptr;`
  - 删 `drawFallbackHome()` 整个函数 + 两处调用（`begin()` 里一处、
    `tick()` 里那个 `s_flushCount == 0` 时每 2 秒重绘的兜底块）
  - 删 `runSelfTest()` + `kLcdSelfTest`（常量恒为 false，本来就是死代码）
  - 删随之失去使用者的常量：`kBootColorFlash`、`kDisplayRotationNormal/Flipped`、
    六个颜色常量 `kBlack/kRed/kGreen/kBlue/kCyan/kWhite`、`s_lastFallbackDraw`
  - `applyDisplayRotation()` 去掉 `setRotation()` 调用，只保留 `m_displayFlipped`
    赋值（该标志仍参与触摸坐标映射，函数不能删）
  - `pollImuOrientation()` 的守卫去掉 `|| !s_gfx` 项。该函数目前没有任何调用点，
    行为不变。
  - 初始化日志文案 `[LCD] init Arduino_GFX ST7789` → `[LCD] init esp_lcd ST7789`
- `src/ui/waveshare_lvgl_port.h`：删 `void runSelfTest();` 声明
- `platformio.ini`：更新 `lib_deps` 上方注释，说明依赖已移除、
  以及 `lib/lvgl`、`lib/tftLib` 不要一起清理

`lib/Arduino_GFX/`（vendored、gitignored）**目录本身保留未删** ——
`probes/lvgl9_probe/` 是个独立的 PlatformIO 探针工程，仍然 include 它。
主工程这边没有任何源文件再 include，LDF 不会把它拉进依赖图，所以留着不产生成本。

### 实测结果 —— 两条预期收益只兑现了一条

真机验证：编译通过、烧录校验通过、板子正常启动，
`[LCD] init esp_lcd ST7789` → `panel ready (SPI2, 80000000 Hz)` → flush 正常，
`[PERF] flush/s≈192~204, busy≈25~28%`。显示、触摸无异常。

**固件体积：没有减小，反而略微增大。**

| | 移除前（`149cb4e`） | 移除后 | 变化 |
|---|---|---|---|
| Flash | 5170158 B | 5172134 B | **+1976 B（+0.04%）** |
| RAM | 98828 B | 98820 B | −8 B |

原因：ESP-IDF 默认带 `-ffunction-sections -fdata-sections` + `--gc-sections`，
迁移到 esp_lcd 之后没有任何符号再引用 Arduino_GFX，**链接器早就已经把它整个丢掉了**
——固件里本来就没有它，自然没有体积可回收。
反证也成立：如果之前真被链进去了，去掉 100 个 `.o` 应该掉几十 KB，而不是这个量级。
`nm firmware.elf | grep -c Arduino_GFX` 现在是 `0`。
那 +2KB 属于链接布局/对齐噪音，在 16MB flash 上无实际意义。

**编译时间：只在完整重编译时有收益，增量编译没有。**

Arduino_GFX 是 **100 个编译单元**，占本工程 1744 个 `.o` 的约 **5.7%**。
完整重编译时这部分开销消失；但增量编译本来就命中缓存
（改动前那批 `.o` 时间戳还停在 08-31，最近几次构建根本没重编过它们），所以没有差别。

> ⚠️ **未做严格测量**：没有跑「移除前 / 移除后各一次 clean 全量构建」的计时对比，
> 上面 5.7% 是编译单元占比推算，不是实测秒数。要准确数字得跑两次全量构建。

**结论**：这次改动的实际价值主要是**代码清晰度**——删掉了一整套永远不会执行的
no-op 代码路径和随之悬空的常量，以及一条已经名不副实的第三方依赖。
体积和增量编译时间上都不要期待收益。

---

## 11. 2026-09-05 追加：脏区域治理（UI 层，非驱动层）

### 起因

esp_lcd 迁移之后驱动层已经没什么可榨的（SPI 到硬件上限 80MHz、异步 DMA、
零拷贝字节序），但 `[PERF]` 仍显示 CPU 占用 33~36%、单次 `lv_timer_handler()`
最长阻塞 21~22ms。查下来瓶颈在 UI 逻辑层，不在驱动。

### 根因：LVGL 8 的 setter 一律无条件标脏

- `lv_label_set_text()` 的**第一行**就是 `lv_obj_invalidate(obj)`，之后才看文字内容
- `lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN)` 同样先 invalidate，不管本来是否已隐藏
- `lv_obj_set_style_*()` 走 `lv_obj_refresh_style()`，同理

而 `HifiUi::tick()` 每 60ms 调一次 `refresh()`，`refresh()` 把所有控件全量重写一遍。

**精确证据**：`flush/s` 稳定在 192 / 204 之间跳 = `16×12` 和 `17×12`，
即每次 `refresh()` 恰好产生 12 次 flush，而满屏 = `ceil(170/16) = 11` 次。
换算成与假设无关的量：`200 flush/s × 320×16×2 B = 2.05 MB/s`，
满屏 108.8 KB → **一个静止界面每秒被整屏重绘约 19 次**。

### 做法

在 `hifi_ui.cpp` 的匿名 namespace 里加一组"值变了才写"的包装，
然后把 18 个 `refresh*` 函数体内的 **173 处**调用全部换掉：

| 包装 | 替换数 |
|---|---|
| `uiSetText` | 95 |
| `uiSetHidden`（add/clear HIDDEN） | 29 |
| `uiSetBgColor` | 14 |
| `uiSetPos` | 10 |
| `uiSetBgOpa` / `uiSetTextColor` | 9 / 9 |
| `uiSetBarValue` | 3 |
| `uiSetBorderColor` / `uiSetShadowColor` / `uiSetHeight` | 2 / 1 / 1 |

两个实现要点：

1. **读样式用 `lv_obj_get_local_style_prop()`，不用 `lv_obj_get_style_*()`。**
   后者返回的是**计算后**的值（叠加 state / transition / 继承），控件处于
   按下或聚焦态时会和我们要写的本地值不一致，导致误判。local 版读的就是
   自己 set 进去的那一层，语义正好对上。
2. **替换只改函数名、保留全部实参**（包装的 selector 参数带默认值，签名兼容），
   避免用正则动参数出错。只有 `LV_OBJ_FLAG_HIDDEN` 那两个需要改参数形态。

`build*` 那类一次性构造代码**没有改**——对象刚建出来没有旧值可比，包装没意义。

### 实测（同一块板，Home 页空闲，WiFi 已连）

| 指标 | 改造前 | 改造后 | 变化 |
|---|---|---|---|
| `flush/s` | 192~204 | **16~21** | ↓ 约 10 倍 |
| CPU busy | 33~36% | **4.4~5.0%** | ↓ 约 7 倍 |
| 单次最长阻塞 `max_call_us` | 21000~22400 | **~1300** | ↓ 约 16 倍 |
| Flash | 5172134 B | 5175570 B | +3436 B（包装函数） |

`timer_handler_calls/s` 从 ~300 涨到 ~670 是预期内的：每次调用变便宜了，
循环转得更快。全程无 `draw_bitmap dropped` 告警。

`max_call_us` 21ms → 1.3ms 是对音频最有价值的一项——`loop()` 里
`audio.loop()` 被 LVGL 挤占的最坏时长少了一个数量级。

### 连带影响：第二条建议的紧迫性下降

原本排在第二位的"把 LVGL 移到独立任务 / core 0"，动机是
`lv_timer_handler()` 单次 15~22ms 会卡住同线程的 `audio.loop()`。
现在最坏 1.3ms，这个风险大幅缓解，**建议重新评估是否还值得做**
（那个改动要引入 LVGL 互斥锁，正确性风险不低）。

### 待人工确认

脏区域治理的固有风险是"该更新的地方没更新"。串口无异常，但**画面正确性
需要肉眼验证**，重点看：时间/日期/天气、音量条与百分比、播放暂停图标、
进度条、播放时的频谱与转盘动画、各页面切换、云音乐列表加载。

---

## 12. 2026-09-05 追加：CPU 频率策略 —— 固定 240MHz，**这是有意为之，不要"优化"**

查证结论（编译期 `sdkconfig.h` 实测）：

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ        240
CONFIG_PM_ENABLE                       未定义  → 动态调频(DFS)整个功能关闭
CONFIG_FREERTOS_USE_TICKLESS_IDLE      未定义  → 空闲不进 tickless
CONFIG_PM_DFS_INIT_AUTO                未定义
```

`src/` 与 `lib/tftLib/` 中**没有任何** `setCpuFrequencyMhz()` / `esp_pm_configure()` /
`esp_pm_lock_*` / `esp_light_sleep_start()` 调用。即 CPU 全程满频 240MHz 运行。

> ⚠️ **一个容易看错的地方**：`sdkconfig.h` 里能 grep 到
> `CONFIG_PM_SLP_IRAM_OPT=1` 和 `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=1`，
> 但它们是 `CONFIG_PM_ENABLE` 的**子选项**，主开关没开时完全不起作用。
> 别据此误判成"省电已经开了"。

### 决定：维持固定 240MHz（2026-09-05 用户确认）

设备是常插电的桌面播放器，**不是电池供电**，所以 DFS 的唯一收益（功耗/发热）
对本项目没有价值，而风险直接落在最不能出问题的地方：

- DFS 会在 80/160/240MHz 之间切换，**APB 时钟跟着变**，扰动 I2S 与 SPI 时序
- 外设必须正确持有 PM lock 才安全，而 Arduino 的 audio 库那条路径是否规范持锁
  **从未验证过**
- 收益是功耗，不是性能——满频不会让程序变慢，只是空闲时白烧电
  （当前 `busy` 仅 4.6%，CPU 约 95% 时间在空转）

**若将来要做便携电池版**，DFS + tickless idle 是必须补的功课，但那是一个
需要单独验证一轮的课题（每个外设的 PM lock 都要过一遍），不能顺手改。

### 顺带查到：WiFi 射频省电是开着的，且与 CPU 策略不一致

arduino-esp32 的 `WiFiGenericClass::_sleepEnabled` 在非 ESP32-S2 目标上默认是
`WIFI_PS_MIN_MODEM`，而本工程**从未调用 `WiFi.setSleep()` 覆盖它**。
所以射频侧在省电模式、CPU 侧满频。目前没观察到由此引发的问题，
但排查网络相关的延迟/丢包时值得记住这一条。
