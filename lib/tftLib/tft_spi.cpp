// first release on 09/2019
// updated on Jul 05 2026

#include "common.h"

#include "tft_spi.h"
#include "Arduino.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"

SPIClass*   SPItransfer;

static const sh8601_lcd_init_cmd_t waveshare_sh8601_init[] = {
    {0x36, (uint8_t[]){0x70}, 1, 0}, {0xB2, (uint8_t[]){0x0C,0x0C,0x00,0x33,0x33}, 5, 0},
    {0xB7, (uint8_t[]){0x35}, 1, 0}, {0xBB, (uint8_t[]){0x13}, 1, 0}, {0xC0, (uint8_t[]){0x2C}, 1, 0},
    {0xC2, (uint8_t[]){0x01}, 1, 0}, {0xC3, (uint8_t[]){0x0B}, 1, 0}, {0xC4, (uint8_t[]){0x20}, 1, 0},
    {0xC6, (uint8_t[]){0x0F}, 1, 0}, {0xD0, (uint8_t[]){0xA4,0xA1}, 2, 0}, {0xD6, (uint8_t[]){0xA1}, 1, 0},
    {0xE0, (uint8_t[]){0x00,0x03,0x07,0x08,0x07,0x15,0x2A,0x44,0x42,0x0A,0x17,0x18,0x25,0x27}, 14, 0},
    {0xE1, (uint8_t[]){0x00,0x03,0x08,0x07,0x07,0x23,0x2A,0x43,0x42,0x09,0x18,0x17,0x25,0x27}, 14, 0},
    {0x21, nullptr, 0, 0}, {0x11, nullptr, 0, 120}, {0x29, nullptr, 0, 0},
};

#define __malloc_heap_psram(size) heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL)


//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
TFT_SPI::TFT_SPI(SPIClass& spiInstance, int csPin){
    m_freq = 20000000;
    _TFT_CS = csPin;
    spi_TFT = &spiInstance;
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
}
TFT_SPI::~TFT_SPI() {
    if(m_framebuffer[0]) {free(m_framebuffer[0]);}
    if(m_framebuffer[1]) {free(m_framebuffer[1]);}
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::loop(){
    GIF_loop();
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::setTFTcontroller(uint8_t TFTcontroller) {
    m_TFTcontroller = TFTcontroller; // 0=ILI9341, 1=HX8347D, 2=ILI9486(a), 3=ILI9486(b), 5=ST7796

    if(m_TFTcontroller == ILI9341)   { m_h_res = 320; m_v_res = 240; m_rotation = 0;}
    if(m_TFTcontroller == ILI9486)  { m_h_res = 480; m_v_res = 320; m_rotation = 0;}
    if(m_TFTcontroller == ILI9488_ST7796 )   { m_h_res = 480; m_v_res = 320; m_rotation = 0;}
    if(m_TFTcontroller == SH8601)            { m_h_res = 320; m_v_res = 170; m_rotation = 0;}

    m_framebuffer[0] = (uint16_t*)ps_malloc(m_h_res * m_v_res * 2);
    if(!m_framebuffer[0]) {if(tft_info) tft_info("Error allocating memory framebuffer 0"); return; }
    memset(m_framebuffer[0], 0, m_h_res * m_v_res * 2);

    m_framebuffer[1] = (uint16_t*)ps_malloc(m_h_res * m_v_res * 2);
    if(!m_framebuffer[1]) {if(tft_info) tft_info("Error allocating memory framebuffer 1"); return; }
    memset(m_framebuffer[1], 0, m_h_res * m_v_res * 2);

    // m_framebuffer[2] = (uint16_t*)ps_malloc(m_h_res * m_v_res * 2);
    // if(!m_framebuffer[2]) {if(tft_info) tft_info("Error allocating memory framebuffer 2"); return; }
    // memset(m_framebuffer[2], 0, m_h_res * m_v_res * 2);
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::setDisplayInversion(uint8_t i) {
    m_displayInversion = i;
    if(m_TFTcontroller == SH8601 && !m_sh8601_panel) return;
    startWrite();
    if(m_TFTcontroller == ILI9341) { writeCommand(i ? INVON : INVOFF); }
    if(m_TFTcontroller == ILI9486) { writeCommand(i ? INVON : INVOFF); }
    if(m_TFTcontroller == ILI9488_ST7796)  { writeCommand(i ? INVON : INVOFF); }
    if(m_TFTcontroller == SH8601)          { writeCommand(i ? INVON : INVOFF); }
    endWrite();

}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::displayInversion() {
    if(m_TFTcontroller == ILI9341) { writeCommand(m_displayInversion ? INVON : INVOFF); }
    if(m_TFTcontroller == ILI9486) { writeCommand(m_displayInversion ? INVON : INVOFF); }
    if(m_TFTcontroller == ILI9488_ST7796)  { writeCommand(m_displayInversion ? INVON : INVOFF); }
    if(m_TFTcontroller == SH8601)          { writeCommand(m_displayInversion ? INVON : INVOFF); }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::setFrequency(uint32_t f) {
    if(f > 80000000) f = 80000000;
    m_freq = f; // overwrite default
    spi_TFT->setFrequency(m_freq);
    SPIset = SPISettings(m_freq, MSBFIRST, SPI_MODE0);
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::startWrite(void) {
    spi_TFT->beginTransaction(SPIset);
    TFT_CS_LOW();
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::endWrite(void) {
    TFT_CS_HIGH();
    spi_TFT->endTransaction();
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// Return the size of the display (per current rotation)
// int16_t TFT_SPI::width(void) const { return m_h_res; }
// int16_t TFT_SPI::height(void) const { return m_v_res; }
uint8_t TFT_SPI::getRotation(void) const { return m_rotation; }
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
bool TFT_SPI::panelDrawBitmap(int16_t x0, int16_t y0, int16_t x1, int16_t y1, const void* bitmap) {
    if(m_TFTcontroller == SH8601 && m_sh8601_panel) {
        const uint16_t* pixels = static_cast<const uint16_t*>(bitmap);
        uint16_t line[320];
        for (int16_t y = y0; y < y1; ++y) {
            const uint16_t* source = pixels + y * m_h_res + x0;
            for (int16_t x = 0; x < x1 - x0; ++x) line[x] = __builtin_bswap16(source[x]);
            if (esp_lcd_panel_draw_bitmap(m_sh8601_panel, x0, y + 35, x1, y + 36, line) != ESP_OK) return false;
        }
        return true;
    }
    bool res = false;
    if(x0 >= x1 || y0 >= y1) {log_w("%s %i: x0 %i, y0 %i, x1 %i, y1 %i", __FILE__, __LINE__, x0, y0, x1, y1); return false;}

    int16_t w = abs(x1 - x0);
    int16_t h = abs(y1 - y0);
    uint16_t* pixels = const_cast<uint16_t*>(static_cast<const uint16_t*>(bitmap));

    startWrite();
    setAddrWindow(x0, y0, w, h);
    for(int16_t j = y0; j < y0 + h; j++) {
        writePixels(pixels + j * m_h_res + x0, w);
    }
    endWrite();

    return res;
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::writeCommand(uint16_t cmd) {
    TFT_DC_LOW();
    if(m_TFTcontroller == ILI9341 || m_TFTcontroller == ILI9488_ST7796 || m_TFTcontroller == SH8601) spi_TFT->write(cmd);

    if(m_TFTcontroller == ILI9486) spi_TFT->write16(cmd);
    TFT_DC_HIGH();
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint16_t TFT_SPI::readCommand() {
    uint16_t ret = 0;
    TFT_DC_LOW();
    if(m_TFTcontroller == ILI9341 || m_TFTcontroller == ILI9488_ST7796 || m_TFTcontroller == SH8601) ret = spi_TFT->transfer(0);

    if(m_TFTcontroller == ILI9486) ret = spi_TFT->transfer16(0);
    TFT_DC_HIGH();
    return ret;
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::begin(uint8_t DC) {
    SPIset = SPISettings(m_freq, MSBFIRST, SPI_MODE0);
    String info = "";

    _TFT_DC = DC;

    pinMode(_TFT_DC, OUTPUT);
    digitalWrite(_TFT_DC, LOW);
    pinMode(_TFT_CS, OUTPUT);
    digitalWrite(_TFT_CS, HIGH);
    if(m_TFTcontroller == SH8601) {
        spi_bus_config_t bus = {};
        bus.sclk_io_num = TFT_SCK;
        bus.mosi_io_num = TFT_MOSI;
        bus.miso_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.quadwp_io_num = -1;
        bus.max_transfer_sz = m_h_res * 20 * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
        esp_lcd_panel_io_spi_config_t io = SH8601_PANEL_IO_SPI_CONFIG(_TFT_CS, _TFT_DC, nullptr, nullptr);
        io.pclk_hz = TFT_FREQUENCY;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io, &panel_io));
        sh8601_vendor_config_t vendor = { .init_cmds = waveshare_sh8601_init, .init_cmds_size = sizeof(waveshare_sh8601_init) / sizeof(waveshare_sh8601_init[0]) };
        esp_lcd_panel_dev_config_t panel = {};
        panel.reset_gpio_num = TFT_RST;
        panel.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel.bits_per_pixel = 16;
        // MiniWebRadio stores RGB565 pixels in the ESP32-native byte order.
        // Unlike the Waveshare LVGL demo, it does not enable LV_COLOR_16_SWAP.
        panel.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
        panel.vendor_config = &vendor;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel, &m_sh8601_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(m_sh8601_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(m_sh8601_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(m_sh8601_panel, true));
        return;
    }
    init(); //
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::writePixels(uint16_t* colors, uint32_t len) {
    if(m_TFTcontroller == ILI9488_ST7796) {
        uint32_t i = 0;
        while(len) {
            write24BitColor(*(colors + i));
            i++;
            len--;
        }
    }
    else if(m_TFTcontroller == SH8601) {
        // ESP32 stores RGB565 words little-endian; SH8601 expects MSB first.
        while (len) {
            const uint32_t count = min<uint32_t>(len, sizeof(buf) / 2);
            for (uint32_t i = 0; i < count; ++i) {
                const uint16_t color = colors[i];
                buf[i * 2] = color >> 8;
                buf[i * 2 + 1] = color & 0xFF;
            }
            spi_TFT->writeBytes(buf, count * 2);
            colors += count;
            len -= count;
        }
    }
    else { spi_TFT->writePixels((uint8_t*)colors, len * 2); }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::writeColor(uint16_t color, uint32_t len) {
    if(m_TFTcontroller == ILI9488_ST7796) {
        uint8_t r = (color & 0xF800) >> 8;
        uint8_t g = (color & 0x07E0) >> 3;
        uint8_t b = (color & 0x001F) << 3;
        uint8_t c[3] = {r, g, b};
        spi_TFT->writePattern(c, 3, len);
    }
    else {
        uint8_t c[2];
        c[0] = (color & 0xFF00) >> 8;
        c[1] = color & 0x00FF;
        spi_TFT->writePattern(c, 2, len);
    }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::write24BitColor(uint16_t color) {
    spi_TFT->write((color & 0xF800) >> 8); // r
    spi_TFT->write((color & 0x07E0) >> 3); // g
    spi_TFT->write((color & 0x001F) << 3); // b
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::writePixel(int16_t x, int16_t y, uint16_t color) {
    if((x < 0) || (x >= m_v_res) || (y < 0) || (y >= m_h_res)) return;
    setAddrWindow(x, y, 1, 1);
    switch(m_TFTcontroller) {
        case ILI9341: spi_TFT->write16(color); break;
        case ILI9486:
            writeCommand(RAMWR); spi_TFT->write16(color);
            break;
        case ILI9488_ST7796:
            writeCommand(RAMWR); write24BitColor(color);
            break;
        case SH8601: spi_TFT->write16(color); break;
        default:
            if(tft_info) tft_info("unknown tft controller");
            break;
    }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::readRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    // Check whether parameters are within the valid range
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > logicalWidth() || y + h > logicalHeight()) return; // logicalWidth() = vertical resolution
    if (!data || !m_framebuffer[0]) return;

    uint16_t* dst = data;
    uint16_t* src = m_framebuffer[0] + y * logicalWidth() + x;

    for (int32_t row = 0; row < h; row++) {
        memcpy(dst, src, w * sizeof(uint16_t));
        src += logicalWidth();
        dst += w;
    }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
//  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫ CONTROLLER SPECIFIC  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫ ⏫⏫⏫⏫⏫⏫  ⏫⏫⏫⏫⏫⏫
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::init() {
    startWrite();
    if(m_TFTcontroller == ILI9341) {
        if(tft_info) tft_info("init " ANSI_ESC_CYAN "ILI9341");
        writeCommand(0xCB); // POWERA
        spi_TFT->write(0x39); spi_TFT->write(0x2C); spi_TFT->write(0x00); spi_TFT->write(0x34);
        spi_TFT->write(0x02);
        writeCommand(0xCF); // POWERB
        spi_TFT->write(0x00); spi_TFT->write(0xC1); spi_TFT->write(0x30);
        writeCommand(0xE8); // DTCA
        spi_TFT->write(0x85); spi_TFT->write(0x00); spi_TFT->write(0x78);
        writeCommand(0xEA); // DTCB
        spi_TFT->write(0x00); spi_TFT->write(0x00);
        writeCommand(0xED); // POWER_SEQ
        spi_TFT->write(0x64); spi_TFT->write(0x03); spi_TFT->write(0X12); spi_TFT->write(0X81);
        writeCommand(0xF7); // PRC
        spi_TFT->write(0x20);
        writeCommand(0xC0);   // Power control
        spi_TFT->write(0x23); // VRH[5:0]
        writeCommand(0xC1);   // Power control
        spi_TFT->write(0x10); // SAP[2:0];BT[3:0]
        writeCommand(0xC5); // VCM control
        spi_TFT->write(0x3e); spi_TFT->write(0x28);
        writeCommand(0xC7); // VCM control2
        spi_TFT->write(0x86);
        writeCommand(0x36);   // Memory Access Control
        spi_TFT->write(0x48); // 88
        writeCommand(0x3A); // PIXEL_FORMAT
        spi_TFT->write(0x55);
        writeCommand(0xB1); // FRC
        spi_TFT->write(0x00); spi_TFT->write(0x18);
        writeCommand(0xB6); // Display Function Control
        spi_TFT->write(0x08); spi_TFT->write(0x82); spi_TFT->write(0x27);
        writeCommand(0xF2); // 3Gamma Function Disable
        spi_TFT->write(0x00);
        writeCommand(0x2A); // COLUMN_ADDR
        spi_TFT->write(0x00); spi_TFT->write(0x00); spi_TFT->write(0x00); spi_TFT->write(0xEF);
        writeCommand(0x2A); // PAGE_ADDR
        spi_TFT->write(0x00); spi_TFT->write(0x00); spi_TFT->write(0x01); spi_TFT->write(0x3F);
        writeCommand(0x26); // Gamma curve selected
        spi_TFT->write(0x01);
        writeCommand(0xE0); // Set Gamma
        spi_TFT->write(0x0F); spi_TFT->write(0x31); spi_TFT->write(0x2B); spi_TFT->write(0x0C); spi_TFT->write(0x0E); spi_TFT->write(0x08);
        spi_TFT->write(0x4E); spi_TFT->write(0xF1); spi_TFT->write(0x37); spi_TFT->write(0x07); spi_TFT->write(0x10); spi_TFT->write(0x03);
        spi_TFT->write(0x0E); spi_TFT->write(0x09); spi_TFT->write(0x00);
        writeCommand(0xE1); // Set Gamma
        spi_TFT->write(0x00); spi_TFT->write(0x0E); spi_TFT->write(0x14); spi_TFT->write(0x03); spi_TFT->write(0x11); spi_TFT->write(0x07);
        spi_TFT->write(0x31); spi_TFT->write(0xC1); spi_TFT->write(0x48); spi_TFT->write(0x08); spi_TFT->write(0x0F); spi_TFT->write(0x0C);
        spi_TFT->write(0x31); spi_TFT->write(0x36); spi_TFT->write(0x0F);
        writeCommand(SLPOUT); // Sleep out
        delay(120);
        writeCommand(RAMWR);
        displayInversion();
        writeCommand(DISPON); // Display on

        writeCommand(RAMWR);
        writeCommand(MADCTL);
        spi_TFT->write(MADCTL_MV | MADCTL_BGR);
    }
    if(m_TFTcontroller == ILI9486) {
        if(tft_info) tft_info("init " ANSI_ESC_CYAN "ILI9486");

        writeCommand(0x11); // Sleep out, also SW reset
        delay(120);

        writeCommand(0x3A); // Interface Pixel Format
        spi_TFT->write16(0x55);

        writeCommand(0xC2); // Power Control 3 (For Normal Mode)
        spi_TFT->write16(0x44);

        writeCommand(0xC5); // VCOM Control
        spi_TFT->write16(0x00); spi_TFT->write16(0x00); spi_TFT->write16(0x00); spi_TFT->write16(0x00);

        if(m_TFTcontroller == ILI9486) {
            writeCommand(0xE0); // PGAMCTRL(alternative Positive Gamma Control)
            spi_TFT->write16(0x0F); spi_TFT->write16(0x1F); spi_TFT->write16(0x1C); spi_TFT->write16(0x0C); spi_TFT->write16(0x0F); spi_TFT->write16(0x08);
            spi_TFT->write16(0x48); spi_TFT->write16(0x98); spi_TFT->write16(0x37); spi_TFT->write16(0x0A); spi_TFT->write16(0x13); spi_TFT->write16(0x04);
            spi_TFT->write16(0x11); spi_TFT->write16(0x0D); spi_TFT->write16(0x00);

            writeCommand(0xE1); // NGAMCTRL (alternative Negative Gamma Correction)
            spi_TFT->write16(0x0F); spi_TFT->write16(0x32); spi_TFT->write16(0x2E); spi_TFT->write16(0x0B); spi_TFT->write16(0x0D); spi_TFT->write16(0x05);
            spi_TFT->write16(0x47); spi_TFT->write16(0x75); spi_TFT->write16(0x37); spi_TFT->write16(0x06); spi_TFT->write16(0x10); spi_TFT->write16(0x03);
            spi_TFT->write16(0x24); spi_TFT->write16(0x20); spi_TFT->write16(0x00);
        }
        writeCommand(MADCTL); // Memory Access Control
        spi_TFT->write16(0x48);

        displayInversion();

        writeCommand(DISPON); // Display ON
        delay(150);

        writeCommand(MADCTL);
        spi_TFT->write16(MADCTL_MV | MADCTL_BGR);
    }
    if(m_TFTcontroller == ILI9488_ST7796) {
        if(tft_info) tft_info("init " ANSI_ESC_CYAN "ILI9488 or ST7796");
        writeCommand(0x01);
        delay(120);
        writeCommand(SLPOUT); // Sleep Out
        delay(120);
        writeCommand(MADCTL); // Memory Data Access Control
        spi_TFT->write(0x40);
        writeCommand(0xF0); // Command Set Control
        spi_TFT->write(0xC3);
        writeCommand(0xF0); // Command Set Control
        spi_TFT->write(0x96);
        writeCommand(0xB4); // Display Inversion Control
        spi_TFT->write(0x00);
        writeCommand(0xB0); // RAM control
        spi_TFT->write(0x00);
        writeCommand(0xB5); // Blanking Porch Control
        spi_TFT->write(0x08); spi_TFT->write(0x08); spi_TFT->write(0x00); spi_TFT->write(0x64);
        writeCommand(0xC0); // Power Control 1
        spi_TFT->write(0xF0); spi_TFT->write(0x17);
        writeCommand(0xC0); // Power Control 2
        spi_TFT->write(0x14);
        writeCommand(0xC2); // Power Control 3
        spi_TFT->write(0xA7);
        writeCommand(0xC5); // VCOM Control
        spi_TFT->write(0x20);
        writeCommand(0xE8); // Display Output Ctrl Adjust
        spi_TFT->write(0x40); spi_TFT->write(0x8A); spi_TFT->write(0x00); spi_TFT->write(0x00);
        spi_TFT->write(0x29); spi_TFT->write(0x01); spi_TFT->write(0xBF); spi_TFT->write(0x33);

        writeCommand(0xE0); // PGAMCTRL(Positive Gamma Control)
        spi_TFT->write(0xF0); spi_TFT->write(0x0B); spi_TFT->write(0x11); spi_TFT->write(0x0B); spi_TFT->write(0x0A); spi_TFT->write(0x27);
        spi_TFT->write(0x3C); spi_TFT->write(0x55); spi_TFT->write(0x51); spi_TFT->write(0x37); spi_TFT->write(0x15); spi_TFT->write(0x17);
        spi_TFT->write(0x31); spi_TFT->write(0x35);

        writeCommand(0xE1); // NGAMCTRL (Negative Gamma Correction)
        spi_TFT->write(0x4E); spi_TFT->write(0x15); spi_TFT->write(0x19); spi_TFT->write(0x0B); spi_TFT->write(0x09); spi_TFT->write(0x27);
        spi_TFT->write(0x34); spi_TFT->write(0x32); spi_TFT->write(0x46); spi_TFT->write(0x38); spi_TFT->write(0x14); spi_TFT->write(0x16);
        spi_TFT->write(0x26); spi_TFT->write(0x2A);

        writeCommand(0xF0); // Command Set Control
        spi_TFT->write(0x3C);

        writeCommand(0xF0); // Command Set Control
        spi_TFT->write(0x69);
        displayInversion();
        writeCommand(DISPON); // Display on
        delay(25);

        writeCommand(MADCTL);
        spi_TFT->write(MADCTL_MV | MADCTL_BGR);
     }
    if(m_TFTcontroller == SH8601) {
        if(tft_info) tft_info("init " ANSI_ESC_CYAN "SH8601 320x170");
        if (TFT_RST >= 0) {
            pinMode(TFT_RST, OUTPUT);
            digitalWrite(TFT_RST, LOW);
            delay(20);
            digitalWrite(TFT_RST, HIGH);
            delay(120);
        }
        // Required by the official Waveshare SH8601 driver: RGB565 pixels.
        writeCommand(0x3A); spi_TFT->write(0x55);
        writeCommand(0x36); spi_TFT->write(0x70);
        writeCommand(0xB2); { const uint8_t d[] = {0x0C,0x0C,0x00,0x33,0x33}; spi_TFT->writeBytes(d, sizeof(d)); }
        writeCommand(0xB7); spi_TFT->write(0x35);
        writeCommand(0xBB); spi_TFT->write(0x13);
        writeCommand(0xC0); spi_TFT->write(0x2C);
        writeCommand(0xC2); spi_TFT->write(0x01);
        writeCommand(0xC3); spi_TFT->write(0x0B);
        writeCommand(0xC4); spi_TFT->write(0x20);
        writeCommand(0xC6); spi_TFT->write(0x0F);
        writeCommand(0xD0); { const uint8_t d[] = {0xA4,0xA1}; spi_TFT->writeBytes(d, sizeof(d)); }
        writeCommand(0xD6); spi_TFT->write(0xA1);
        writeCommand(0xE0); { const uint8_t d[] = {0x00,0x03,0x07,0x08,0x07,0x15,0x2A,0x44,0x42,0x0A,0x17,0x18,0x25,0x27}; spi_TFT->writeBytes(d, sizeof(d)); }
        writeCommand(0xE1); { const uint8_t d[] = {0x00,0x03,0x08,0x07,0x07,0x23,0x2A,0x43,0x42,0x09,0x18,0x17,0x25,0x27}; spi_TFT->writeBytes(d, sizeof(d)); }
        writeCommand(0x21);
        writeCommand(SLPOUT); delay(120);
        writeCommand(DISPON);
    }

    endWrite();
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::setRotation(uint8_t m) {
    m_rotation = m % 4; // can't be higher than 3
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void TFT_SPI::setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if(m_TFTcontroller == ILI9341) { // ILI9341
        uint32_t xa = ((uint32_t)x << 16) | (x + w - 1);
        uint32_t ya = ((uint32_t)y << 16) | (y + h - 1);
        writeCommand(CASET);
        spi_TFT->write32(xa);
        writeCommand(RASET);
        spi_TFT->write32(ya);
        writeCommand(RAMWR);
    }
    if(m_TFTcontroller == ILI9486) {
        writeCommand(CASET); // Column addr set
        spi_TFT->write16(x >> 8);
        spi_TFT->write16(x & 0xFF); // XSTART
        w = x + w - 1;
        spi_TFT->write16(w >> 8);
        spi_TFT->write16(w & 0xFF);  // XEND
        writeCommand(PASET); // Row addr set
        spi_TFT->write16(y >> 8);
        spi_TFT->write16(y & 0xFF); // YSTART
        h = y + h - 1;
        spi_TFT->write16(h >> 8);
        spi_TFT->write16(h & 0xFF); // YEND
        writeCommand(RAMWR);
    }
    if(m_TFTcontroller == ILI9488_ST7796) {
        writeCommand(CASET); // Column addr set
        spi_TFT->write(x >> 8);
        spi_TFT->write(x & 0xFF); // XSTART
        w = x + w - 1;
        spi_TFT->write(w >> 8);
        spi_TFT->write(w & 0xFF);   // XEND
        writeCommand(RASET); // Row addr set
        spi_TFT->write(y >> 8);
        spi_TFT->write(y & 0xFF); // YSTART
        h = y + h - 1;
        spi_TFT->write(h >> 8);
        spi_TFT->write(h & 0xFF); // YEND
        writeCommand(RAMWR);
    }
    if(m_TFTcontroller == SH8601) {
        const uint16_t panel_y = y + 35;
        writeCommand(CASET);
        spi_TFT->write(x >> 8); spi_TFT->write(x & 0xFF);
        const uint16_t x_end = x + w - 1;
        spi_TFT->write(x_end >> 8); spi_TFT->write(x_end & 0xFF);
        writeCommand(RASET);
        spi_TFT->write(panel_y >> 8); spi_TFT->write(panel_y & 0xFF);
        const uint16_t y_end = panel_y + h - 1;
        spi_TFT->write(y_end >> 8); spi_TFT->write(y_end & 0xFF);
        writeCommand(RAMWR);
    }
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
