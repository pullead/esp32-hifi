// USB Audio Class 2.0 扬声器模式 —— 把这块板当成电脑/手机的外挂声卡。
//
// 形态：播放器的第三个独立模式，和 USB MSC（U盘）模式并列、互斥。
// 进入方式沿用 MSC 那一套已验证的机制：写 NVS 标志 -> 重启 -> 开机分支进入。
// 必须重启是硬性限制，不是设计选择：main.cpp 里那条注释写明了这块板运行时调
// USB.begin() 会触发 ESP_RST_USB 复位，TinyUSB 只能在开机时启动。
//
// ⚠️ 进入本模式后 USB-C 口被 TinyUSB 占用，**USB-Serial/JTAG 控制台会消失**，
// printf 抓不到任何东西。所以本模块的诊断计数器必须全部暴露给 UI，屏幕是这个
// 模式下唯一的仪表盘（见 UsbDacStatus）。这是 esp_lcd 迁移那次的教训：新外设的
// 诊断要和第一版代码一起写，不能等出问题再补。
//
// 信号链：PC/手机 --USB等时传输--> ESP32-S3 --I2S--> PCM5100A --> 3.5mm
//
// 已知的硬性上限：ESP32-S3 的 USB-OTG 只有全速（12Mbps），等时端点每毫秒最多
// 1023 字节。48kHz/16bit 立体声 = 192 字节/ms，96kHz/24bit = 576 字节/ms，都放得下；
// 192kHz/24bit 需要 1152 字节/ms，**超上限，这块芯片做不到**。

#pragma once

#include <stdbool.h>
#include <stdint.h>

// 本模式的诊断快照。UI 每次刷新读一份显示出来。
//
// bufferUnderruns / bufferOverruns 是判断时钟漂移的核心指标：主机的 USB 帧时钟
// 和本板 I2S 的晶振必然会漂，不处理就表现为周期性爆音。欠载说明 I2S 消费快于
// USB 供给，溢出说明反过来。稳定运行时两者都应该长时间不涨。
struct UsbDacStatus {
    bool     mounted;          // 已被主机枚举
    bool     streaming;        // 主机选中了 alt setting 1，正在传音频
    uint32_t sampleRateHz;     // 主机当前请求的采样率
    uint8_t  bitsPerSample;
    uint8_t  channels;
    bool     muted;            // 主机侧 Feature Unit 下发的静音
    int16_t  volumeDb256;      // 主机侧音量，1/256 dB 为单位（UAC2 规定）
    uint8_t  vuLeft;           // 左声道峰值电平 0..255（供 VU / 转轮转速用）
    uint8_t  vuRight;
    bool     primed;           // 已攒够预填水位、正在正常出声（false = 还在攒）
    uint32_t bufferLevelBytes; // 环形缓冲当前水位
    uint32_t bufferCapacity;
    uint32_t bufferUnderruns;  // 见上面关于时钟漂移的说明
    uint32_t bufferOverruns;
    uint32_t framesReceived;   // 收到的 USB 音频包数，用来确认数据真的在流动
};

// 开机时调用，仅在 NVS 标志为真的分支里。返回 false 表示初始化失败，
// 调用方应当清标志并重启回正常模式（和 MSC 模式同样的兜底）。
bool usbDacBegin();

// 主循环里调用，喂 I2S 并更新统计。
void usbDacLoop();

void usbDacGetStatus(UsbDacStatus* out);
