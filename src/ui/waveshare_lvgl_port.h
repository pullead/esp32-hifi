#pragma once

#include <Arduino.h>
#include <lvgl.h>

enum class TouchGesture : uint8_t { None, EdgeBack, EdgeTopOpen, EdgeBottomClose };

// Sole LCD/touch owner once MWR_LVGL_UI is enabled.
class WaveshareLvglPort {
  public:
    // 最近 withinMs 毫秒内有没有手指在屏上。
    // ⚠️ 给"别在用户操作时做慢活"用的：SD 写入等阻塞操作若落在滑动过程中，
    // 会造成几百毫秒的可感知卡顿（2026-09-05 实测 181~413ms）。
    static bool recentlyTouched(uint32_t withinMs);

    bool begin();
    void tick();
    bool consumeGesture(TouchGesture* gesture);
    // PWM-dimmable backlight (was a plain digitalWrite on/off) -- 1..100,
    // clamped; below ~5% the panel is unreadably dark so callers shouldn't
    // go to 0 unless they intend the screen to look off.
    void setBacklightPercent(uint8_t percent);
    uint8_t backlightPercent() const { return m_backlightPercent; }

  private:
    static void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colorMap);
    static void readTouch(lv_indev_drv_t* driver, lv_indev_data_t* data);
    static WaveshareLvglPort* s_instance;

    bool initPanel();
    void pollImuOrientation();
    bool initImu();
    bool readImuAccel(int16_t* ax, int16_t* ay, int16_t* az);
    bool readImuGyroZ(int16_t* gz);
    void applyDisplayRotation(bool flipped);
    bool readTouchPoint(uint16_t* x, uint16_t* y);
    void updateGesture(bool pressed, uint16_t x, uint16_t y);

    lv_disp_draw_buf_t m_drawBuffer{};
    lv_disp_drv_t m_displayDriver{};
    lv_disp_t* m_display = nullptr;
    lv_indev_drv_t m_touchDriver{};
    lv_indev_t* m_touchIndev = nullptr;
    lv_color_t* m_bufferA = nullptr;
    lv_color_t* m_bufferB = nullptr;
    uint16_t m_lastX = 0;
    uint16_t m_lastY = 0;
    uint16_t m_touchStartX = 0;
    uint16_t m_touchStartY = 0;
    uint16_t m_touchCurrentX = 0;
    uint16_t m_touchCurrentY = 0;
    uint8_t m_touchMisses = 0;
    uint32_t m_lastTick = 0;
    uint32_t m_touchStartMs = 0;
    uint32_t m_touchLastActiveMs = 0;   // 最近一次有手指在屏上的时刻
    uint32_t m_touchSuppressUntilMs = 0;
    uint32_t m_lastImuPoll = 0;
    uint32_t m_lastGyroMs = 0;
    uint32_t m_orientationSince = 0;
    int16_t m_imuBaseX = 0;
    int16_t m_imuBaseY = 0;
    float m_gyroBiasZ = 0.0f;
    float m_gyroYawDegrees = 0.0f;
    uint16_t m_gyroCalibSamples = 0;
    void* m_panel = nullptr;
    bool m_touchWasPressed = false;
    bool m_touchGestureFired = false;
    bool m_touchSuppressingGesture = false;
    bool m_imuReady = false;
    bool m_imuBaselineReady = false;
    bool m_autoRotation = true;
    bool m_displayFlipped = false;
    TouchGesture m_pendingGesture = TouchGesture::None;
    uint8_t m_backlightPercent = 100;
};
