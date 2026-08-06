#include "waveshare_lvgl_port.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <algorithm>
#include <cmath>
#include <esp_heap_caps.h>

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
constexpr int kRowsPerBuffer = 80;
// 这个常量之前一直是这条 LVGL/Arduino_GFX 路径真正生效的 LCD SPI 频率——
// settings.h 里的 TFT_FREQUENCY 只给旧的非 LVGL 驱动路径用，那条路径在
// MWR_LVGL_UI 打开时被 setup() 里的提前 return 挡住，根本不会执行到，所以
// 改 TFT_FREQUENCY 对这条实际在跑的路径没有任何效果。40 -> 80MHz 已经实测
// 稳定；现在测试 100MHz，其余配置（Flash QIO 40MHz / PSRAM 120MHz / 80行
// buffer）保持不动，单独隔离这一个变量。
constexpr uint32_t kTftSpiHz = 100000000;
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

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_gfx = nullptr;
uint32_t s_flushCount = 0;
uint32_t s_lastFallbackDraw = 0;

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

    s_bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_CLK, LCD_DIN);
    // ips=true: photos of the light-mode retheme showed a clean inversion
    // signature (white bg -> near-black, purple #8A6CFF -> olive/yellow-
    // green, teal #1CA9A0 -> pink/red -- textbook 255-minus-channel
    // inversion). ST7789 panels disagree on default polarity; this board's
    // panel wants INVON. Was invisible on the old dark theme by coincidence
    // (inverted-dark still reads as "dark"), obvious once the bg is white.
    s_gfx = new Arduino_ST7789(s_bus, LCD_RST, 0, true, 170, 320, 35, 0, 35, 0);
    if (!s_gfx || !s_gfx->begin(kTftSpiHz)) {
        printf("[LCD] Arduino_GFX begin failed\n");
        return false;
    }
    applyDisplayRotation(false);
    if (kBootColorFlash) {
        s_gfx->fillScreen(kRed);
        delay(120);
        s_gfx->fillScreen(kGreen);
        delay(120);
        s_gfx->fillScreen(kBlue);
        delay(120);
    }
    s_gfx->fillScreen(kBlack);
    printf("[LCD] Arduino_GFX panel ready\n");
    m_panel = s_gfx;
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
    const size_t bufferBytes = kWidth * kRowsPerBuffer * sizeof(lv_color_t);
    // PSRAM, not MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL: Arduino_HWSPI (the
    // driver actually in use here, see initPanel()) doesn't DMA straight out
    // of this buffer anyway -- writePixels() already copies through its own
    // internal bounce buffer via plain CPU reads before handing bytes to
    // SPI.transfer(). So there's no hardware requirement for this buffer to
    // be internal/DMA-capable; PSRAM just costs a bit more read latency
    // during that copy. Moving it here gives back ~25KB of internal RAM
    // that used to be permanently reserved by these two buffers -- see
    // setupLvglRuntime()'s comment in main.cpp on why that RAM matters (WiFi
    // was failing esp_wifi_init() for lack of exactly this much headroom).
    m_bufferA = static_cast<lv_color_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM));
    m_bufferB = static_cast<lv_color_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM));
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
    auto* gfx = static_cast<Arduino_GFX*>(driver->user_data);
    if (!gfx || !colorMap) {
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
    gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(colorMap), width, height);
    lv_disp_flush_ready(driver);
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
