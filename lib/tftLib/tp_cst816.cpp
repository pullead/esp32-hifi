#include "common.h"
#include "tp_cst816.h"

bool CST816::begin(TwoWire* wire, uint8_t address, uint16_t width, uint16_t height)
{
    m_wire = wire;
    m_address = address;
    m_width = width;
    m_height = height;
    m_wire->setTimeOut(1000);
    m_wire->beginTransmission(m_address);
    if (m_wire->endTransmission() != 0) {
        log_e("CST816 not found at 0x%02X", m_address);
        return false;
    }
    m_wire->beginTransmission(m_address);
    m_wire->write(0x00);
    m_wire->write(0x00);
    m_wire->endTransmission(true);
    return true;
}

void CST816::setRotation(uint8_t rotation) { m_rotation = rotation % 4; }
void CST816::setMirror(bool horizontal, bool vertical) { m_mirror_h = horizontal; m_mirror_v = vertical; }

bool CST816::readPoint(uint16_t* x, uint16_t* y)
{
    uint8_t data[7] = {0};
    m_wire->beginTransmission(m_address);
    m_wire->write(0x00);
    if (m_wire->endTransmission(false) != 0 || m_wire->requestFrom((int)m_address, 7) != 7) {
        return false;
    }
    for (uint8_t i = 0; i < sizeof(data); ++i) data[i] = m_wire->read();
    if ((data[2] & 0x0F) == 0) return false;
    uint16_t px = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
    uint16_t py = ((uint16_t)(data[5] & 0x0F) << 8) | data[6];
    if (m_rotation == 1) { uint16_t t = px; px = py; py = m_width - 1 - t; }
    if (m_rotation == 2) { px = m_width - 1 - px; py = m_height - 1 - py; }
    if (m_rotation == 3) { uint16_t t = px; px = m_height - 1 - py; py = t; }
    if (m_mirror_h) px = m_width - 1 - px;
    if (m_mirror_v) py = m_height - 1 - py;
    *x = px < m_width ? px : m_width - 1;
    *y = py < m_height ? py : m_height - 1;
    return true;
}

void CST816::loop()
{
    uint16_t x = 0;
    uint16_t y = 0;
    const bool touched = readPoint(&x, &y);
    if (touched && !m_pressed) {
        m_pressed = true;
        m_long_pressed = false;
        m_last_x = x;
        m_last_y = y;
        m_pressed_at = millis();
        tp_pressed(x, y);
        return;
    }
    if (touched && m_pressed) {
        if (x != m_last_x || y != m_last_y) {
            m_last_x = x;
            m_last_y = y;
            tp_moved(x, y);
        } else if (!m_long_pressed && millis() - m_pressed_at >= 2000) {
            m_long_pressed = true;
            tp_long_pressed(x, y);
        }
        return;
    }
    if (!touched && m_pressed) {
        if (m_long_pressed) tp_long_released(m_last_x, m_last_y);
        else tp_released(m_last_x, m_last_y);
        m_pressed = false;
        m_long_pressed = false;
    }
}
