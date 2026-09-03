# Dev Log — 2026-09-05：USB 声卡模式（UAC2）—— ⚠️ 本文原标题「真机验证通过」是错的

> **⚠️ 更正（2026-09-06 追加，2026-09-07 再次更正）：本文原先声称的「真机验证
> 通过 / 出声正常、音质正常」是错的，而且比 9/6 当时以为的还要错。**
>
> - 9/6 的认识：「只在 Windows 上验过，macOS/iOS/Android 全无声」
> - **实际情况：Windows 上也从未验证过。** 当时那句「有声音、音质还可以」是
>   随口说的，没有真的测。也就是说**任何主机上都没有出过声**。
>
> 这与根因完全自洽：rx 回调名写错（见
> `DEV_LOG_2026-09-06_usb_dac_no_sound.md` §2.1），数据一个字节都进不了环形
> 缓冲，物理上不可能有声音。当时屏幕上那组 `under` 十万多 / `over` 恒为 0
> 的读数，正是「从未收到任何数据」的准确表现——只是被误读成了别的东西。
>
> **本文中凡涉及"验证通过 / 出声正常"的结论一律作废**，但 §2~§5 记录的协议
> 上限、实现细节和踩过的坑仍然有效（那些是查证过的事实，不是验证结论）。
> 真正跑通是在 `DEV_LOG_2026-09-06_usb_dac_no_sound.md`（macOS 实测）。
>
> 分支 `codex/usb-audio-dac-20260905`。

---

## 1. 这是什么

把这块板当成电脑/手机的**外挂 USB 声卡**：

```
PC / 手机 --USB 等时传输--> ESP32-S3 --I2S--> PCM5100A --> 3.5mm 耳机口
```

形态是**播放器的第三个独立模式**，和 U 盘（MSC）模式并列、互斥——一个 USB 口、
一套 TinyUSB 配置，同时只能是一种用途。

### 切换方式

| | 进入 | 退出 |
|---|---|---|
| U盘模式 | 设置 → U盘 → 「挂载U盘」 | 主机弹出 / 屏上卸载 |
| **声卡模式** | 设置 → U盘 → 「声卡模式」 | 声卡页的「退出声卡模式（重启）」 |

两个入口并排放在同一行，让互斥关系一眼可见。切换必须重启（约 3 秒），这是硬性
限制不是设计选择：这块板运行时调 `USB.begin()` 会触发 `ESP_RST_USB` 复位
（见 `main.cpp` 里那段注释和 DEV_LOG 2026-07-31），TinyUSB 只能开机时启动。

**屏幕和触摸完全独立于 USB**，所以「退出」按钮在声卡模式下始终可用，不会出现
"变成声卡就回不来了"。初始化失败时固件也会自动清标志重启回正常模式。

---

## 2. 硬性上限：ESP32-S3 的 USB 只有全速

`CFG_TUD_MAX_SPEED = OPT_MODE_FULL_SPEED`（12 Mbps）。全速等时端点每毫秒最多
1023 字节：

| 格式 | 带宽 | 可行性 |
|---|---|---|
| 48kHz / 16bit / 立体声 | 192 KB/s | ✅ 轻松 |
| 96kHz / 24bit / 立体声 | 576 KB/s | ✅ 可行 |
| 192kHz / 24bit / 立体声 | 1152 KB/s | ❌ **超上限，这块芯片做不到** |

要更高得换有 USB 高速的芯片（ESP32-P4）。v1 只做 44.1k/48k、16bit、立体声。

---

## 3. 实现要点

改动集中在新增的 `src/usb_dac.{h,cpp}`，加上四个文件的接线。

### 3.1 不需要引入新组件

`espressif/tinyusb` 的 CMakeLists **无条件编译 audio 类**（`cdc/hid/midi/msc/…/
audio/video/…` 全在 SRCS 里），所以 `tud_audio` 本来就编进来了，只是
`CFG_TUD_AUDIO` 被设成 0。直接用 TinyUSB 原生 UAC 即可，**不用**
`espressif/usb_device_uac`（那个会拉进它自己依赖的一份 LVGL/组件树）。

### 3.2 描述符

TinyUSB 0.21 只提供 `TUD_AUDIO20_DESC_*` 积木块，没有现成的"扬声器"整套宏，
描述符要自己拼。总长 **143 字节**，必须和 `CFG_TUD_AUDIO_FUNC_1_DESC_LEN`
完全一致（驱动按这个长度解析，对不上直接枚举失败）：

```
IAD 8 + STD_AC 9 + CS_AC 9 + CLK_SRC 8 + INPUT_TERM 17
+ FEATURE_UNIT(2ch) 18 + OUTPUT_TERM 12
+ STD_AS(alt0) 9 + STD_AS(alt1) 9 + CS_AS_INT 16 + TYPE_I_FORMAT 6
+ ISO_EP 7 + CS_ISO_EP 8 + ISO_FB_EP 7  = 143
```

`CS_AC` 的 `_totallen` 按 TinyUSB 注释要求：只算它后面那些 Clock Source /
Unit / Terminal，**不含 CS_AC 自身**。

### 3.3 数据通路

```
USB 等时 OUT --> tud_audio_rx_done_post_read_cb() --> 自己的 SPSC 环形缓冲(24KB)
                                                          |
                          i2sTask (core 0, 优先级 5) <-----+
                                     |
                              i2s_channel_write() --> I2S DMA(6x240帧 ≈ 30ms)
```

I2S 任务钉在 core 0（core 1 留给 Arduino loop + LVGL），优先级 5 高于 LVGL。

---

## 4. 踩过的坑（根因已确认，不用重查）

### 4.1 `tinyusb_enable_interface2()` **不会启动 USB 栈**

第一版只注册了描述符回调就完事，结果：**屏幕正常进了声卡页，但主机侧毫无反应**
——Windows 的声音设备列表里什么都没有。

必须再调 `USB.begin()`。MSC 模式里对应的是 `usbStoragePrepareMsc()` 末尾那段
（`main.cpp:1792`），照搬时漏了。

```cpp
USB.manufacturerName("ESP32-S3 HiFi");
USB.productName("MiniWebRadio DAC");
if (!USB.begin()) return false;
```

### 4.2 `common.h` 不能被第二个编译单元包含

它定义的是 `audio` / `pref` / `webSrv` / `rtc` / `i2cBusOne` / `s_tag` / `spiBus`
等全局对象的**本体**而非 `extern` 声明，设计上只供 `main.cpp` 独占。
`usb_dac.cpp` 包了它 → 链接期十几个 `multiple definition`。

改成只包 `settings.h`（要三个 I2S 引脚宏）。settings.h 是安全的：里面的 `const`
对象在 C++ 里是内部链接，每个编译单元各一份，而且都在别的板型的 `#if` 分支里。

### 4.3 arduino-esp32 已经定义了 `tud_mount_cb` 等四个回调

`cores/esp32/USB.cpp` 实现了 `tud_mount_cb` / `tud_umount_cb` /
`tud_suspend_cb` / `tud_resume_cb`（它要转成 ArduinoUSB 事件），我们再定义一遍
就是 `multiple definition`。枚举状态改用轮询 `tud_mounted()`。

### 4.4 `tud_audio_feedback_interval_isr` 是**驱动提供给应用调用**的函数

不是应用要实现的回调。我当回调实现了，会和驱动自身的定义符号冲突。

同时 `tud_audio_feedback_params_cb()` 返回 **void 不是 bool**；而且它内置的
`AUDIO_FEEDBACK_METHOD_FIFO_COUNT` 看的是 **TinyUSB 自己的 EP FIFO**——我们在
`rx_done_post_read_cb` 里已经把它立刻搬空了，水位恒为 0，用那个方法算出来的
反馈值是错的。

最终做法：**不实现** `tud_audio_feedback_params_cb`（驱动弱默认就是
`AUDIO_FEEDBACK_METHOD_DISABLED`，正是我们要的），反馈值由 `updateFeedback()`
从自己的环形缓冲水位算，调 `tud_audio_n_fb_set()` 写进去。

### 4.5 枚举名（这一版 TinyUSB 的实际名字）

| 我以为的 | 实际 |
|---|---|
| `AUDIO_FUNC_HEADPHONES` | **不存在**。HEADPHONES 只是**终端类型**（`AUDIO_TERM_TYPE_OUT_HEADPHONES`），功能类别用 `AUDIO20_FUNC_DESKTOP_SPEAKER` |
| `AUDIO_FEATURE_UNIT_CTRL_MUTE` | `AUDIO20_FU_CTRL_MUTE`（`_POS` 后缀的那组是描述符里用的位偏移，别混） |
| 自己拼 range 匿名结构 | 用类型宏 `audio_control_range_4_n_t(N)`——它是 packed 的，布局必须逐字节对齐 UAC2 规范 |

### 4.6 `buildUsbStorage()` 后半段是**死代码**

这个函数开头是一个**无条件的裸代码块**，末尾直接 `return`：

```cpp
void HifiUi::buildUsbStorage() {
    {
        lv_obj_t* usbScreen = lv_scr_act();
        ...
        refreshUsbStorage();
        return;          // ← 永远在这里返回
    }
    lv_obj_t* screen = lv_scr_act();   // ← 以下全是死代码
    ...
}
```

我把「声卡模式」按钮加进了后半段，界面上完全没反应。**往那里加任何东西都不会
生效**，代码里已加注释标注。

### 4.7 `lv_font_cjk_16` 是子集化字体

`hifi_fonts.h` 写明它 "intentionally limited to fixed UI title glyphs"（为规避
LVGL 8 的 bitmap-index 溢出）。用它渲染新写的中文 → **直接乱码**。

动态中文必须用 `HIFI_FONT_DYNAMIC_TEXT`（= `lv_font_cjk_13`，广字符集）。
`buildAudioTopBar()` 的标题也走 cjk_16，所以声卡页标题用了纯 ASCII 的 `USB DAC`。

### 4.8 欠载计数器测了一个不存在的事件

第一版跑十分钟报了 **11 万次欠载**（约 183 次/秒），但**声音完全正常**。
两者矛盾——真有那个频率的断流，声音会碎得没法听。

根因：`i2s_channel_write()` 会一直搬到 I2S DMA 塞满才阻塞，所以 I2S 任务是
**贪婪消费者**，它必然把环形缓冲抽空、下一轮发现是空的。**这是稳态下的正常现象**，
不是断流。真正在护着音频的是 I2S DMA 那 30ms 缓冲，它一直没空过。

`over` 恒为 0 也印证了：主机发送速率 ≤ 消费速率，缓冲根本堆不起来。

**但这个假警报盖住了一个真问题**：缓冲长期贴着 0 跑 = 除了 DMA 那 30ms 之外零
余量，任何调度抖动都会变成真断流。根因是漏了**预填**——开流时清零后立刻开始
消费，它再也没机会涨到目标水位。

修复三处：

1. **预填**：开流后先攒到 `kRingTargetBytes`（25%，≈31ms）再开始送 I2S；
   真干涸后退回预填重新攒（真实 USB 声卡的重同步做法）
2. **计数语义**：只有「已预填 + 正在传 + 仍然干涸」才算真欠载
3. **反馈增益 1024 → 8192**：原值满偏只能修正约 0.016%，而两块晶振之间的典型
   漂移就有 0.01% 量级，几乎没有调节余量；新值给出约 0.08%，高一个数量级，
   同时远小于会引起可闻音高抖动的幅度

延迟预算：目标水位取 1/4 而非 1/2 是为了压延迟。24KB 满缓冲 @48k/16bit/立体声
约 125ms，1/4 约 31ms，叠加 I2S DMA 的 30ms，总延迟约 60ms——音乐没问题，看
视频也还不至于明显唇音不同步；取 1/2 就会到 90ms 以上。

### 4.9 退出声卡模式必须用 `usb_persist_restart()`

ESP32-S3 的 USB-OTG（TinyUSB）和 USB-Serial/JTAG **共用同一对 D+/D- 引脚**，
同时只能有一个生效。本次启动里调过 `USB.begin()` 之后 PHY 就被 OTG 控制器占住，
普通的 `ESP.restart()` 不保证把它干净地交还给 ROM 的 USB-Serial/JTAG。

**实测表现**：退出后板子在 USB 上彻底消失——没有串口、没有音频设备、连
`VID_303A` 都查不到，必须按住 BOOT 拔插才能恢复（花了一次物理干预）。

改用 `usb_persist_restart(RESTART_NO_PERSIST)`（声明在 `esp32-hal-tinyusb.h`，
`<USB.h>` 不转出来，要显式包含），它会先复位 USB 外设再重启。只在确实进过声卡
模式时才用——正常模式下 TinyUSB 从没启动过，没必要动 USB 外设。

---

## 5. 屏幕是这个模式下唯一的仪表盘

TinyUSB 接管 USB 口后 **USB-Serial/JTAG 控制台会消失，printf 一个字都抓不到**。
所以诊断必须做在屏幕上，而且要和第一版代码一起写——这是 esp_lcd 迁移那次的教训。

`Page::UsbDac` 显示：

```
等待主机连接... / 已连接（待机）/ 正在播放
48000 Hz  16 bit  2ch  [静音]
buf 6000/24576 B (25%)  target 25%  [预填中]
under 0  over 0  pkts 1234567
```

最后一行是调时钟同步的**唯一手段**：稳定运行时 `under`/`over` 应当长时间不涨。

---

## 6. 已知限制 / 下一步

### UAC 协议里没有元数据

UAC2 传输的**只有 PCM 采样**，协议里不存在歌名、艺术家、封面这些字段。所以
"声卡模式下显示当前播放的歌曲"**靠 UAC 本身做不到**，这是协议层面的限制。
主机能下发的只有采样率、位深、声道数、音量、静音——已经全在屏上了。

### 下一步计划

- [ ] **方案 A：把音频流本身可视化**（推荐）。PCM 数据就在环形缓冲里：
      算 RMS 驱动磁带转轮/VU，用 esp-dsp 做 FFT 驱动点阵频谱，
      复用现成的 `buildCassetteVisual()` 和 `drawSpecDot()`。
      **对任何主机都成立**，PC/iPhone/Android 零主机侧安装。
- [ ] **方案 C：HID 媒体控制**。再挂一个标准 HID Consumer Control 接口，
      触摸屏就能给主机发播放/暂停/上下一首。HID 是标准协议，主机零安装。
- [ ] 诊断信息收进二级页面（`under`/`over`/`pkts` 不能删，但不该占主界面）
- [ ] 更高采样率（96k/24bit）——等时钟同步长期稳定后再加
- [ ] ~~方案 B：元数据走第二通道（UAC+CDC + PC 伴随程序）~~ —— **暂不做**。
      需要每台电脑装程序，且**手机上完全行不通**，为一行歌名丢掉"插上就能用"
      这个最大优点，不划算。

---

## 7. 追加：界面重做（方案 A 第一步）—— ⚠️ 尚未上机验证

> **状态：编译通过，但还没烧进板子。** 收尾时板子在 USB 上没有出现（既无串口、
> 也无音频设备），来不及验证。下次接手请先烧录并按 §7.4 的检查项确认。

### 7.1 为什么是"把声音画出来"而不是显示歌曲信息

UAC2 协议里**只有 PCM 采样**，不存在歌名 / 艺术家 / 封面这些字段（见 §6）。
主机能下发的只有采样率、位深、声道、音量、静音。

所以"声卡模式下显示正在放什么歌"做不到——但 **PCM 数据本身就在我们手上**，
把声音可视化反而比元数据更直接，而且**对任何主机都成立**：PC / iPhone /
Android 插上就有，不需要在主机侧装任何东西。这恰好保住了"手机外挂耳放"
那个最初场景。

（对比之下，走第二通道拿元数据要求每台电脑装伴随程序、手机上完全行不通，
为一行歌名丢掉"插上就能用"这个最大优点，不划算。已记在 §6 里存档。）

### 7.2 音频侧：峰值电平检测

在 `tud_audio_rx_done_post_read_cb()` 里直接从 PCM 算左右声道峰值：

- **用峰值而不是真 RMS**：每包才 49 帧，峰值更便宜，视觉上也更像传统 VU 表
- **弹道"快起慢落"**：新峰值立即顶上去，否则每包（1ms）衰减 3，约 85ms 落到底
- ⚠️ 取绝对值必须用 `int32_t` 中转——`-INT16_MIN` 在 `int16_t` 里会溢出
- 放在写环形缓冲**之前**：静音时 `scratch` 已被清零，VU 自然归零，不用额外判断

导出为 `UsbDacStatus.vuLeft` / `vuRight`（0..255）。未在传输时强制归零。

### 7.3 界面：复用磁带视图

直接复用本地播放页那套真实位图美术（`buildCassetteVisual()`：底图满屏
320x170，两个转轮是独立小图，靠 `lv_img_set_angle()` 旋转）：

```
┌────────────────────────────────┐
│ ◎        正在播放         ◎    │  ← 转轮 14 秒一圈，只在真正出声时转
│ ↻     48.0k / 16bit / 2ch  ↻   │
│        ▬▬▬▬▬▬▬▬▬▬▬░░░░░        │  ← 左声道 VU（148px 满程）
│        ▬▬▬▬▬▬▬▬▬░░░░░░░        │  ← 右声道 VU
│        [ 退出声卡模式 ]         │
└────────────────────────────────┘
```

转轮只在 `streaming && !muted` 时转，和本地播放页同一条动画预算规则。

**诊断行改为默认收起**，点屏幕中间的磁带标签区（透明热区 160x66）切换显示。
`under`/`over`/`pkts` 是声卡模式下调时钟同步的**唯一手段**（串口控制台被
TinyUSB 占用，printf 抓不到任何东西），所以不能删，但也不该常驻主画面。
收起时 `refreshUsbDac()` 直接跳过它们的 snprintf + 文本写入，省掉脏区域。

VU 的宽度每帧都在变，**故意没有走 `uiSetWidth` 那类脏检查包装**——这里本来
就是需要重画的动画，加检查没有意义（脏区域治理的原则是"值没变就别写"，
不是"什么都别写"）。

### 7.4 下次接手要验证的

- [ ] 磁带底图和转轮在声卡页正常显示（这套美术原本只在 Local Now Playing 用过，
      换个页面挂上去可能有层级/裁剪问题）
- [ ] 放音乐时转轮转、停止/静音时停
- [ ] 左右 VU 跟着音乐动，且左右声道能分开（放个单声道偏移的测试音确认没接反）
- [ ] 点标签区能切出/收起诊断行
- [ ] 退出按钮位置（88,146 尺寸 144x22）没有被磁带底图盖住、能点中
- [ ] 加了 VU 和转轮动画之后 CPU 占用（脏区域治理后正常模式是 4.6%，
      这页有持续动画，会明显更高，要确认不影响音频）

### 7.5 还没做的

- [ ] **FFT 点阵频谱**（esp-dsp 已在依赖里，`drawSpecDot()` 现成）—— 等 VU 这版
      验证通过再上，先确认可视化这条路走得通
- [ ] **方案 C：HID 媒体控制**。再挂一个标准 HID Consumer Control 接口，触摸屏
      就能给主机发播放/暂停/上下一首。HID 是标准协议，主机侧零安装，PC 和手机
      都认——显示不了歌名，但能**控制**播放，实用性可能比显示歌名更高
- [ ] 96k/24bit 支持（全速 USB 放得下，见 §2）
