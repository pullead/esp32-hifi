#include "waveshare_lvgl_port.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <algorithm>
#include <cmath>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

// Owned by MiniWebRadio. Reuse it instead of starting Arduino's default Wire
// object on the same I2C controller.
extern TwoWire i2cBusOne;

namespace {
constexpr int kWidth = 320;
constexpr int kHeight = 170;
// Audio and display smoothness both take priority over WiFi at boot (see
// setupLvglRuntime()'s comment in main.cpp) -- this stays at full size
// rather than being shrunk to free up internal RAM for WiFi's own
// esp_wifi_init(). WiFi is deliberately the one deferred/best-effort here,
// not this.
// 2026-08-06/07: 20 -> 40 -> 80 行，两个 buffer 共约 100KB，都在 PSRAM 里
// （见下面 heap_caps_malloc 的 MALLOC_CAP_SPIRAM），PSRAM 剩余空间很宽裕。
// 这个改动不会让单次刷新传输的总字节数变少（传输是同步阻塞的，总量不
// 变），只是把覆盖同一块屏幕区域所需的 flush() 调用次数减少，省掉相应的
// 函数调用/LVGL 调度开销——80 行已经接近屏幕高度(170)的一半，再往上加边
// 际收益会更小。
// 2026-09-04：80 -> 16，配合缓冲从 PSRAM 移回内部 DMA 内存（见下面
// begin() 里的分配处）。80 行 x2 在 PSRAM 里没问题，但换成内部 RAM 就是
// 100KB，这块板绝对给不起。16 行 x2 = 20KB。
//
// 这个数字变小**不会**让传输总字节数变多——覆盖同一块脏区域的数据量不
// 变，只是拆成更多次 flush()。而且现在 flush 是异步的（提交完就返回，DMA
// 在后台搬），多几次调用的开销远小于以前同步阻塞时代。
constexpr int kRowsPerBuffer = 16;
// 这个常量之前一直是这条 LVGL/Arduino_GFX 路径真正生效的 LCD SPI 频率——
// settings.h 里的 TFT_FREQUENCY 只给旧的非 LVGL 驱动路径用，那条路径在
// MWR_LVGL_UI 打开时被 setup() 里的提前 return 挡住，根本不会执行到，所以
// 改 TFT_FREQUENCY 对这条实际在跑的路径没有任何效果。40 -> 80MHz 已经实测
// 稳定；现在测试 100MHz，其余配置（Flash QIO 40MHz / PSRAM 120MHz / 80行
// buffer）保持不动，单独隔离这一个变量。
// 2026-09-03: 100MHz -> 80MHz。ESP32-S3 的 GPSPI 硬件上限就是 80MHz，
// esp_lcd_new_panel_io_spi() 会校验 pclk_hz 并对超限值直接返回
// ESP_ERR_INVALID_ARG（实测：填 100MHz 时 begin() 卡在这一步，整个显示
// 驱动没建起来，屏幕不刷新）。
//
// 顺带纠正一条既有结论：此前 Arduino_GFX 路径填 100MHz "实测稳定"是假象。
// Arduino 的 SPIClass 只按分频系数取最接近的可用档位，不做上限校验，所以
// 那时候实际跑的本来就是 80MHz，从来没真到过 100MHz——换成会校验的
// esp_lcd 之后这个问题才暴露出来。也就是说这里从 100 改回 80 并不会损失
// 任何实际速度，只是把配置写成了硬件真正能做到的值。
constexpr uint32_t kTftSpiHz = 80000000;
constexpr uint8_t kTouchAddress = 0x15;
constexpr uint8_t kImuAddress = 0x6B;
constexpr bool kLcdSelfTest = false;
constexpr bool kBootColorFlash = true;
constexpr uint8_t kDisplayRotationNormal = 3;
constexpr uint8_t kDisplayRotationFlipped = 1;
constexpr bool kInvertTouchX = false;
constexpr bool kInvertTouchY = false;
constexpr uint16_t kEdgeBackPx = 54;
// Vertical edge-swipe zones for the quick-settings panel: must START within
// this many px of the top/bottom edge, same safety pattern as kEdgeBackPx --
// a prior unconstrained vertical-gesture branch here swallowed ordinary
// taps (see updateGesture()'s history comment), so these stay edge-gated.
constexpr uint16_t kEdgeTopPx = 24; // matches the status bar height
constexpr uint16_t kEdgeBottomPx = 24;
constexpr uint16_t kGestureMinPx = 38;
constexpr uint16_t kGesturePreclaimPx = 10;
constexpr uint16_t kGestureCrossMaxPx = 86;
constexpr uint8_t kTouchReleaseGraceReads = 4;
constexpr uint32_t kGestureMaxMs = 1400;
constexpr uint32_t kPostGestureClickSuppressMs = 450;
constexpr uint32_t kImuPollMs = 50;
constexpr uint32_t kOrientationDebounceMs = 1200;
constexpr int32_t kImuMinHorizontalMagnitudeSquared = 4800L * 4800L;
constexpr int32_t kImuFlipDotThreshold = -4200L * 4200L;
constexpr uint16_t kGyroCalibSamples = 24;
constexpr float kGyroLsbPerDps = 64.0f; // QMI8658 gyro configured to 512 dps full-scale.
constexpr float kGyroActiveDps = 35.0f;
constexpr float kGyroFlipDegrees = 145.0f;

constexpr int LCD_RST = 9;
constexpr int LCD_CLK = 10;
constexpr int LCD_DC = 11;
constexpr int LCD_CS = 12;
constexpr int LCD_DIN = 13;
constexpr int LCD_BL = 14;
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kWhite = 0xFFFF;

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    i2cBusOne.beginTransmission(address);
    i2cBusOne.write(reg);
    i2cBusOne.write(value);
    return i2cBusOne.endTransmission() == 0;
}

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t len) {
    i2cBusOne.beginTransmission(address);
    i2cBusOne.write(reg);
    if (i2cBusOne.endTransmission(false) != 0) return false;
    if (i2cBusOne.requestFrom(address, len) != len) return false;
    for (size_t i = 0; i < len; ++i) data[i] = i2cBusOne.read();
    return true;
}

// Both stay null since the 2026-09-03 esp_lcd migration -- the panel is now
// driven by s_panel/s_panelIo below. Every remaining s_gfx user is null-
// guarded and therefore a no-op; kept (rather than deleted along with the
// Arduino_GFX dependency) only until the migration has proven stable on
// hardware, so a revert stays a one-file change. What that costs today:
//   - drawFallbackHome(): the pre-LVGL placeholder screen no longer draws.
//     Harmless -- LVGL takes over within a second of boot either way.
//   - runSelfTest(): already dead (kLcdSelfTest == false).
//   - applyDisplayRotation() / pollImuOrientation(): already inert (IMU
//     auto-rotate was removed by request 2026-07-24); orientation is now
//     fixed at init via esp_lcd_panel_swap_xy/mirror.
// s_bus is gone entirely -- nothing referenced it once Arduino_HWSPI was
// dropped. s_gfx stays only because the null-guarded no-op users listed
// above still name it.
Arduino_GFX* s_gfx = nullptr;
uint32_t s_flushCount = 0;
uint32_t s_lastFallbackDraw = 0;

// 2026-09-03: 显示链路迁移到官方 esp_lcd（方案见 docs/PLAN_esp_lcd_migration.md）。
// 目的是把 flush 从"同步阻塞直到传完"改成"提交给 DMA 后立刻返回、传输完成中断
// 里才通知 LVGL"，这样 LVGL 的双缓冲才真正吃到"渲染下一块 / 传输上一块"的并行。
//
// SPI host：ESP32-S3 上 Arduino 的 FSPI=0 对应 SPI2 总线，全局 SPI 对象就是
// SPIClass SPI(FSPI)，而 Arduino_HWSPI 默认用它——所以此前能正常出画面的这条
// 路径实际跑在 SPI2 上。注意 common.h:100 那条"必须用 SPI3、FSPI 收不到数据"的
// 注释是旧 tftLib 路径的经验，不适用于这里。IDF 的枚举编号和 Arduino 不同：
// SPI2_HOST 在 IDF 里是 1，别按 Arduino 的 FSPI=0 去填。
constexpr spi_host_device_t kLcdSpiHost = SPI2_HOST;
esp_lcd_panel_io_handle_t s_panelIo = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;

// Dropped-transfer accounting. A transfer esp_lcd refuses is otherwise
// completely silent, and the only visible effect is that region of the screen
// keeping its previous content -- which is exactly how the migration's
// garbling presented, so it stays instrumented.
//
// Debugging note for anything added here later: this board re-enumerates USB
// when the app starts, so init-time printf is unobservable (a reset-and-
// capture loses the connection, a capture without reset joins too late).
// Report from the periodic [PERF] tick instead.
esp_err_t s_lastDrawErr = ESP_OK;
uint32_t s_drawErrCount = 0;
uint32_t s_transDoneCount = 0;

// Runs in ISR context from the SPI DMA completion interrupt. Must stay tiny
// and must not call anything that blocks -- lv_disp_flush_ready() only flips
// a flag, which is safe here (this is the pattern esp_lcd's own LVGL examples
// use). Returning false means "no higher-priority task was woken".
bool IRAM_ATTR onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* userCtx) {
    ++s_transDoneCount;
    auto* driver = static_cast<lv_disp_drv_t*>(userCtx);
    if (driver) lv_disp_flush_ready(driver);
    return false;
}

// 2026-08-07: 临时的串口性能基线统计，配合 LV_USE_PERF_MONITOR 一起看——
// 那个只能在屏幕上看，这里额外从串口打印一份方便留存/对比，不依赖人盯着
// 屏幕读数。每秒打印一次：这一秒里 flush() 被调用了几次（近似"这一秒画了
// 多少块脏区域"，不是标准意义的整帧 FPS，但能反映刷新频率）、
// lv_timer_handler() 总共被调用了几次、这些调用累计占用了多少 CPU 时间
// （用来看渲染+同步阻塞发送到底占了这一秒里多大比例）、以及单次调用里最
// 慢的一次耗时（用来揪卡顿尖峰）。测完记得删掉/关掉，不是长期要留的代码。
uint32_t s_perfWindowStartMs = 0;
uint32_t s_perfFlushAtWindowStart = 0;
uint32_t s_perfTimerHandlerCalls = 0;
uint32_t s_perfTimerHandlerBusyUs = 0;
uint32_t s_perfTimerHandlerMaxUs = 0;

void drawFallbackHome() {
    if (!s_gfx) return;
    s_gfx->fillScreen(kBlack);
    s_gfx->drawRect(0, 0, kWidth, kHeight, kCyan);
    s_gfx->drawFastHLine(0, 28, kWidth, kCyan);
    s_gfx->setTextSize(2);
    s_gfx->setTextColor(kWhite, kBlack);
    s_gfx->setCursor(12, 7);
    s_gfx->print("NOW PLAYING");
    s_gfx->setTextSize(1);
    s_gfx->setCursor(248, 10);
    s_gfx->print("OFFLINE");

    const char* labels[] = {"PLAY", "LOCAL", "RADIO", "SAVED", "SET"};
    for (uint8_t i = 0; i < 5; ++i) {
        const int16_t x = 10 + i * 62;
        const uint16_t border = i == 0 ? kGreen : kCyan;
        s_gfx->drawRoundRect(x, 44, 54, 84, 7, border);
        s_gfx->drawRoundRect(x + 1, 45, 52, 82, 7, border);
        s_gfx->setTextSize(2);
        s_gfx->setTextColor(i == 0 ? kGreen : kCyan, kBlack);
        s_gfx->setCursor(x + 20, 65);
        s_gfx->print(i == 0 ? ">" : (i == 1 ? "M" : (i == 2 ? "R" : (i == 3 ? "*" : "="))));
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(kWhite, kBlack);
        s_gfx->setCursor(x + 12, 103);
        s_gfx->print(labels[i]);
    }
    s_gfx->setTextColor(kCyan, kBlack);
    s_gfx->setCursor(82, 151);
    s_gfx->print("MINIWEBRADIO HIFI PLAYER");
}
} // namespace

WaveshareLvglPort* WaveshareLvglPort::s_instance = nullptr;

bool WaveshareLvglPort::initPanel() {
    printf("[LCD] init Arduino_GFX ST7789 320x170\n");
    // This board's backlight driver is active-low (every prior version of
    // this code only ever wrote LOW here, and the panel lights up) --
    // setBacklightPercent() accounts for that when it takes over via PWM.
    constexpr uint32_t kBacklightPwmHz = 5000;
    constexpr uint8_t kBacklightPwmBits = 8;
    if (ledcAttach(LCD_BL, kBacklightPwmHz, kBacklightPwmBits)) {
        setBacklightPercent(m_backlightPercent);
    } else {
        // PWM attach failed for some reason -- fall back to the plain
        // on/off behavior this pin always had rather than leaving it
        // floating (no backlight at all).
        pinMode(LCD_BL, OUTPUT);
        digitalWrite(LCD_BL, LOW);
    }

    // ---- SPI bus -------------------------------------------------------
    // max_transfer_sz must cover the largest single draw_bitmap we ever
    // issue: one full draw buffer (320 * kRowsPerBuffer * 2 bytes).
    spi_bus_config_t busCfg = {};
    busCfg.sclk_io_num = LCD_CLK;
    busCfg.mosi_io_num = LCD_DIN;
    busCfg.miso_io_num = -1;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
    busCfg.max_transfer_sz = kWidth * kRowsPerBuffer * static_cast<int>(sizeof(lv_color_t));
    if (spi_bus_initialize(kLcdSpiHost, &busCfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        printf("[LCD] spi_bus_initialize failed\n");
        return false;
    }

    // ---- panel IO ------------------------------------------------------
    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.cs_gpio_num = LCD_CS;
    ioCfg.dc_gpio_num = LCD_DC;
    ioCfg.spi_mode = 0;
    ioCfg.pclk_hz = kTftSpiHz;
    ioCfg.trans_queue_depth = 10;
    ioCfg.lcd_cmd_bits = 8;
    ioCfg.lcd_param_bits = 8;
    // on_color_trans_done is registered later, in begin(), because its
    // user_ctx has to be &m_displayDriver and that isn't initialized until
    // after lv_disp_drv_init().
    // esp_lcd_spi_bus_handle_t is just an int holding the host number.
    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &ioCfg, &s_panelIo) != ESP_OK) {
        printf("[LCD] esp_lcd_new_panel_io_spi failed\n");
        return false;
    }

    // ---- ST7789 panel --------------------------------------------------
    esp_lcd_panel_dev_config_t panelCfg = {};
    panelCfg.reset_gpio_num = LCD_RST;
    panelCfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panelCfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(s_panelIo, &panelCfg, &s_panel) != ESP_OK) {
        printf("[LCD] esp_lcd_new_panel_st7789 failed\n");
        return false;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    // This board's panel wants INVON -- previously expressed as Arduino_GFX's
    // ips=true. Photos of the light-mode retheme showed a clean inversion
    // signature (white bg -> near-black, purple -> olive, teal -> pink), which
    // stayed invisible on the old dark theme by coincidence.
    esp_lcd_panel_invert_color(s_panel, true);
    // Panel is physically 170x320 portrait; the UI is 320x170 landscape.
    // swap_xy turns the axes, mirror fixes the resulting handedness, and the
    // 35px RAM offset (portrait x) follows the swap onto the y axis.
    // All three are empirical -- verify on hardware, see the plan's §6.
    // 这三个参数不是推导的，是从此前能正常工作的 Arduino_GFX 配置反推出来
    // 的（旧代码：Arduino_ST7789(..., 170, 320, 35, 0, 35, 0) + setRotation(3)）：
    //   Arduino_ST7789::setRotation 的 case 3 => MADCTL = MY | MV | RGB
    //     MV  -> swap_xy(true)
    //     MX  没有 -> mirror_x = false
    //     MY  有   -> mirror_y = true
    //   Arduino_TFT::setRotation 的 case 3 => _xStart = ROW_OFFSET2 = 0
    //                                        _yStart = COL_OFFSET1 = 35
    //     -> set_gap(0, 35)
    // 2026-09-04：mirror 的两个参数最初被我写反成 (true, false)，画面方向/
    // 位置全错，连带触摸也"没反应"——因为显示位置和 LVGL 控件的逻辑坐标对
    // 不上，点哪都命不中。
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_set_gap(s_panel, 0, 35);
    esp_lcd_panel_disp_on_off(s_panel, true);

    printf("[LCD] esp_lcd ST7789 panel ready (SPI%d, %lu Hz)\n", static_cast<int>(kLcdSpiHost) + 1,
           static_cast<unsigned long>(kTftSpiHz));
    m_panel = s_panel;
    return true;
}

void WaveshareLvglPort::setBacklightPercent(uint8_t percent) {
    if (percent > 100) percent = 100;
    m_backlightPercent = percent;
    // Active-low driver (see initPanel()'s comment): duty is the fraction
    // of the PWM period the pin is HIGH, i.e. backlight OFF. Full
    // brightness (100%) needs the pin LOW the whole period -> duty 0.
    constexpr uint32_t kMaxDuty = 255; // matches initPanel()'s 8-bit resolution
    const uint32_t dutyHigh = kMaxDuty - (static_cast<uint32_t>(percent) * kMaxDuty / 100);
    ledcWrite(LCD_BL, dutyHigh);
}

bool WaveshareLvglPort::begin() {
    printf("[LVGL] port begin\n");
    if (!initPanel()) return false;
    if (kLcdSelfTest) runSelfTest();
    drawFallbackHome();
    i2cBusOne.setClock(400000);
    m_imuReady = initImu();

    lv_init();
    // Rounded up to a whole number of 64-byte cache lines for the same
    // writeback reason as the alignment below (a partial trailing line is
    // exactly where stale-data artifacts like to appear). 320 * 2 bytes per
    // row is already a multiple of 64, so this is currently a no-op -- it's
    // here so a future kWidth/kRowsPerBuffer change can't silently break it.
    const size_t bufferBytes = ((kWidth * kRowsPerBuffer * sizeof(lv_color_t)) + 63u) & ~static_cast<size_t>(63u);
    // INTERNAL DMA memory, not PSRAM. This is forced by how the SPI master
    // driver handles a tx buffer it can't DMA from directly
    // (esp_driver_spi/src/gpspi/spi_master.c, ~line 1163):
    //
    //     if (!esp_ptr_dma_capable(send_ptr) || tx_unaligned) {
    //         temp = heap_caps_aligned_alloc(alignment, tx_byte_len, MALLOC_CAP_DMA);
    //         if (temp == NULL) { goto clean_up; }   // -> ESP_ERR_NO_MEM
    //         memcpy(temp, send_ptr, ...);
    //     }
    //
    // esp_ptr_dma_capable() is false for PSRAM unconditionally (it means
    // "internal DMA-capable", not "the GDMA can reach it"), so with the
    // buffers in PSRAM the driver allocated a fresh internal bounce buffer
    // *the size of the whole transfer* and memcpy'd into it on every single
    // flush -- ~150 times a second. Consequences, all observed:
    //   - transient 51KB internal allocations; when one failed we got
    //     ESP_ERR_NO_MEM and the transfer was silently dropped, leaving that
    //     screen region showing stale content (the "花屏" patches).
    //   - the PSRAM->internal copy the migration was supposed to remove was
    //     still happening, just relocated into the driver.
    // Aligning the PSRAM allocation didn't help and couldn't have: the
    // esp_ptr_dma_capable() half of that condition is false either way.
    //
    // Internal DMA memory takes the zero-copy path instead. At 16 rows the
    // two buffers cost 20KB, which is *less* internal-RAM pressure than the
    // 51KB transient allocations it replaces -- and internal RAM here is
    // genuinely scarce (see setupLvglRuntime() in main.cpp; a 48KB increase
    // for cache sizing bricked boot on 2026-09-03).
    m_bufferA = static_cast<lv_color_t*>(heap_caps_aligned_alloc(64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    m_bufferB = static_cast<lv_color_t*>(heap_caps_aligned_alloc(64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    printf("[LVGL] draw buffers: A=%p B=%p (internal free=%u, psram free=%u)\n", m_bufferA, m_bufferB,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!m_bufferA || !m_bufferB) {
        printf("[LVGL] draw buffer alloc failed\n");
        return false;
    }

    lv_disp_draw_buf_init(&m_drawBuffer, m_bufferA, m_bufferB, kWidth * kRowsPerBuffer);
    lv_disp_drv_init(&m_displayDriver);
    m_displayDriver.hor_res = kWidth;
    m_displayDriver.ver_res = kHeight;
    m_displayDriver.draw_buf = &m_drawBuffer;
    m_displayDriver.flush_cb = flush;
    m_displayDriver.user_data = m_panel;
    m_display = lv_disp_drv_register(&m_displayDriver);
    if (!m_display) {
        printf("[LVGL] display register failed\n");
        return false;
    }
    lv_disp_set_default(m_display);

    // Async completion: flush() queues the DMA transfer and returns; this
    // callback is what actually tells LVGL the buffer is free again. Has to
    // be registered after lv_disp_drv_register() so user_ctx points at a
    // fully initialized driver.
    esp_lcd_panel_io_callbacks_t ioCbs = {};
    ioCbs.on_color_trans_done = onColorTransDone;
    if (esp_lcd_panel_io_register_event_callbacks(s_panelIo, &ioCbs, &m_displayDriver) != ESP_OK) {
        printf("[LVGL] esp_lcd callback register failed\n");
        return false;
    }

    lv_indev_drv_init(&m_touchDriver);
    m_touchDriver.type = LV_INDEV_TYPE_POINTER;
    m_touchDriver.read_cb = readTouch;
    // 2026-08-07: 16 -> 8（LVGL 默认是10）。这个值是"手指移动超过多少像素
    // 才判定为滑动，而不是点击"的阈值——16 比默认值还大，导致轻/快速的一
    // 划，手指还没挪够16px就抬起了，会被误判成点击，正好对应"明明只是滑
    // 动却触发了点击"的反馈。调小之后滑动更容易被正确识别，代价是极短距
    // 离的滑动手势会更容易被当成滑动而不是点击（可以接受，列表这类场景
    // 本来就应该优先滑动）。
    m_touchDriver.scroll_limit = 8;
    // Lower = slower slow-down = more momentum glide after release (LVGL's
    // own doc comment on this field: "Greater value means faster
    // slow-down"). Was 18; dropped toward LVGL's own ~10 default so a flick
    // on the local-music list actually glides and decelerates instead of
    // stopping the instant the finger lifts.
    m_touchDriver.scroll_throw = 10;
    m_touchDriver.gesture_limit = 56;
    m_touchDriver.long_press_time = 450;
    m_touchIndev = lv_indev_drv_register(&m_touchDriver);
    s_instance = this;
    m_lastTick = millis();
    printf("[LVGL] port ready\n");
    return true;
}

void WaveshareLvglPort::runSelfTest() {
    if (!s_gfx) return;
    const uint16_t colors[] = {kRed, kGreen, kBlue, kWhite, kBlack};
    while (true) {
        for (uint16_t color : colors) {
            s_gfx->fillScreen(color);
            delay(700);
        }
    }
}

void WaveshareLvglPort::tick() {
    const uint32_t now = millis();
    lv_tick_inc(now - m_lastTick);
    m_lastTick = now;
    // IMU auto-rotate removed by request 2026-07-24 — the screen orientation
    // is fixed now. m_displayFlipped stays false, so the touch mapping never
    // takes the flipped branch either. initImu()/pollImuOrientation() remain
    // in the file but are no longer driven.
    const uint32_t callStartUs = micros();
    lv_timer_handler();
    const uint32_t callUs = micros() - callStartUs;
    ++s_perfTimerHandlerCalls;
    s_perfTimerHandlerBusyUs += callUs;
    if (callUs > s_perfTimerHandlerMaxUs) s_perfTimerHandlerMaxUs = callUs;
    if (s_perfWindowStartMs == 0) s_perfWindowStartMs = now;
    if (now - s_perfWindowStartMs >= 1000) {
        const uint32_t flushesThisWindow = s_flushCount - s_perfFlushAtWindowStart;
        printf("[PERF] flush/s=%lu timer_handler_calls/s=%lu busy_us=%lu (%.1f%%) max_call_us=%lu\n",
               static_cast<unsigned long>(flushesThisWindow),
               static_cast<unsigned long>(s_perfTimerHandlerCalls),
               static_cast<unsigned long>(s_perfTimerHandlerBusyUs),
               static_cast<double>(s_perfTimerHandlerBusyUs) / 10000.0,
               static_cast<unsigned long>(s_perfTimerHandlerMaxUs));
        // A dropped transfer leaves that screen region showing stale content
        // (this is what the pre-fix garbling was), and it is otherwise
        // completely silent -- so keep reporting it, but only when it
        // actually happens rather than on every line.
        if (s_drawErrCount) {
            printf("[LCD] draw_bitmap dropped %lu transfer(s), last=%s\n",
                   static_cast<unsigned long>(s_drawErrCount), esp_err_to_name(s_lastDrawErr));
        }
        s_perfWindowStartMs = now;
        s_perfFlushAtWindowStart = s_flushCount;
        s_perfTimerHandlerCalls = 0;
        s_perfTimerHandlerBusyUs = 0;
        s_perfTimerHandlerMaxUs = 0;
    }
    if (s_flushCount == 0 && now - s_lastFallbackDraw > 2000) {
        s_lastFallbackDraw = now;
        drawFallbackHome();
    }
}

bool WaveshareLvglPort::consumeGesture(TouchGesture* gesture) {
    if (!gesture || m_pendingGesture == TouchGesture::None) return false;
    *gesture = m_pendingGesture;
    m_pendingGesture = TouchGesture::None;
    return true;
}

void WaveshareLvglPort::applyDisplayRotation(bool flipped) {
    m_displayFlipped = flipped;
    if (s_gfx) s_gfx->setRotation(flipped ? kDisplayRotationFlipped : kDisplayRotationNormal);
}

bool WaveshareLvglPort::initImu() {
    uint8_t who = 0;
    if (!readRegisters(kImuAddress, 0x00, &who, 1) || who != 0x05) {
        printf("[IMU] QMI8658 not detected\n");
        return false;
    }
    // QMI8658: CTRL1 enables address auto-increment, CTRL2 configures accel,
    // CTRL3 configures gyro to 512 dps, CTRL7 enables accel + gyro.
    writeRegister(kImuAddress, 0x02, 0x40);
    writeRegister(kImuAddress, 0x03, 0x95);
    writeRegister(kImuAddress, 0x04, 0x56);
    writeRegister(kImuAddress, 0x08, 0x03);
    delay(80);
    printf("[IMU] QMI8658 ready\n");
    return true;
}

bool WaveshareLvglPort::readImuAccel(int16_t* ax, int16_t* ay, int16_t* az) {
    uint8_t data[6] = {};
    if (!ax || !ay || !az || !readRegisters(kImuAddress, 0x35, data, sizeof(data))) return false;
    *ax = static_cast<int16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
    *ay = static_cast<int16_t>((static_cast<uint16_t>(data[3]) << 8) | data[2]);
    *az = static_cast<int16_t>((static_cast<uint16_t>(data[5]) << 8) | data[4]);
    return true;
}

bool WaveshareLvglPort::readImuGyroZ(int16_t* gz) {
    uint8_t data[2] = {};
    if (!gz || !readRegisters(kImuAddress, 0x3F, data, sizeof(data))) return false;
    *gz = static_cast<int16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
    return true;
}

void WaveshareLvglPort::pollImuOrientation() {
    if (!m_autoRotation || !m_imuReady || !s_gfx) return;
    const uint32_t now = millis();
    if (now - m_lastImuPoll < kImuPollMs) return;
    const uint32_t deltaMs = m_lastGyroMs == 0 ? kImuPollMs : now - m_lastGyroMs;
    m_lastGyroMs = now;
    m_lastImuPoll = now;

    int16_t gz = 0;
    if (readImuGyroZ(&gz)) {
        if (m_gyroCalibSamples < kGyroCalibSamples) {
            m_gyroBiasZ += (static_cast<float>(gz) - m_gyroBiasZ) / static_cast<float>(m_gyroCalibSamples + 1);
            ++m_gyroCalibSamples;
            if (m_gyroCalibSamples == kGyroCalibSamples) {
                printf("[IMU] gyro baseline z=%.1f\n", static_cast<double>(m_gyroBiasZ));
            }
        } else {
            const float dps = (static_cast<float>(gz) - m_gyroBiasZ) / kGyroLsbPerDps;
            if (fabsf(dps) >= kGyroActiveDps) {
                m_gyroYawDegrees += dps * (static_cast<float>(deltaMs) / 1000.0f);
                if (fabsf(m_gyroYawDegrees) >= kGyroFlipDegrees) {
                    applyDisplayRotation(!m_displayFlipped);
                    lv_obj_invalidate(lv_scr_act());
                    printf("[IMU] gyro 180 rotation %s yaw=%.1f dps=%.1f\n",
                           m_displayFlipped ? "flipped" : "normal",
                           static_cast<double>(m_gyroYawDegrees),
                           static_cast<double>(dps));
                    m_gyroYawDegrees = 0.0f;
                    m_orientationSince = 0;
                    return;
                }
            } else if (fabsf(m_gyroYawDegrees) < 30.0f) {
                m_gyroYawDegrees *= 0.86f;
            }
        }
    }

    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;
    if (!readImuAccel(&ax, &ay, &az)) return;
    const int32_t magnitude2 = static_cast<int32_t>(ax) * ax + static_cast<int32_t>(ay) * ay;
    if (magnitude2 < kImuMinHorizontalMagnitudeSquared) return;
    if (!m_imuBaselineReady) {
        m_imuBaseX = ax;
        m_imuBaseY = ay;
        m_imuBaselineReady = true;
        printf("[IMU] baseline ax=%d ay=%d az=%d\n", ax, ay, az);
        return;
    }

    const int32_t dot = static_cast<int32_t>(ax) * m_imuBaseX + static_cast<int32_t>(ay) * m_imuBaseY;
    const bool wantFlipped = dot < kImuFlipDotThreshold;
    if (wantFlipped == m_displayFlipped) {
        m_orientationSince = 0;
        return;
    }
    if (m_orientationSince == 0) {
        m_orientationSince = now;
        return;
    }
    if (now - m_orientationSince < kOrientationDebounceMs) return;

    applyDisplayRotation(wantFlipped);
    lv_obj_invalidate(lv_scr_act());
    printf("[IMU] display auto-rotation %s ax=%d ay=%d dot=%ld\n",
           wantFlipped ? "flipped" : "normal",
           ax,
           ay,
           static_cast<long>(dot));
    m_orientationSince = 0;
}

void WaveshareLvglPort::flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colorMap) {
    auto* panel = static_cast<esp_lcd_panel_handle_t>(driver->user_data);
    if (!panel || !colorMap) {
        lv_disp_flush_ready(driver);
        return;
    }

    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    if (s_flushCount < 3) {
        printf("[LVGL] flush #%lu x=%d y=%d w=%ld h=%ld\n",
                      static_cast<unsigned long>(s_flushCount + 1),
                      area->x1,
                      area->y1,
                      static_cast<long>(width),
                      static_cast<long>(height));
    }
    ++s_flushCount;
    // esp_lcd's end coordinates are exclusive, LVGL's area is inclusive.
    // Queues the transfer and returns immediately -- lv_disp_flush_ready() is
    // deliberately NOT called here. It happens in onColorTransDone() once the
    // DMA actually finishes, which is the whole point of this migration:
    // LVGL gets to render into the other buffer while this one is still on
    // the wire. Calling flush_ready here would let LVGL overwrite a buffer
    // the DMA is still reading.
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, colorMap);
    if (err != ESP_OK) {
        // If the transfer is rejected outright there will be no completion
        // callback, so LVGL would wait on flush_ready forever and the whole
        // UI silently freezes with flush/s stuck at 0. Report it and release
        // the buffer ourselves so at least the failure is visible/recoverable.
        s_lastDrawErr = err;
        ++s_drawErrCount;
        lv_disp_flush_ready(driver);
    }
}

bool WaveshareLvglPort::readTouchPoint(uint16_t* x, uint16_t* y) {
    uint8_t data[7] = {};
    i2cBusOne.beginTransmission(kTouchAddress);
    i2cBusOne.write(static_cast<uint8_t>(0x00));
    if (i2cBusOne.endTransmission(false) != 0 || i2cBusOne.requestFrom(kTouchAddress, sizeof(data)) != sizeof(data)) return false;
    for (uint8_t& byte : data) byte = i2cBusOne.read();
    if ((data[2] & 0x0F) == 0) return false;
    const uint16_t rawX = ((data[3] & 0x0F) << 8) | data[4]; // CST816 short axis ~0..169 (panel width)
    const uint16_t rawY = ((data[5] & 0x0F) << 8) | data[6]; // CST816 long axis  ~0..319 (panel height)

    // The panel is portrait-native (170x320); the display runs landscape
    // (320x170) via MADCTL, but the CST816 still reports in portrait. Map it:
    //   landscape X (0..319) = long axis, inverted = (kWidth-1) - rawY
    //   landscape Y (0..169) = short axis          = rawX
    // Derived and cross-checked on-device 2026-07-24 against four independent
    // symptoms (PLAY tap hitting LOCAL, SET tap dead, top-edge-down wrongly
    // firing back, left-edge-right failing to). The old code assigned the
    // short axis to X and clamped the long axis into Y=170, so the right half
    // of the screen was unreachable and the vertical swipe axis was dead.
    const uint16_t ry = std::min<uint16_t>(rawY, kWidth - 1);
    const uint16_t rx = std::min<uint16_t>(rawX, kHeight - 1);
    uint16_t px = (kWidth - 1) - ry;
    uint16_t py = rx;
    if (kInvertTouchX) px = kWidth - 1 - px;
    if (kInvertTouchY) py = kHeight - 1 - py;
    if (m_displayFlipped) {
        px = kWidth - 1 - px;
        py = kHeight - 1 - py;
    }
    *x = px;
    *y = py;
    return true;
}

void WaveshareLvglPort::updateGesture(bool pressed, uint16_t x, uint16_t y) {
    const uint32_t now = millis();
    if (pressed) {
        m_touchCurrentX = x;
        m_touchCurrentY = y;
        if (!m_touchWasPressed) {
            m_touchStartX = x;
            m_touchStartY = y;
            m_touchStartMs = now;
            m_touchWasPressed = true;
            m_touchGestureFired = false;
        }
        const int16_t dx = static_cast<int16_t>(m_touchCurrentX) - static_cast<int16_t>(m_touchStartX);
        const int16_t dy = static_cast<int16_t>(m_touchCurrentY) - static_cast<int16_t>(m_touchStartY);
        const uint32_t duration = now - m_touchStartMs;
        if (!m_touchSuppressingGesture && duration <= kGestureMaxMs) {
            const bool edgeBackCandidate = m_touchStartX <= kEdgeBackPx && dx >= kGesturePreclaimPx && abs(dy) <= kGestureCrossMaxPx;
            const bool edgeTopCandidate = m_touchStartY <= kEdgeTopPx && dy >= kGesturePreclaimPx && abs(dx) <= kGestureCrossMaxPx;
            const bool edgeBottomCandidate = m_touchStartY >= kHeight - kEdgeBottomPx && -dy >= kGesturePreclaimPx && abs(dx) <= kGestureCrossMaxPx;
            if (edgeBackCandidate || edgeTopCandidate || edgeBottomCandidate) {
                m_touchSuppressingGesture = true;
                m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
                if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
            }
        }
        // Only the horizontal edge-back swipe is a real gesture. A prior
        // vertical-motion branch here matched on dy alone and set
        // m_touchGestureFired with no action -- any natural finger tremor
        // during an ordinary tap on the 46px-tall control bar could cross
        // that threshold and silently swallow the tap. Removed; see
        // touch diagnostic log from 2026-07-23 (dy=-154 on what looked
        // like a Play tap, consumed with zero feedback).
        if (!m_touchGestureFired && duration <= kGestureMaxMs && abs(dx) >= kGestureMinPx && abs(dy) <= kGestureCrossMaxPx) {
            if (m_touchStartX <= kEdgeBackPx && dx > 0) {
                m_pendingGesture = TouchGesture::EdgeBack;
                m_touchGestureFired = true;
                m_touchSuppressingGesture = true;
                m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
                if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
            }
        }
        // Quick-settings panel: swipe down starting within kEdgeTopPx of the
        // top edge opens it, swipe up starting within kEdgeBottomPx of the
        // bottom edge closes it -- edge-gated the same way as EdgeBack, so
        // an ordinary vertical scroll/tap deep in the middle of the screen
        // never qualifies (see the removed-branch history above).
        if (!m_touchGestureFired && duration <= kGestureMaxMs && abs(dy) >= kGestureMinPx && abs(dx) <= kGestureCrossMaxPx) {
            if (m_touchStartY <= kEdgeTopPx && dy > 0) {
                m_pendingGesture = TouchGesture::EdgeTopOpen;
                m_touchGestureFired = true;
                m_touchSuppressingGesture = true;
                m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
                if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
            } else if (m_touchStartY >= kHeight - kEdgeBottomPx && dy < 0) {
                m_pendingGesture = TouchGesture::EdgeBottomClose;
                m_touchGestureFired = true;
                m_touchSuppressingGesture = true;
                m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
                if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
            }
        }
        return;
    }
    if (!m_touchWasPressed) return;
    m_touchWasPressed = false;
    if (m_touchSuppressingGesture) {
        m_touchSuppressingGesture = false;
        m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
    }
    const int16_t dx = static_cast<int16_t>(m_touchCurrentX) - static_cast<int16_t>(m_touchStartX);
    const int16_t dy = static_cast<int16_t>(m_touchCurrentY) - static_cast<int16_t>(m_touchStartY);
    const uint32_t duration = now - m_touchStartMs;
    if (duration > kGestureMaxMs || m_touchGestureFired) return;

    if (abs(dx) >= kGestureMinPx && abs(dy) <= kGestureCrossMaxPx) {
        if (m_touchStartX <= kEdgeBackPx && dx > 0) {
            m_pendingGesture = TouchGesture::EdgeBack;
            m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
            if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
        }
    } else if (abs(dy) >= kGestureMinPx && abs(dx) <= kGestureCrossMaxPx) {
        if (m_touchStartY <= kEdgeTopPx && dy > 0) {
            m_pendingGesture = TouchGesture::EdgeTopOpen;
            m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
            if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
        } else if (m_touchStartY >= kHeight - kEdgeBottomPx && dy < 0) {
            m_pendingGesture = TouchGesture::EdgeBottomClose;
            m_touchSuppressUntilMs = now + kPostGestureClickSuppressMs;
            if (m_touchIndev) lv_indev_reset(m_touchIndev, nullptr);
        }
    }
}

void WaveshareLvglPort::readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
    auto* self = s_instance;
    const uint32_t now = millis();
    uint16_t x = 0;
    uint16_t y = 0;
    if (self && self->readTouchPoint(&x, &y)) {
        self->m_touchMisses = 0;
        self->m_lastX = x;
        self->m_lastY = y;
        self->updateGesture(true, x, y);
        const bool suppressClick = self->m_touchSuppressingGesture || static_cast<int32_t>(now - self->m_touchSuppressUntilMs) < 0;
        data->state = suppressClick ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
    } else {
        if (self && self->m_touchWasPressed && self->m_touchMisses < kTouchReleaseGraceReads) {
            ++self->m_touchMisses;
            self->updateGesture(true, self->m_lastX, self->m_lastY);
            const bool suppressClick = self->m_touchSuppressingGesture || static_cast<int32_t>(now - self->m_touchSuppressUntilMs) < 0;
            data->state = suppressClick ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
        } else {
            if (self) {
                self->m_touchMisses = 0;
                self->updateGesture(false, self->m_lastX, self->m_lastY);
            }
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->point.x = self ? self->m_lastX : 0;
    data->point.y = self ? self->m_lastY : 0;
}
