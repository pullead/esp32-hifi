#pragma once

#include <Arduino.h>
#include <Wire.h>

extern __attribute__((weak)) void tp_moved(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_pressed(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_long_pressed(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_released(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_long_released(uint16_t x, uint16_t y);

class CST816 {
  public:
    bool begin(TwoWire* wire, uint8_t address, uint16_t width, uint16_t height);
    void loop();
    void setRotation(uint8_t rotation);
    void setMirror(bool horizontal, bool vertical);

  private:
    bool readPoint(uint16_t* x, uint16_t* y);

    TwoWire* m_wire = nullptr;
    uint8_t m_address = 0x15;
    uint16_t m_width = 320;
    uint16_t m_height = 170;
    uint8_t m_rotation = 0;
    bool m_mirror_h = false;
    bool m_mirror_v = false;
    bool m_pressed = false;
    bool m_long_pressed = false;
    uint16_t m_last_x = 0;
    uint16_t m_last_y = 0;
    uint32_t m_pressed_at = 0;
};
