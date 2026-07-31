#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <lvgl.h>

namespace {
constexpr int kWidth = 320;
constexpr int kHeight = 170;
constexpr int kRows = 20;
constexpr uint8_t kDisplayRotation = 3;
constexpr uint8_t kTouchAddress = 0x15;
constexpr int LCD_RST = 9;
constexpr int LCD_CLK = 10;
constexpr int LCD_DC = 11;
constexpr int LCD_CS = 12;
constexpr int LCD_DIN = 13;
constexpr int LCD_BL = 14;
constexpr int I2C_SDA = 47;
constexpr int I2C_SCL = 48;

Arduino_DataBus* bus = nullptr;
Arduino_GFX* gfx = nullptr;
lv_display_t* display = nullptr;
lv_indev_t* touch = nullptr;
void* drawBufA = nullptr;
void* drawBufB = nullptr;
lv_obj_t* progress = nullptr;
lv_obj_t* playLabel = nullptr;
lv_obj_t* bars[14] = {};
bool playing = true;
uint32_t flushCount = 0;
uint32_t lastTick = 0;

uint16_t clampX(uint16_t value) {
    return std::min<uint16_t>(value, kWidth - 1);
}

uint16_t clampY(uint16_t value) {
    return std::min<uint16_t>(value, kHeight - 1);
}

void drawFallback() {
    gfx->fillScreen(0x0000);
    gfx->drawRect(0, 0, kWidth, kHeight, 0x07ff);
    gfx->drawFastHLine(0, 26, kWidth, 0x07ff);
    gfx->setTextColor(0xffff, 0x0000);
    gfx->setTextSize(2);
    gfx->setCursor(12, 6);
    gfx->print("LVGL 9.5 P1");
    gfx->setTextSize(1);
    gfx->setCursor(12, 48);
    gfx->print("Arduino_GFX fallback is alive");
    gfx->setCursor(12, 66);
    gfx->print("Init LVGL display/touch...");
}

void flushDisplay(lv_display_t* disp, const lv_area_t* area, uint8_t* pixelMap) {
    if (!gfx || !pixelMap) {
        lv_display_flush_ready(disp);
        return;
    }
    const int32_t x1 = std::max<int32_t>(0, area->x1);
    const int32_t y1 = std::max<int32_t>(0, area->y1);
    const int32_t x2 = std::min<int32_t>(kWidth - 1, area->x2);
    const int32_t y2 = std::min<int32_t>(kHeight - 1, area->y2);
    if (x2 < x1 || y2 < y1) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t width = x2 - x1 + 1;
    const int32_t height = y2 - y1 + 1;
    if (flushCount < 6) {
        Serial.printf("[LVGL9] flush #%lu x=%ld y=%ld w=%ld h=%ld\n",
                      static_cast<unsigned long>(flushCount + 1),
                      static_cast<long>(x1),
                      static_cast<long>(y1),
                      static_cast<long>(width),
                      static_cast<long>(height));
    }
    ++flushCount;
    gfx->draw16bitRGBBitmap(x1, y1, reinterpret_cast<uint16_t*>(pixelMap), width, height);
    lv_display_flush_ready(disp);
}

bool readTouchPoint(uint16_t* x, uint16_t* y) {
    uint8_t data[7] = {};
    Wire.beginTransmission(kTouchAddress);
    Wire.write(static_cast<uint8_t>(0x00));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(kTouchAddress, sizeof(data)) != sizeof(data)) return false;
    for (uint8_t& byte : data) byte = Wire.read();
    if ((data[2] & 0x0f) == 0) return false;

    uint16_t px = ((data[3] & 0x0f) << 8) | data[4];
    uint16_t py = ((data[5] & 0x0f) << 8) | data[6];
    *x = clampX(px);
    *y = clampY(py);
    return true;
}

void readTouch(lv_indev_t*, lv_indev_data_t* data) {
    static uint16_t lastX = 0;
    static uint16_t lastY = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    if (readTouchPoint(&x, &y)) {
        lastX = x;
        lastY = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = lastX;
    data->point.y = lastY;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

void onPlayClicked(lv_event_t*) {
    playing = !playing;
    lv_label_set_text(playLabel, playing ? "PAUSE" : "PLAY");
}

void animTimer(lv_timer_t*) {
    static uint16_t value = 0;
    if (playing) value = (value + 9) % 1000;
    lv_bar_set_value(progress, value, LV_ANIM_ON);
    for (uint8_t i = 0; i < 14; ++i) {
        const uint8_t phase = (millis() / 70 + i * 3) % 18;
        const uint8_t height = 5 + (phase < 9 ? phase : 18 - phase) * 3;
        lv_obj_set_height(bars[i], height);
        lv_obj_set_y(bars[i], 124 - height);
    }
}

void buildUi() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080914), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t* top = lv_obj_create(screen);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, kWidth, 24);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0d1020), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    label(top, "12:30", 8, 3, &lv_font_montserrat_14, lv_color_white());
    label(top, "Wi-Fi  ONLINE   DAC PCM 44.1k   VOL 20", 70, 4, &lv_font_montserrat_12, lv_color_hex(0xd8d8f6));

    lv_obj_t* cover = lv_obj_create(screen);
    lv_obj_set_pos(cover, 12, 36);
    lv_obj_set_size(cover, 86, 86);
    lv_obj_set_style_radius(cover, 43, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x16192e), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0xc65cff), 0);
    lv_obj_set_style_border_width(cover, 2, 0);
    label(cover, "HiFi", 21, 27, &lv_font_montserrat_20, lv_color_hex(0xeeccff));

    label(screen, "LVGL Music Probe", 114, 41, &lv_font_montserrat_18, lv_color_white());
    label(screen, "320x170 / ST7789 / CST816", 114, 66, &lv_font_montserrat_12, lv_color_hex(0xb7b8cf));
    label(screen, "MP3 320k | 44.1kHz | 16bit", 114, 85, &lv_font_montserrat_12, lv_color_hex(0xa86cff));

    progress = lv_bar_create(screen);
    lv_obj_set_pos(progress, 114, 109);
    lv_obj_set_size(progress, 154, 5);
    lv_bar_set_range(progress, 0, 1000);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x24263b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0xc65cff), LV_PART_INDICATOR);

    for (uint8_t i = 0; i < 14; ++i) {
        bars[i] = lv_obj_create(screen);
        lv_obj_set_pos(bars[i], 276 + i * 3, 114);
        lv_obj_set_size(bars[i], 2, 10);
        lv_obj_set_style_bg_color(bars[i], lv_color_hex(i < 10 ? 0xc65cff : 0x00e5ff), 0);
        lv_obj_set_style_border_width(bars[i], 0, 0);
        lv_obj_set_style_radius(bars[i], 1, 0);
    }

    lv_obj_t* prev = lv_button_create(screen);
    lv_obj_set_pos(prev, 72, 135);
    lv_obj_set_size(prev, 58, 28);
    label(prev, "PREV", 10, 6, &lv_font_montserrat_12, lv_color_white());

    lv_obj_t* play = lv_button_create(screen);
    lv_obj_set_pos(play, 138, 130);
    lv_obj_set_size(play, 68, 36);
    lv_obj_set_style_border_color(play, lv_color_hex(0xc65cff), 0);
    lv_obj_set_style_border_width(play, 2, 0);
    lv_obj_add_event_cb(play, onPlayClicked, LV_EVENT_CLICKED, nullptr);
    playLabel = label(play, "PAUSE", 14, 9, &lv_font_montserrat_12, lv_color_white());

    lv_obj_t* next = lv_button_create(screen);
    lv_obj_set_pos(next, 214, 135);
    lv_obj_set_size(next, 58, 28);
    label(next, "NEXT", 10, 6, &lv_font_montserrat_12, lv_color_white());

    lv_timer_create(animTimer, 80, nullptr);
}
} // namespace

void setup() {
    Serial.begin(921600);
    delay(250);
    Serial.println("[LVGL9] probe boot");
    Serial.printf("[LVGL9] heap internal=%u psram=%u largest=%u\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);
    bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_CLK, LCD_DIN);
    gfx = new Arduino_ST7789(bus, LCD_RST, 0, false, 170, 320, 35, 0, 35, 0);
    if (!gfx || !gfx->begin(20000000)) {
        Serial.println("[LVGL9] LCD init failed");
        while (true) delay(1000);
    }
    gfx->setRotation(kDisplayRotation);
    drawFallback();

    Wire.begin(I2C_SDA, I2C_SCL, 400000);
    lv_init();

    const size_t bufferBytes = kWidth * kRows * sizeof(uint16_t);
    drawBufA = heap_caps_malloc(bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    drawBufB = heap_caps_malloc(bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    Serial.printf("[LVGL9] draw buffers %u bytes each A=%p B=%p\n", static_cast<unsigned>(bufferBytes), drawBufA, drawBufB);
    if (!drawBufA || !drawBufB) {
        Serial.println("[LVGL9] draw buffer alloc failed");
        while (true) delay(1000);
    }

    display = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flushDisplay);
    lv_display_set_buffers(display, drawBufA, drawBufB, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, readTouch);

    buildUi();
    lastTick = millis();
    Serial.println("[LVGL9] probe ready");
}

void loop() {
    const uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;
    lv_timer_handler();
    delay(5);
}
