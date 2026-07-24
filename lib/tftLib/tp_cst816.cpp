#include "tp_cst816.h"

#include <algorithm>

__attribute__((weak)) void tp_info(const char* info) {}
__attribute__((weak)) void tp_moved(uint16_t x, uint16_t y) {}
__attribute__((weak)) void tp_pressed(uint16_t x, uint16_t y) {}
__attribute__((weak)) void tp_long_pressed(uint16_t x, uint16_t y) {}
__attribute__((weak)) void tp_released(uint16_t x, uint16_t y) {}
__attribute__((weak)) void tp_long_released(uint16_t x, uint16_t y) {}

#define ANSI_ESC_RED   "\033[31m"
#define ANSI_ESC_CYAN  "\033[36m"
#define ANSI_ESC_RESET "\033[0m"

static constexpr uint8_t CST816_REG_TOUCH_NUM = 0x02;
static constexpr uint8_t CST816_REG_TOUCH1_XH = 0x03;
static constexpr uint8_t CST816_REG_TOUCH1_XL = 0x04;
static constexpr uint8_t CST816_REG_TOUCH1_YH = 0x05;
static constexpr uint8_t CST816_REG_TOUCH1_YL = 0x06;

TP_CST816::TP_CST816() {}

bool TP_CST816::begin(TwoWire* twi, int addr, uint16_t h_resolution, uint16_t v_resolution) {
    m_wire = twi;
    m_wire->setTimeOut(1000);
    if (addr >= 0) { m_addr = static_cast<uint8_t>(addr); }

    if (probe()) {
        m_isInit = true;
        m_h_resolution = h_resolution;
        m_v_resolution = v_resolution;

        char buff[64] = {0};
        sprintf(buff, "CST816 TouchPad found at " ANSI_ESC_CYAN "0x%02X" ANSI_ESC_RESET, m_addr);
        tp_info(buff);
        return true;
    }

    m_wire->flush();
    log_e(ANSI_ESC_RED "CST816 TouchPad not Initialized at 0x%02X" ANSI_ESC_RESET, m_addr);
    return false;
}

bool TP_CST816::probe() {
    m_wire->beginTransmission(m_addr);
    return m_wire->endTransmission(true) == 0;
}

void TP_CST816::loop() {
    if (!m_isInit) return;
    static T_Point  p, p1;
    static uint32_t ts = 0;
    uint8_t         t = touched();

    if (t == 1 && !m_f_isTouch) {
        p = getPoint(t);
        tp_pressed(p.x, p.y);
        ts = millis();
        m_f_isTouch = true;
        return;
    }
    if (t == 1 && m_f_isTouch) {
        p1 = getPoint(t);
        if (p1.x != p.x || p1.y != p.y) {
            p = p1;
            tp_moved(p.x, p.y);
            return;
        }
    }
    if (t == 1 && m_f_isTouch && (millis() > ts + 2000) && !m_f_isLongPressed) {
        m_f_isLongPressed = true;
        tp_long_pressed(p.x, p.y);
        ts = millis() + 10000;
        return;
    }
    if (t == 0 && m_f_isTouch && !m_f_isLongPressed) {
        tp_released(p.x, p.y);
        m_f_isTouch = false;
        return;
    }
    if (t == 0 && m_f_isLongPressed) {
        m_f_isLongPressed = false;
        tp_long_released(p.x, p.y);
        m_f_isTouch = false;
    }
}

void TP_CST816::setRotation(uint8_t m) {
    m_rotation = m;
}

void TP_CST816::setMirror(bool h, bool v) {
    m_mirror_h = h;
    m_mirror_v = v;
}

uint8_t TP_CST816::touched() {
    if (!m_isInit) return 0;
    return read(CST816_REG_TOUCH_NUM) & 0x0F;
}

TP_CST816::T_Point TP_CST816::getPoint(uint8_t num) {
    if (!m_isInit || num != 1) return {0, 0, 0};

    T_Point points;
    points.id = 0;
    points.x = (read(CST816_REG_TOUCH1_XH) & 0x0F) << 8;
    points.x += read(CST816_REG_TOUCH1_XL);
    points.y = (read(CST816_REG_TOUCH1_YH) & 0x0F) << 8;
    points.y += read(CST816_REG_TOUCH1_YL);

    if (m_rotation == 0) {
        if (m_mirror_v) points.x = m_h_resolution - points.x;
        if (m_mirror_h) points.y = m_v_resolution - points.y;
    } else if (m_rotation == 1) {
        uint16_t tmp = points.x;
        points.x = m_mirror_v ? m_h_resolution - points.y : points.y;
        points.y = m_mirror_h ? tmp : m_v_resolution - tmp;
    } else if (m_rotation == 2) {
        if (!m_mirror_v) points.x = m_h_resolution - points.x;
        if (!m_mirror_h) points.y = m_v_resolution - points.y;
    } else if (m_rotation == 3) {
        uint16_t tmp = points.x;
        points.x = m_mirror_v ? points.y : m_h_resolution - points.y;
        points.y = m_mirror_h ? m_v_resolution - tmp : tmp;
    }

    points.x = std::min<uint16_t>(points.x, m_h_resolution ? m_h_resolution - 1 : 0);
    points.y = std::min<uint16_t>(points.y, m_v_resolution ? m_v_resolution - 1 : 0);
    return points;
}

uint8_t TP_CST816::read(uint8_t reg) {
    if (!m_isInit) return 0;
    m_wire->beginTransmission(m_addr);
    m_wire->write(reg);
    if (m_wire->endTransmission(false) != 0) return 0;
    if (m_wire->requestFrom(m_addr, static_cast<uint8_t>(1)) != 1) return 0;
    return m_wire->read();
}
