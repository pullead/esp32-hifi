#pragma once

#include <Arduino.h>
#include <Wire.h>

extern __attribute__((weak)) void tp_info(const char* info);
extern __attribute__((weak)) void tp_moved(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_pressed(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_long_pressed(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_released(uint16_t x, uint16_t y);
extern __attribute__((weak)) void tp_long_released(uint16_t x, uint16_t y);

class TP_CST816 {
  private:
    struct T_Point {
        uint16_t x;
        uint16_t y;
        uint16_t id;
    };

  public:
    static const uint8_t CST816_I2C_ADDRESS = 0x15;

    TP_CST816();
    bool    begin(TwoWire* twi, int addr, uint16_t h_resolution, uint16_t v_resolution);
    bool    probe();
    void    loop();
    void    setRotation(uint8_t m);
    void    setMirror(bool h, bool v);
    uint8_t touched();
    T_Point getPoint(uint8_t num);

  private:
    TwoWire* m_wire = nullptr;
    uint8_t  m_addr = CST816_I2C_ADDRESS;
    uint8_t  m_rotation = 0;
    uint16_t m_h_resolution = 0;
    uint16_t m_v_resolution = 0;
    bool     m_f_isTouch = false;
    bool     m_f_isLongPressed = false;
    bool     m_isInit = false;
    bool     m_mirror_h = false;
    bool     m_mirror_v = false;

    uint8_t read(uint8_t reg);
};
