#include "usb_dac.h"

#if MWR_USB_DAC

// 只包 settings.h（要 I2S_DOUT/BCLK/LRC 三个引脚宏），**不要包 common.h**。
// common.h 里定义的是 audio / pref / webSrv / rtc / i2cBusOne / s_tag 等全局
// 对象的**本体**而不是 extern 声明，它被设计成只由 main.cpp 一个编译单元包含；
// 这里包进来会让每个符号在链接期重复定义（实测报了十几个 multiple definition）。
// settings.h 则是安全的：里面的 const 对象在 C++ 里是内部链接，每个编译单元
// 各有一份，而且那些都在别的板型的 #if 分支里，本板根本不编译。
#include "settings.h"

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <math.h>

// 点阵频谱用。esp-dsp 已经在 managed_components 里（sdkconfig 的
// CONFIG_DSP_OPTIMIZED=y / MAX_FFT_SIZE=4096），只需在 src/CMakeLists.txt 的
// PRIV_REQUIRES 里加上 espressif__esp-dsp 这条边。
#include "esp_dsp.h"

#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include <USB.h>  // USB.begin() —— 光注册接口不会启动 USB 栈，见 usbDacBegin() 末尾

namespace {

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

// v1 只做 16bit 立体声，支持 44.1k 和 48k 两档。
// 44.1k 是绝大多数音乐的原生采样率，48k 是系统默认，两者覆盖日常使用。
// 更高采样率留到时钟同步验证稳定之后再加——先把"不爆音"解决掉。
constexpr uint32_t kSampleRates[] = {44100, 48000};
constexpr uint8_t  kChannels      = 2;
constexpr uint8_t  kBytesPerSample = 2; // 16bit

// 全速 USB 每毫秒一个等时包。异步同步方式下主机可能多发一个采样，所以要留余量。
// (48 + 1) * 2ch * 2byte = 196
constexpr uint16_t kEpOutMaxBytes = 196;

// 环形缓冲：USB 侧每毫秒灌 ~192 字节，I2S 侧按自己的时钟取走。
// 24KB 约等于 128ms @48k/16bit/立体声 —— 足够吸收调度抖动，又不至于让延迟
// 大到影响观感（目标水位是一半，即约 64ms）。
constexpr size_t kRingBytes       = 24 * 1024;
// 目标/预填水位取 1/4 而不是 1/2，是为了压延迟：24KB 满缓冲 @48k/16bit/立体声
// 约 125ms，1/4 约 31ms；再叠加 I2S DMA 的 30ms（6 x 240 帧），总延迟约 60ms。
// 音乐没问题，看视频也还不至于明显唇音不同步；取 1/2 就会到 90ms 以上。
constexpr size_t kRingTargetBytes = kRingBytes / 4;

// --- 频谱快照 ---
// UI 要画点阵频谱，但声卡模式下拿不到 Audio::getSpectrumBands()（那是解码器
// 算的，而这里 PCM 不经过解码器），只能自己算。
//
// 分工：rx 回调里只把样本抄进这个环形快照（纯 memcpy 量级，不做任何计算），
// 真正的 FFT 由 UI 线程在 usbDacGetSpectrum() 里做。音频路径一点都不碰——
// 512 点 FFT 大约 100~200us，放到 core 0 的音频任务上是没必要的风险。
//
// 长度取 2 的幂，写指针用位与回绕（FFT 也要求 2 的幂）。
constexpr size_t kSpecFftSize = 512;

// ---------------------------------------------------------------------------
// 描述符长度
//
// CS_AC 的 _totallen 参数按 TinyUSB 的注释要求：只算它后面那些 Clock Source /
// Unit / Terminal 描述符，**不含 CS_AC 自身**。
// ---------------------------------------------------------------------------
constexpr uint16_t kAcTotalLen =
    TUD_AUDIO20_DESC_CLK_SRC_LEN +
    TUD_AUDIO20_DESC_INPUT_TERM_LEN +
    TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(kChannels) +
    TUD_AUDIO20_DESC_OUTPUT_TERM_LEN;

// 实体 ID。UAC2 里这些是描述符内部的编号，主机按它们寻址控制请求。
constexpr uint8_t kEntityClock       = 0x04;
constexpr uint8_t kEntityInputTerm   = 0x01;
constexpr uint8_t kEntityFeatureUnit = 0x02;
constexpr uint8_t kEntityOutputTerm  = 0x03;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
uint8_t  s_epOut = 0;
uint8_t  s_epFb  = 0;
uint8_t  s_itfAudioControl = 0;

volatile bool     s_mounted   = false;
volatile bool     s_streaming = false;
int16_t         s_specRing[kSpecFftSize];
volatile size_t s_specHead = 0;

volatile uint32_t s_sampleRate = 48000;
// 主机请求的采样率与 I2S 当前实际配置的采样率。只有 i2sTask 会把后者改成前者，
// 见 tud_audio_set_req_entity_cb() 里关于 use-after-free 的注释。
volatile uint32_t s_pendingRate = 0;
uint32_t          s_activeRate  = 0;
volatile bool     s_muted = false;
volatile int16_t  s_volumeDb256 = 0;

// 主机音量的实际作用点。
//
// 2026-09-07：此前 s_volumeDb256 只是被存下来、并在主机来读时原样回报，
// **从来没有乘进 PCM**。表现是 iPhone 上音量滑块完全无反应，而 Android 正常
// ——因为两家策略不同：Android 在主机侧就把音量调好了再送 PCM，iOS 送的是
// 全幅 PCM，指望设备按 Feature Unit 的值自己衰减。
//
// 用 Q16 定点而不是浮点：换算只在音量变化时做一次（powf），采样点上是纯整数
// 乘加。i2sTask 不是 ISR，浮点本来允许，但每个采样都做浮点没有必要。
//
// ⚠️ UAC2 的音量单位是 **1/256 dB**，不是线性刻度。直接把这个 int16 当线性
// 系数用会完全不对（-60dB 会变成 -15360 这种荒谬的"负增益"）。
constexpr int32_t kGainUnity = 65536; // Q16 的 1.0
volatile int32_t  s_gainQ16  = kGainUnity;
int16_t           s_gainSrcDb256 = 0; // 上次换算用的源值，只有 i2sTask 碰
volatile uint32_t s_underruns = 0;
volatile uint32_t s_overruns  = 0;
volatile uint32_t s_framesReceived = 0;

// 左右声道电平（0..255），供 UI 做 VU 和转轮转速。
//
// UAC2 协议里没有任何元数据（歌名/艺术家/封面都不存在），主机能下发的只有
// 采样率/位深/声道/音量/静音。所以"显示正在放什么"做不到——但 PCM 数据本身
// 就在我们手上，把**声音**可视化反而比元数据更直接，而且对任何主机都成立，
// PC / iPhone / Android 都不需要装任何东西。
//
// 用峰值而不是真 RMS：每包才 49 帧，峰值更便宜、视觉上也更像传统 VU 表。
// 弹道是"快起慢落"（attack 立即、decay 每包减 3），和硬件 VU 表的观感一致。
volatile uint8_t  s_vuLeft  = 0;
volatile uint8_t  s_vuRight = 0;

uint8_t* s_ring = nullptr;
volatile size_t s_ringHead = 0; // 写入位置（USB 侧）
volatile size_t s_ringTail = 0; // 读出位置（I2S 侧）

i2s_chan_handle_t s_i2sTx = nullptr;
TaskHandle_t      s_i2sTask = nullptr;
volatile bool     s_i2sRunning = false;

// 预填标志。开流时缓冲被清零，必须先攒够 kRingTargetBytes 再开始送 I2S，
// 否则消费者一上来就把它抽干、水位永远贴着 0 跑——那样除了 I2S DMA 自带的
// 30ms 之外没有任何余量，任何调度抖动都会变成真正的断流。
// 真的干涸之后也退回预填状态重新攒，这是真实 USB 声卡的重同步做法。
volatile bool     s_primed = false;

// SPSC 环形缓冲。生产者只动 head，消费者只动 tail，所以不需要锁。
size_t ringUsed() {
    const size_t head = s_ringHead, tail = s_ringTail;
    return head >= tail ? head - tail : kRingBytes - tail + head;
}
size_t ringFree() { return kRingBytes - ringUsed() - 1; }

void ringWrite(const uint8_t* data, size_t len) {
    if (len > ringFree()) {
        s_overruns = s_overruns + 1;
        return; // 丢掉这一包，而不是覆盖还没播出去的数据
    }
    size_t head = s_ringHead;
    const size_t firstChunk = kRingBytes - head < len ? kRingBytes - head : len;
    memcpy(s_ring + head, data, firstChunk);
    if (firstChunk < len) memcpy(s_ring, data + firstChunk, len - firstChunk);
    s_ringHead = (head + len) % kRingBytes;
}

size_t ringRead(uint8_t* out, size_t len) {
    const size_t available = ringUsed();
    if (len > available) len = available;
    if (!len) return 0;
    size_t tail = s_ringTail;
    const size_t firstChunk = kRingBytes - tail < len ? kRingBytes - tail : len;
    memcpy(out, s_ring + tail, firstChunk);
    if (firstChunk < len) memcpy(out + firstChunk, s_ring, len - firstChunk);
    s_ringTail = (tail + len) % kRingBytes;
    return len;
}

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------
bool i2sStart(uint32_t sampleRate) {
    if (s_i2sTx) {
        i2s_channel_disable(s_i2sTx);
        i2s_del_channel(s_i2sTx);
        s_i2sTx = nullptr;
    }
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 6;
    chanCfg.dma_frame_num = 240;
    chanCfg.auto_clear = true; // 欠载时输出静音而不是重复上一段（重复会是刺耳的嗡声）
    if (i2s_new_channel(&chanCfg, &s_i2sTx, nullptr) != ESP_OK) return false;

    i2s_std_config_t stdCfg = {};
    stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; // 这块板 I2S_MCLK = -1，PCM5100A 靠内部 PLL
    stdCfg.gpio_cfg.bclk = static_cast<gpio_num_t>(I2S_BCLK);
    stdCfg.gpio_cfg.ws   = static_cast<gpio_num_t>(I2S_LRC);
    stdCfg.gpio_cfg.dout = static_cast<gpio_num_t>(I2S_DOUT);
    stdCfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(s_i2sTx, &stdCfg) != ESP_OK) return false;
    if (i2s_channel_enable(s_i2sTx) != ESP_OK) return false;
    return true;
}

// 反馈端点：告诉主机我们的实际消费速率，让它微调发送量。
//
// 主机的 USB 帧时钟和本板晶振必然会漂，不纠正就表现为缓冲单向漂移，最终一端
// 溢出 -> 周期性爆音。这是 DIY USB 声卡翻车的头号原因。
//
// 算法是最简单的比例控制：缓冲比目标浅就请主机发快一点，深了就发慢一点。
// tud_audio_n_fb_set() 收的是 16.16 定点数，整数部分 = 每个 USB 帧应发多少采样。
//
// ⚠️ kFeedbackGain 是拍脑袋定的初值，**必须实测调**——判据是 UI 上的
// underrun/overrun 计数能否长时间保持不变。增益太大会让音高抖动（听感是轻微
// 颤音），太小则缓冲缓慢漂移直到某一端溢出。先取保守值。
// 2026-09-05 实测后上调 1024 -> 8192。原值满偏时只能修正约 0.016%，而两块晶振
// 之间的典型漂移就有 0.01% 量级，等于几乎没有调节余量。8192 给出约 0.08% 的
// 权限，比漂移高一个数量级，同时远小于会引起可闻音高抖动的幅度。
constexpr int32_t kFeedbackGain = 8192;

// 把主机下发的 1/256 dB 换算成 Q16 线性增益。只在值变化时调用。
void updateGainFromVolume() {
    const int16_t db256 = s_volumeDb256;
    if (db256 == s_gainSrcDb256) return;
    s_gainSrcDb256 = db256;

    // UAC2 规定 0x8000 表示负无穷（静音）
    if (db256 == static_cast<int16_t>(0x8000)) {
        s_gainQ16 = 0;
        return;
    }
    const float db  = static_cast<float>(db256) / 256.0f;
    float       lin = powf(10.0f, db / 20.0f);
    if (lin > 1.0f) lin = 1.0f; // 我们声明的范围上限就是 0dB，不做提升
    if (lin < 0.0f) lin = 0.0f;
    s_gainQ16 = static_cast<int32_t>(lin * 65536.0f + 0.5f);
}

void updateFeedback() {
    const uint32_t nominal = (s_sampleRate << 16) / 1000;
    const int32_t  error   = static_cast<int32_t>(ringUsed()) - static_cast<int32_t>(kRingTargetBytes);
    const int32_t  correction = -(error * kFeedbackGain) / static_cast<int32_t>(kRingBytes);
    tud_audio_n_fb_set(0, static_cast<uint32_t>(static_cast<int32_t>(nominal) + correction));
}

void i2sTask(void*) {
    // 本地搬运缓冲。一次搬 4ms 左右，兼顾任务切换开销和延迟。
    constexpr size_t kChunk = 768;
    uint8_t chunk[kChunk];
    uint32_t tick = 0;
    // 连续读空的轮数。见下面 ringRead 返回 0 时的说明：单次读空是正常的，
    // 只有连续空到超过 I2S DMA 撑得住的 30ms 才算真断流。
    constexpr uint32_t kEmptyStreakLimit = 40; // 每轮 1ms
    uint32_t emptyStreak = 0;
    while (s_i2sRunning) {
        // 主机换了采样率就在这里重配 —— 只有本任务碰 s_i2sTx，所以安全。
        // 重配等于换了时钟，缓冲里剩下的旧数据按新速率放会变调，直接丢掉重新预填。
        const uint32_t wanted = s_pendingRate;
        if (wanted && wanted != s_activeRate) {
            if (i2sStart(wanted)) {
                s_activeRate = wanted;
                s_ringTail = s_ringHead; // 丢弃旧速率的残留
                s_primed   = false;      // 退回预填，重新攒到目标水位
            }
        }

        // 每搬几次更新一次反馈值。不需要每帧都算——反馈端点本身是 1ms 一次，
        // 而缓冲水位的变化远比这慢。
        if ((++tick & 0x07) == 0 && s_streaming) updateFeedback();

        // 预填阶段：只等待，不消费，也不计欠载。
        if (!s_primed) {
            if (ringUsed() < kRingTargetBytes) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            s_primed = true;
        }

        const size_t got = ringRead(chunk, kChunk);
        if (!got) {
            // 环形缓冲被抽空是稳态下的**正常现象**，不是断流：i2s_channel_write()
            // 会一直搬到 I2S DMA 塞满才阻塞，本任务是贪婪消费者，必然把 ring 抽干。
            // 真正在护着音频的是 I2S DMA 那 30ms，只要它没跑干就一个采样都不会丢。
            //
            // ⚠️ 所以这里**绝不能**一读空就退回预填。之前那版就是这么写的，
            // 后果是自己制造断流：
            //   I2S DMA 容量 6x240 帧 = 5760 字节 = 30ms
            //   预填目标   kRingTargetBytes = 6144 字节 = 32ms
            // 预填攒够的 6144 字节一上来就被 DMA 吸走 5760，ring 立刻见底 →
            // 判定空 → 停止消费回去攒 32ms → 而 DMA 只撑得住 30ms → 断音。
            // 攒够再放、再被吸干、再断，表现就是声音断续 + 状态在
            // "缓冲中/正在播放"之间反复闪。
            //
            // 正确的判据是**连续空到超过 DMA 撑得住的时间**才算真断流。
            // 这里每轮等 1ms，阈值取 40 轮 = 40ms > DMA 的 30ms。
            if (++emptyStreak >= kEmptyStreakLimit) {
                emptyStreak = 0;
                if (s_streaming && s_primed) {
                    s_underruns = s_underruns + 1;
                    s_primed = false; // 真断流了，退回预填重新攒
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        emptyStreak = 0;
        // 应用主机音量。满音量时整段旁路，保持 bit-perfect 直通——对声卡这种
        // 设备这是该有的取舍（数字衰减必然损失有效位数）。
        updateGainFromVolume();
        const int32_t gain = s_gainQ16;
        if (gain != kGainUnity) {
            int16_t*     pcm = reinterpret_cast<int16_t*>(chunk);
            const size_t n   = got / sizeof(int16_t);
            // gain < 65536 时最大乘积 32768 * 65535 < 2^31，不会溢出。
            for (size_t i = 0; i < n; ++i) {
                pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * gain) >> 16);
            }
        }

        size_t written = 0;
        i2s_channel_write(s_i2sTx, chunk, got, &written, portMAX_DELAY);
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// USB 描述符
// ---------------------------------------------------------------------------
uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf) {
    const uint8_t strIndex = tinyusb_add_string_descriptor("MiniWebRadio DAC");
    s_epOut = tinyusb_get_free_out_endpoint();
    s_epFb  = tinyusb_get_free_in_endpoint();
    TU_VERIFY(s_epOut != 0 && s_epFb != 0);

    s_itfAudioControl = *itf;
    const uint8_t itfAc = *itf;
    const uint8_t itfAs = static_cast<uint8_t>(*itf + 1);

    const uint8_t descriptor[CFG_TUD_AUDIO_FUNC_1_DESC_LEN] = {
        // 接口关联描述符：把下面两个接口捆成一个音频功能
        TUD_AUDIO20_DESC_IAD(itfAc, 2, 0),

        // --- AudioControl 接口（alt 0，无端点）---
        TUD_AUDIO20_DESC_STD_AC(itfAc, 0, strIndex),
        TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER, kAcTotalLen, 0),

        // 时钟源：内部固定时钟，采样率可由主机读写
        TUD_AUDIO20_DESC_CLK_SRC(kEntityClock, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                                 (AUDIO_CTRL_RW << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0x00, 0x00),

        // 输入端：来自 USB 的流
        TUD_AUDIO20_DESC_INPUT_TERM(kEntityInputTerm, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, kEntityClock,
                                    kChannels, AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT,
                                    0x00, 0x0000, 0x00),

        // 功能单元：音量与静音（主机的音量条控制这里）
        TUD_AUDIO20_DESC_FEATURE_UNIT(kEntityFeatureUnit, kEntityInputTerm, 0x00,
                                      (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) |
                                          (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
                                      (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) |
                                          (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
                                      (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) |
                                          (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS)),

        // 输出端：耳机
        TUD_AUDIO20_DESC_OUTPUT_TERM(kEntityOutputTerm, AUDIO_TERM_TYPE_OUT_HEADPHONES, 0x00,
                                     kEntityFeatureUnit, kEntityClock, 0x0000, 0x00),

        // --- AudioStreaming 接口 alt 0：零带宽（主机不播音频时选这个，省 USB 带宽）---
        TUD_AUDIO20_DESC_STD_AS_INT(itfAs, 0x00, 0x00, 0x00),

        // --- alt 1：真正传音频 ---
        TUD_AUDIO20_DESC_STD_AS_INT(itfAs, 0x01, 0x02, 0x00),
        TUD_AUDIO20_DESC_CS_AS_INT(kEntityInputTerm, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM,
                                   kChannels, AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT, 0x00),
        TUD_AUDIO20_DESC_TYPE_I_FORMAT(kBytesPerSample, kBytesPerSample * 8),

        // 等时输出端点。用 ASYNCHRONOUS + 反馈端点：主机时钟和本板晶振必然会漂，
        // 反馈端点让主机按我们的实际消费速率微调发送量，这是避免周期性爆音的正解。
        TUD_AUDIO20_DESC_STD_AS_ISO_EP(s_epOut,
                                       (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                       kEpOutMaxBytes, 0x01),
        TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
                                      AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

        // 反馈端点（IN 方向，主机读我们的实际速率）。
        // wMaxPacketSize 必须是 **3** 而不是 4：USB 2.0 规范 5.12.4.2 规定全速
        // 反馈是 10.14 格式的 3 字节，16.16 的 4 字节那是高速才用的，而 ESP32-S3
        // 的 USB 只有全速。配套还要打 TinyUSB 的补丁——它原本按 UAC 版本而不是
        // 按总线速度选格式，全速 UAC2 会错发 4 字节 16.16，主机解析出来是真值的
        // 4 倍（192 采样/帧），于是不停重新协商流。详见 audiod_fb_send() 里的注释。
        // ⚠️ wMaxPacketSize 这里声明 **4**，而实际发送的是 **3 字节**（见上面和
        // patches/patch_tinyusb_feedback.py）。这个不一致是**故意的**，用来同时
        // 满足两种主机：
        //
        // - 规范侧（USB 2.0 §5.12.4.2）：全速反馈是 10.14 的 3 字节。macOS /
        //   iOS / Android 按这个来，收到 3 字节短包解析正确。等时 IN 端点发短包
        //   是合法的，声明 4 只表示"最多能装 4"。
        // - Windows：usbaudio2.sys 内部一律按 4 字节处理（不实现全速的 10.14
        //   变体，是已知的互操作问题），声明 3 的端点会被它直接跳过，然后报
        //   "could not find a feedback endpoint for an asynchronous data OUT
        //   endpoint"（系统事件日志里的原话），驱动启动失败 → 设备管理器 Code 10、
        //   声音设置里根本不出现。
        //
        // 2026-09-06 那版按规范改成 3，macOS 正常但 Windows 就是这么坏掉的。
        // 现实中大量全速 UAC2 设备都采用"声明 4、发 3"这个折中。
        //
        // 改这里务必两边都复测：Windows 看设备能否正常出现在声音输出列表，
        // macOS/Android 看声音是否仍然连续（反馈格式错会表现为断断续续）。
        TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP((uint8_t)(0x80 | s_epFb), 4, 0x01),
    };

    *itf += 2; // 本功能占用 AC + AS 两个接口
    memcpy(dst, descriptor, CFG_TUD_AUDIO_FUNC_1_DESC_LEN);
    return CFG_TUD_AUDIO_FUNC_1_DESC_LEN;
}

} // namespace

// ---------------------------------------------------------------------------
// TinyUSB 回调
// ---------------------------------------------------------------------------

extern "C" {

// 注意：**不要**定义 tud_mount_cb / tud_umount_cb / tud_suspend_cb /
// tud_resume_cb。arduino-esp32 的 cores/esp32/USB.cpp 已经实现了这四个（它要
// 把它们转成 ArduinoUSB 事件），我们再定义一遍会在链接期 multiple definition。
// 枚举状态改用轮询 tud_mounted()，见 usbDacGetStatus()。

// 主机切换 alt setting：1 = 开始传音频，0 = 停
bool tud_audio_set_itf_cb(uint8_t, tusb_control_request_t const* request) {
    const uint8_t alt = tu_u16_low(request->wValue);
    s_streaming = (alt != 0);
    if (s_streaming) {
        // 每次开流都把缓冲清空重来，避免上一次残留的数据造成起始爆音
        s_ringHead = 0;
        s_ringTail = 0;
        s_primed = false;

        // ⚠️ 必须**在这里**就给出一个有效的反馈值，不能等 i2sTask 慢慢更新。
        //
        // 异步等时传输里，主机切到 alt=1 之后会先读反馈端点，再据此决定每个 USB
        // 帧发多少个采样。反馈值是 0 就等于告诉它"这设备每帧要 0 个采样"——于是
        // 它一个数据包都不发，环形缓冲永远攒不满预填水位，屏幕就一直停在
        // "缓冲中"，频谱也一直不动。
        //
        // Windows 上没暴露出来：usbaudio2.sys 在反馈值无效时会自己回退到标称
        // 速率。macOS / iOS / Android 都严格照反馈值办事。这就是 943c479
        // "真机验证通过"（只在 Windows 上验的）却在另外三种主机上全都没声音的
        // 原因——设备被正确认成声卡，engine 也建起来了，就是一个采样都不来。
        //
        // 开流后的微调仍旧由 i2sTask 里的 updateFeedback() 接管。
        tud_audio_n_fb_set(0, (s_sampleRate << 16) / 1000);
    }
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t, tusb_control_request_t const*) {
    s_streaming = false;
    return true;
}

// 主机写实体控制：静音、音量、采样率
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* request, uint8_t* buf) {
    (void)rhport;
    const audio_control_request_t* req = reinterpret_cast<const audio_control_request_t*>(request);

    if (req->bEntityID == kEntityFeatureUnit) {
        if (req->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
            s_muted = buf[0] != 0;
            return true;
        }
        if (req->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
            s_volumeDb256 = static_cast<int16_t>((buf[1] << 8) | buf[0]);
            return true;
        }
    }
    if (req->bEntityID == kEntityClock && req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        const uint32_t rate = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                              (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
        for (uint32_t supported : kSampleRates) {
            if (rate == supported) {
                s_sampleRate = rate;
                // ⚠️ 这里**不能**直接调 i2sStart()。这个回调跑在 TinyUSB 任务上，
                // 而 i2sStart() 开头就 i2s_channel_disable + i2s_del_channel，
                // 此刻 i2sTask（core 0）很可能正阻塞在 i2s_channel_write() 里
                // 用着同一个句柄 —— 那就是 use-after-free。
                //
                // 之所以在 Windows 上从没炸过：它默认就是 48000，和 s_sampleRate
                // 的初值相同，而且 SET SAM_FREQ 发生在切 alt=1 之前，那时 i2sTask
                // 还停在预填的 vTaskDelay 上，侥幸躲过。iPhone/Apple Music 是
                // 44.1k，必然走到这条路径上来。
                //
                // 改成只记下目标速率，真正的重配由 i2sTask 自己在循环里做 ——
                // 句柄从头到尾只被一个任务碰，竞态从根上就不存在了。
                s_pendingRate = rate;
                return true;
            }
        }
        return false;
    }
    return false;
}

// 主机读实体控制
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* request) {
    (void)rhport;
    const audio_control_request_t* req = reinterpret_cast<const audio_control_request_t*>(request);

    if (req->bEntityID == kEntityClock) {
        if (req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
            if (req->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_4_t cur = {static_cast<int32_t>(s_sampleRate)};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur, sizeof(cur));
            }
            if (req->bRequest == AUDIO_CS_REQ_RANGE) {
                // 报告支持的采样率列表，主机据此决定用哪一档。
                // 用 TinyUSB 的类型宏生成结构，不要自己拼——它是 packed 的，
                // 布局必须和 UAC2 规范逐字节对齐。
                static_assert(TU_ARRAY_SIZE(kSampleRates) == 2, "调整采样率表时同步改这里的 2");
                audio_control_range_4_n_t(2) range;
                range.wNumSubRanges = 2;
                for (size_t i = 0; i < TU_ARRAY_SIZE(kSampleRates); ++i) {
                    range.subrange[i].bMin = static_cast<int32_t>(kSampleRates[i]);
                    range.subrange[i].bMax = static_cast<int32_t>(kSampleRates[i]);
                    range.subrange[i].bRes = 0;
                }
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
            }
        }
        if (req->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_1_t cur = {1}; // 时钟始终有效
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur, sizeof(cur));
        }
    }

    if (req->bEntityID == kEntityFeatureUnit) {
        if (req->bControlSelector == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_1_t cur = {static_cast<int8_t>(s_muted ? 1 : 0)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur, sizeof(cur));
        }
        if (req->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
            if (req->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_2_t cur = {static_cast<int16_t>(s_volumeDb256)};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur, sizeof(cur));
            }
            if (req->bRequest == AUDIO_CS_REQ_RANGE) {
                // -60dB ~ 0dB，步进 0.5dB（UAC2 单位是 1/256 dB）
                audio_control_range_2_n_t(1) range = {};
                range.wNumSubRanges = 1;
                range.subrange[0].bMin = -60 * 256;
                range.subrange[0].bMax = 0;
                range.subrange[0].bRes = 128;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
            }
        }
    }
    return false;
}

// 一包音频到了。只做搬运，不做任何阻塞操作。
//
// ⚠️ 函数名必须是 tud_audio_rx_done_isr。这个回调在新版 TinyUSB 里从
// `tud_audio_rx_done_post_read_cb` 改成了现在这个名字（audio_device.h:251
// 声明、audio_device.c:499 调用），而 audio_compat.h **没有**给旧名提供兼容
// 别名。用旧名的话，它就只是一个没人引用的全局函数：编译零警告、链接正常、
// 运行时驱动一路调用它自己那个什么都不做的弱默认实现，一个包都送不上来。
//
// 2026-09-05 就是栽在这上面：设备在 macOS / iOS / Android 上全都被正确认成
// 声卡（ioreg 里 engine 建好、alt=1、主机确实在推流），可屏幕上 PACKETS 恒为 0。
// 之所以 943c479 那次在 Windows 上真的出过声，是因为当时组件管理器装的还是
// 改名前的 tinyusb —— platformio.ini 里那条依赖写的是 `*`，没锁版本。
//
// ⚠️ 后缀 _isr 是字面意思：它现在在**中断上下文**里被调用（audiod_xfer_isr）。
// 所以这里不能阻塞、不能 printf、也不要用浮点（ESP32 的 FreeRTOS 默认不允许
// 在 ISR 里做浮点运算）。下面全是整数运算和 memcpy，是安全的；FFT 那些浮点
// 活儿都留在 UI 线程的 usbDacGetSpectrum() 里做。
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t, uint8_t, uint8_t) {
    (void)rhport;
    if (!n_bytes_received) return true;
    s_framesReceived = s_framesReceived + 1;

    static uint8_t scratch[kEpOutMaxBytes];
    const uint16_t want = n_bytes_received > sizeof(scratch) ? sizeof(scratch) : n_bytes_received;
    const uint16_t got = tud_audio_read(scratch, want);
    if (!got) return true;

    if (s_muted) {
        // 静音时仍然要往缓冲里灌等量的零，而不是干脆不写——否则 I2S 侧会欠载，
        // 反馈环也会误判成"主机发慢了"，解除静音时反而更容易爆音。
        memset(scratch, 0, got);
    }

    // 峰值电平检测。放在写环形缓冲之前，静音后 scratch 已经是全零，VU 自然归零。
    {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(scratch);
        const size_t frames = got / (kChannels * kBytesPerSample);
        uint16_t peakL = 0, peakR = 0;
        // 顺带把单声道样本抄进频谱快照——和 VU 共用这一次遍历，不再单独扫一遍。
        size_t sh = s_specHead;
        for (size_t i = 0; i < frames; ++i) {
            // 注意用 int32_t 取绝对值：-INT16_MIN 在 int16_t 里会溢出。
            const uint16_t al = static_cast<uint16_t>(pcm[2 * i]     < 0 ? -static_cast<int32_t>(pcm[2 * i])     : pcm[2 * i]);
            const uint16_t ar = static_cast<uint16_t>(pcm[2 * i + 1] < 0 ? -static_cast<int32_t>(pcm[2 * i + 1]) : pcm[2 * i + 1]);
            if (al > peakL) peakL = al;
            if (ar > peakR) peakR = ar;
            // 左右取平均降成单声道。用 int32_t 中转，两个 int16 相加会溢出。
            s_specRing[sh] = static_cast<int16_t>(
                (static_cast<int32_t>(pcm[2 * i]) + static_cast<int32_t>(pcm[2 * i + 1])) / 2);
            sh = (sh + 1) & (kSpecFftSize - 1);
        }
        s_specHead = sh;
        // 快起慢落：新峰值立即顶上去，否则每包（1ms）衰减 3，约 85ms 落到底。
        const uint8_t l8 = static_cast<uint8_t>(peakL >> 7);
        const uint8_t r8 = static_cast<uint8_t>(peakR >> 7);
        const uint8_t curL = s_vuLeft, curR = s_vuRight;
        s_vuLeft  = l8 > curL ? l8 : (curL > 3 ? static_cast<uint8_t>(curL - 3) : 0);
        s_vuRight = r8 > curR ? r8 : (curR > 3 ? static_cast<uint8_t>(curR - 3) : 0);
    }

    ringWrite(scratch, got);
    return true;
}

// 注意：**不要**实现 tud_audio_feedback_params_cb()。
// 驱动的弱默认实现就是 AUDIO_FEEDBACK_METHOD_DISABLED，正是我们要的——
// 内置的 FIFO_COUNT 方法看的是 TinyUSB 自己的 EP FIFO，而我们在
// tud_audio_rx_done_post_read_cb() 里已经把它立刻搬空了，水位恒为 0，
// 用那个方法算出来的反馈值是错的。反馈由 updateFeedback() 自己算（见下）。
//
// 另注：tud_audio_feedback_interval_isr() 是**驱动提供给应用调用**的函数，
// 不是应用要实现的回调，别去定义它（会和驱动自身的定义符号冲突）。

} // extern "C"

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

bool usbDacBegin() {
    s_ring = static_cast<uint8_t*>(heap_caps_malloc(kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_ring) {
        printf("[USBDAC] ring buffer alloc failed\n");
        return false;
    }
    s_ringHead = 0;
        s_ringTail = 0;

    if (!i2sStart(s_sampleRate)) {
        printf("[USBDAC] I2S init failed\n");
        return false;
    }
    s_activeRate = s_sampleRate;

    s_i2sRunning = true;
    // 钉在 core 0：和音频解码任务同一个核（core 1 留给 Arduino loop + LVGL）。
    // 优先级 5 高于 LVGL，欠载比掉帧严重得多。
    if (xTaskCreatePinnedToCore(i2sTask, "usbDacI2s", 4096, nullptr, 5, &s_i2sTask, 0) != pdPASS) {
        printf("[USBDAC] I2S task create failed\n");
        s_i2sRunning = false;
        return false;
    }

    if (tinyusb_enable_interface2(USB_INTERFACE_CUSTOM, CFG_TUD_AUDIO_FUNC_1_DESC_LEN, loadDescriptor, true) != ESP_OK) {
        printf("[USBDAC] tinyusb_enable_interface failed\n");
        return false;
    }

    // ⚠️ tinyusb_enable_interface2() 只是把描述符回调登记进去，**不会启动 USB
    // 栈**。必须再调 USB.begin()，否则主机侧什么都看不到——第一版就漏了这一步，
    // 表现是屏幕正常进了声卡页、但 Windows 的声音设备列表里毫无反应。
    // MSC 模式里对应的是 usbStoragePrepareMsc() 末尾那段（main.cpp:1792）。
    USB.manufacturerName("ESP32-S3 HiFi");
    USB.productName("MiniWebRadio DAC");
    if (!USB.begin()) {
        printf("[USBDAC] USB.begin failed\n");
        return false;
    }
    return true;
}

void usbDacLoop() {
    // 搬运由 i2sTask 承担，这里目前无事可做。保留这个入口是为了后续可能加入的
    // 周期性维护（比如反馈环的长期漂移统计）。
}

void usbDacGetStatus(UsbDacStatus* out) {
    if (!out) return;
    // 枚举状态直接问 TinyUSB —— tud_mount_cb 那条路被 arduino-esp32 占用了
    // （见上面 extern "C" 块里的说明）。
    const bool mounted = tud_mounted();
    s_mounted = mounted;
    out->mounted          = mounted;
    if (!mounted) s_streaming = false;
    out->streaming        = s_streaming;
    out->sampleRateHz     = s_sampleRate;
    out->bitsPerSample    = kBytesPerSample * 8;
    out->channels         = kChannels;
    out->muted            = s_muted;
    out->volumeDb256      = s_volumeDb256;
    if (!s_streaming) { s_vuLeft = 0; s_vuRight = 0; }
    out->vuLeft           = s_vuLeft;
    out->vuRight          = s_vuRight;
    out->primed           = s_primed;
    out->bufferLevelBytes = ringUsed();
    out->bufferCapacity   = kRingBytes;
    out->bufferUnderruns  = s_underruns;
    out->bufferOverruns   = s_overruns;
    out->framesReceived   = s_framesReceived;
}

void usbDacGetSpectrum(uint8_t out[6]) {
    // FFT 工作区。放函数内 static 而不是文件级，是为了让"只有真正画频谱时才
    // 占这 6KB"这件事留在一处——声卡模式之外这个函数永远不会被调到。
    // dsps_fft2r_fc32 的优化版要求 16 字节对齐。
    static bool  ready = false;
    static float window[kSpecFftSize];
    static float work[kSpecFftSize * 2] __attribute__((aligned(16)));
    static float smooth[6] = {0, 0, 0, 0, 0, 0};

    if (!s_streaming || s_muted) {
        // 没在传或静音时让它平滑落下去，而不是"啪"一下全灭。
        for (uint8_t b = 0; b < 6; ++b) {
            smooth[b] *= 0.7f;
            out[b] = static_cast<uint8_t>(smooth[b]);
        }
        return;
    }

    if (!ready) {
        if (dsps_fft2r_init_fc32(nullptr, kSpecFftSize) != ESP_OK) {
            memset(out, 0, 6);
            return;
        }
        dsps_wind_hann_f32(window, kSpecFftSize);
        ready = true;
    }

    // 从写指针往回取最近 kSpecFftSize 个样本。这里和 rx 回调是无锁并发，
    // 允许读到撕裂的一两个样本——这是拿来画图的，不是拿来出声的，
    // 为它上锁反而会拖慢音频回调。
    const size_t head = s_specHead;
    for (size_t i = 0; i < kSpecFftSize; ++i) {
        const int16_t sample = s_specRing[(head + i) & (kSpecFftSize - 1)];
        work[i * 2]     = (static_cast<float>(sample) / 32768.0f) * window[i];
        work[i * 2 + 1] = 0.0f; // 实信号，虚部补零
    }

    dsps_fft2r_fc32(work, kSpecFftSize);
    dsps_bit_rev_fc32(work, kSpecFftSize);

    // 对数分段，不是等宽——等宽的话低音全挤在第一格，人耳对频率本来就是对数感知。
    static const uint16_t kEdgeHz[7] = {30, 150, 400, 1000, 2500, 6000, 16000};
    const uint32_t rate = s_sampleRate ? s_sampleRate : 48000;
    const size_t   maxBin = kSpecFftSize / 2 - 1;

    for (uint8_t b = 0; b < 6; ++b) {
        size_t lo = static_cast<size_t>(static_cast<uint64_t>(kEdgeHz[b])     * kSpecFftSize / rate);
        size_t hi = static_cast<size_t>(static_cast<uint64_t>(kEdgeHz[b + 1]) * kSpecFftSize / rate);
        if (lo < 1) lo = 1;          // bin 0 是直流分量，永远跳过
        if (hi > maxBin) hi = maxBin;
        if (hi <= lo) hi = lo + 1;

        // 取带内峰值而不是平均：频谱分析仪的传统做法，视觉上更活跃，
        // 平均会把一个尖峰摊平到整段里看不见。
        // ⚠️ 必须归一化再算 dB。N 点 FFT 对幅度 A 的正弦波，bin 峰值是 A*N/2，
        // 加了 Hann 窗（相干增益 0.5）之后是 A*N/4 —— 直接拿这个原始值去算 dB，
        // 幅度 0.5 的信号能算出 +36dB，映射完远超 255，于是**所有频段恒满格**，
        // 28 列全部点满 11 行；逐列脏检查又发现"和上次一样"而整列跳过重绘，
        // 屏幕上看起来就是频谱死住不动。
        // 系数 4/N = (2/N 的 FFT 归一化) x (2 倍的 Hann 窗增益补偿)，
        // 还原之后 peak 就是原始信号幅度（0..1）。
        constexpr float kMagScale = 4.0f / static_cast<float>(kSpecFftSize);
        float peak = 0.0f;
        for (size_t i = lo; i <= hi && i <= maxBin; ++i) {
            const float re = work[i * 2];
            const float im = work[i * 2 + 1];
            const float mag = sqrtf(re * re + im * im) * kMagScale;
            if (mag > peak) peak = mag;
        }

        // 转 dB 再映射到 0..255。线性幅度直接画的话，音乐里绝大多数时间
        // 都贴在最底下几格——动态范围有 60dB 以上，线性刻度根本展不开。
        const float db = 20.0f * log10f(peak + 1e-9f);
        // -60dB..+6dB -> 0..255。上限特意留 6dB 余量而不是顶到 0dB：满刻度信号
        // 落在约九成高度上，峰值还有地方可去，不会一直撞天花板。
        float v = (db + 60.0f) * (255.0f / 66.0f);
        if (v < 0.0f)   v = 0.0f;
        if (v > 255.0f) v = 255.0f;

        // 快起慢落，和 VU 同一条弹道原则（见 rx 回调里的峰值检测）。
        smooth[b] = v > smooth[b] ? v : (smooth[b] * 0.72f + v * 0.28f);
        out[b] = static_cast<uint8_t>(smooth[b]);
    }
}

#endif // MWR_USB_DAC
