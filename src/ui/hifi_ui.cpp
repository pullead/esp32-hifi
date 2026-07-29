#include "hifi_ui.h"
#include <extra/libs/qrcode/lv_qrcode.h> // not wired into lv_extra.h in this LVGL fork; include directly

#include "hifi_fonts.h"  // subsetted CJK fonts (lv_font_cjk_16 / _13)
#include "images/cassette_images.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Palette v2 (2026-07-23): switched from the dark graphite/copper spec to a
// light lavender/purple identity per explicit product direction -- see
// docs/UI_DESIGN_SPEC.md's v2 addendum. Layout grid (status bar/control bar/
// cover geometry) is unchanged; only tokens and a few radii/borders differ.
namespace {
// Local Now Playing spectrum canvas height (see buildLocalNowPlaying() and
// refreshLocalNowPlaying()) -- shared so the two can't drift out of sync.
constexpr lv_coord_t kSpecCanvasH = 43;

// v3 dark-neon tokens (docs/UI_DESIGN_SPEC.md v3). Near-black navy ground,
// neon-violet accent, green for confirmable "ON" state, magenta for spectrum.
const lv_color_t kBg = lv_color_hex(0x0A0B12);
const lv_color_t kStatusBar = lv_color_hex(0x05060A);
const lv_color_t kPanel = lv_color_hex(0x161A2B);
const lv_color_t kPanelDeep = lv_color_hex(0x0E1019);
const lv_color_t kInk = lv_color_hex(0xF2F3F7);
const lv_color_t kInkDim = lv_color_hex(0xB0B6C8); // brightened from 0x9AA0B4 -- lowest-contrast row in the font preview A/B
const lv_color_t kInkFaint = lv_color_hex(0x565C70);
const lv_color_t kAccent = lv_color_hex(0xA855F7);
const lv_color_t kAccentBright = lv_color_hex(0xC77DFF);
const lv_color_t kAccentDeep = lv_color_hex(0x6D28D9);
const lv_color_t kMagenta = lv_color_hex(0xD946EF);
const lv_color_t kOk = lv_color_hex(0x34D399);
const lv_color_t kLive = lv_color_hex(0x34D399);
const lv_color_t kMute = lv_color_hex(0xE4574B);
constexpr const char* kDefaultRadioUrl = "http://ice1.somafm.com/groovesalad-128-mp3";

// Shared control-bar action ids for both NowPlaying and Radio (buildMediaPage).
constexpr uintptr_t kActionPrev = 0;
constexpr uintptr_t kActionPlayPause = 1;
constexpr uintptr_t kActionNext = 2;
constexpr uintptr_t kActionOpenSettings = 3;
constexpr uintptr_t kActionOpenList = 4;
constexpr uintptr_t kActionBack = 5;

const char* stateText(PlayerTransport transport) {
    switch (transport) {
        case PlayerTransport::Playing: return "Playing";
        case PlayerTransport::Paused: return "Paused";
        case PlayerTransport::Buffering: return "Buffering";
        case PlayerTransport::Error: return "Stream error";
        default: return "Ready";
    }
}

void formatTime(char* output, size_t size, uint32_t seconds) {
    const uint32_t hours = seconds / 3600;
    if (hours) {
        snprintf(output, size, "%02lu:%02lu:%02lu", static_cast<unsigned long>(hours),
                 static_cast<unsigned long>((seconds / 60) % 60), static_cast<unsigned long>(seconds % 60));
    } else {
        snprintf(output, size, "%02lu:%02lu", static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
    }
}

// Instant touch-down feedback for every tappable element: a light accent
// wash appears the moment the finger lands, independent of whatever the
// action does afterwards. This is the single biggest fix for "I pressed it
// and nothing happened" -- previously buttons had no pressed state at all,
// so even a successful tap looked dead until the next 100ms refresh.
void addPressFx(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, kAccent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, kAccent, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
}

lv_obj_t* addFontPreviewLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                              lv_color_t color, int16_t x, int16_t y, int16_t width,
                              lv_label_long_mode_t longMode = LV_LABEL_LONG_DOT) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_label_set_long_mode(label, longMode);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

void addFontPreviewRow(lv_obj_t* parent, int16_t y, const char* name, const lv_font_t* font,
                       lv_color_t color, const char* sample, bool fauxBold = false,
                       lv_label_long_mode_t longMode = LV_LABEL_LONG_DOT) {
    addFontPreviewLabel(parent, name, &lv_font_montserrat_10, kInkDim, 10, y + 2, 72);
    addFontPreviewLabel(parent, sample, font, color, 84, y, 222, longMode);
    if (fauxBold) {
        addFontPreviewLabel(parent, sample, font, color, 85, y, 222, longMode);
    }
}

// Draws one Local Now Playing spectrum "LED" cell into the canvas pixel
// buffer -- shared by buildLocalNowPlaying()'s initial all-unlit fill and
// refreshLocalNowPlaying()'s per-tick column updates, see both for why this
// is a canvas rather than 308 individual lv_obj cells.
void drawSpecDot(lv_obj_t* canvas, uint8_t col, uint8_t row, lv_coord_t canvasH, lv_color_t color) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 1;
    // row 0 sits at the bottom of the canvas (3px cell height): local y
    // range for row r is [(canvasH-3)-r*4, (canvasH)-r*4).
    lv_canvas_draw_rect(canvas, col * 8, (canvasH - 3) - row * 4, 5, 3, &dsc);
}

// Gives a plain circle the read of a vinyl record: a couple of faint groove
// rings plus a colored center label with a spindle hole. These are children
// of `disc`, so they inherit its rotation (real vinyl labels spin with the
// record) -- only the readable glyph overlay (m_coverLabel) stays a sibling.
void addVinylDetail(lv_obj_t* disc, int16_t diameter) {
    const uint8_t grooveCount = diameter >= 50 ? 3 : 2;
    for (uint8_t g = 0; g < grooveCount; ++g) {
        const int16_t d = diameter - 8 - g * ((diameter - 16) / (grooveCount + 1));
        lv_obj_t* groove = lv_obj_create(disc);
        lv_obj_set_size(groove, d, d);
        lv_obj_center(groove);
        lv_obj_set_style_radius(groove, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(groove, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(groove, 1, 0);
        lv_obj_set_style_border_color(groove, kInkFaint, 0);
        lv_obj_set_style_border_opa(groove, LV_OPA_40, 0);
        lv_obj_set_style_pad_all(groove, 0, 0);
        lv_obj_clear_flag(groove, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(groove, LV_OBJ_FLAG_CLICKABLE);
    }
    const int16_t labelD = diameter * 42 / 100;
    lv_obj_t* label = lv_obj_create(disc);
    lv_obj_set_size(label, labelD, labelD);
    lv_obj_center(label);
    lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(label, kAccentDeep, 0);
    lv_obj_set_style_bg_grad_color(label, kMagenta, 0);
    lv_obj_set_style_bg_grad_dir(label, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    const int16_t holeD = std::max<int16_t>(3, diameter / 16);
    lv_obj_t* hole = lv_obj_create(disc);
    lv_obj_set_size(hole, holeD, holeD);
    lv_obj_center(hole);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, kBg, 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hole, 0, 0);
    lv_obj_set_style_pad_all(hole, 0, 0);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_CLICKABLE);
}

// Small procedural eighth-note glyph (notehead + stem + flag) for the
// vinyl label when a track has no cover art -- nicer than leaving the
// gradient label blank, and doesn't need a bitmap asset.
void addMusicNoteIcon(lv_obj_t* parent, int16_t labelD, lv_color_t color) {
    const int16_t headD = std::max<int16_t>(4, labelD * 30 / 100);
    const int16_t stemH = labelD * 55 / 100;
    const int16_t stemW = std::max<int16_t>(2, labelD * 9 / 100);
    const int16_t cx = labelD / 2;
    const int16_t cy = labelD / 2;
    const int16_t headX = cx - headD / 2 - stemW / 2;
    const int16_t headY = cy + stemH / 2 - headD / 2;

    lv_obj_t* stem = lv_obj_create(parent);
    lv_obj_set_size(stem, stemW, stemH);
    lv_obj_set_pos(stem, headX + headD - stemW, headY - stemH + headD / 2);
    lv_obj_set_style_radius(stem, 1, 0);
    lv_obj_set_style_bg_color(stem, color, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stem, 0, 0);
    lv_obj_set_style_pad_all(stem, 0, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* flag = lv_obj_create(parent);
    const int16_t flagW = std::max<int16_t>(3, labelD * 22 / 100);
    const int16_t flagH = std::max<int16_t>(3, labelD * 20 / 100);
    lv_obj_set_size(flag, flagW, flagH);
    lv_obj_set_pos(flag, headX + headD - stemW, headY - stemH + headD / 2);
    lv_obj_set_style_radius(flag, flagH / 2, 0);
    lv_obj_set_style_bg_color(flag, color, 0);
    lv_obj_set_style_bg_opa(flag, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(flag, 0, 0);
    lv_obj_set_style_pad_all(flag, 0, 0);
    lv_obj_clear_flag(flag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(flag, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* head = lv_obj_create(parent);
    lv_obj_set_size(head, headD, headD);
    lv_obj_set_pos(head, headX, headY);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, color, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
}

// Static tonearm accent for the turntable cover art (see reference: a
// pivot knob + angled arm + headshell resting near the disc's top-right
// edge). Purely decorative, doesn't move with playback -- a sibling of the
// disc, not a child, so it never rotates with it.
void addTonearmDetail(lv_obj_t* parent, int16_t x, int16_t y) {
    lv_obj_t* arm = lv_obj_create(parent);
    lv_obj_set_size(arm, 22, 3);
    lv_obj_set_pos(arm, x, y);
    lv_obj_set_style_radius(arm, 2, 0);
    lv_obj_set_style_bg_color(arm, kInkDim, 0);
    lv_obj_set_style_bg_opa(arm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(arm, 0, 0);
    lv_obj_set_style_pad_all(arm, 0, 0);
    lv_obj_set_style_transform_pivot_x(arm, 21, 0);
    lv_obj_set_style_transform_pivot_y(arm, 1, 0);
    lv_obj_set_style_transform_angle(arm, 1150, 0); // ~115deg, arm swung down-left onto the disc
    lv_obj_clear_flag(arm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(arm, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* pivot = lv_obj_create(parent);
    lv_obj_set_size(pivot, 6, 6);
    lv_obj_set_pos(pivot, x + 21 - 3, y + 1 - 3);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pivot, kInkDim, 0);
    lv_obj_set_style_bg_opa(pivot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pivot, 0, 0);
    lv_obj_set_style_pad_all(pivot, 0, 0);
    lv_obj_clear_flag(pivot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pivot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* head = lv_obj_create(parent);
    lv_obj_set_size(head, 5, 3);
    lv_obj_set_pos(head, x - 4, y + 15);
    lv_obj_set_style_radius(head, 1, 0);
    lv_obj_set_style_bg_color(head, kAccentBright, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
}

// Small procedural headphone glyph (arched band + two ear cups) -- LVGL's
// built-in symbol set has no headphone icon, only a speaker-style one.
// Purely decorative here (status bar), so unlike most of this file's
// widgets it's built once and never recolored.
void addHeadphoneIcon(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* band = lv_obj_create(parent);
    lv_obj_set_size(band, 10, 7);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(band, 5, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(band, color, 0);
    lv_obj_set_style_border_width(band, 2, 0);
    lv_obj_set_style_border_side(band, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    for (lv_align_t corner : {LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT}) {
        lv_obj_t* cup = lv_obj_create(parent);
        lv_obj_set_size(cup, 4, 5);
        lv_obj_align(cup, corner, 0, 0);
        lv_obj_set_style_radius(cup, 2, 0);
        lv_obj_set_style_bg_color(cup, color, 0);
        lv_obj_set_style_bg_opa(cup, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cup, 0, 0);
        lv_obj_set_style_pad_all(cup, 0, 0);
        lv_obj_clear_flag(cup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cup, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Three ascending-length bars for "sequential/in-order" playback -- an
// arrow (LV_SYMBOL_RIGHT) read as "skip forward" rather than "play in
// order", and LV_SYMBOL_LIST (equal-length bars) was already the list
// button elsewhere in this same control bar. Ascending bars are visually
// distinct from both and read naturally as "1, 2, 3, in sequence". Returns
// the 3 bar objects (shortest to longest) so the caller can recolor them
// when the play mode changes, unlike this file's other static icon helpers.
void addSequentialIcon(lv_obj_t* parent, lv_color_t color, lv_obj_t* outBars[3]) {
    constexpr int16_t kWidths[3] = {6, 10, 14};
    for (uint8_t i = 0; i < 3; ++i) {
        lv_obj_t* bar = lv_obj_create(parent);
        lv_obj_set_size(bar, kWidths[i], 2);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, (i - 1) * 5);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        outBars[i] = bar;
    }
}

// Small procedural cassette-tape glyph (rounded body outline + two reel
// dots) for the view-toggle button -- LV_SYMBOL_LOOP was a generic
// "repeat" arrow with no relation to what the button actually does.
void addCassetteIcon(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* body = lv_obj_create(parent);
    lv_obj_set_size(body, 22, 14);
    lv_obj_center(body);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, color, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    for (int8_t side = 0; side < 2; ++side) {
        lv_obj_t* reel = lv_obj_create(body);
        lv_obj_set_size(reel, 5, 5);
        lv_obj_align(reel, LV_ALIGN_CENTER, side ? 6 : -6, 1);
        lv_obj_set_style_radius(reel, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(reel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(reel, color, 0);
        lv_obj_set_style_border_width(reel, 1, 0);
        lv_obj_set_style_pad_all(reel, 0, 0);
        lv_obj_clear_flag(reel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(reel, LV_OBJ_FLAG_CLICKABLE);
    }
}
} // namespace

HifiUi* HifiUi::s_instance = nullptr;

bool HifiUi::begin() {
    printf("[LVGL] HifiUi begin\n");
    if (!m_port.begin()) return false;
    s_instance = this;
    playerService.begin();
    buildQuickPanel(); // on lv_layer_top(), built once, outlives every show() screen swap
    show(Page::Home);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    for (uint8_t i = 0; i < 5; ++i) {
        m_port.tick();
        delay(20);
    }
    printf("[LVGL] HifiUi ready\n");
    return true;
}

void HifiUi::tick() {
    m_port.tick();
    TouchGesture gesture = TouchGesture::None;
    while (m_port.consumeGesture(&gesture)) handleGesture(gesture);
    if (millis() - m_lastRefresh >= 60) { // was 100ms; faster for a smoother-looking spectrum
        m_lastRefresh = millis();
        playerService.tick();
        refresh();
        if (m_quickPanelOpen) refreshQuickPanel();
    }
}

// Status bar, left to right: back/time, WiFi, DAC sample rate, amp (headset)
// state, codec, volume -- this exact grouping/order was specified against a
// wider reference design; compressed to fit our real 320px panel by
// dropping that reference's verbose Chinese word labels in favor of icons
// and short numeric/latin tags (there just isn't room for e.g. "耳放 ON" as
// full text at a legible size here).
void HifiUi::buildStatusBar(lv_obj_t* screen) {
    lv_obj_t* bar = lv_obj_create(screen);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 320, 20);
    lv_obj_set_style_bg_color(bar, kStatusBar, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2A2050), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // 1: back chevron, non-Home only (Home is the hub, nothing to go back
    // to). Small Fitts-friendly corner target, matches the reference's
    // leftmost slot.
    const bool showBack = m_page != Page::Home;
    if (showBack) {
        lv_obj_t* back = lv_btn_create(screen);
        lv_obj_set_pos(back, 0, 0);
        lv_obj_set_size(back, 26, 20);
        lv_obj_set_style_radius(back, 6, 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_pad_all(back, 0, 0);
        lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(back);
        lv_obj_add_event_cb(back, onTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionBack));
        lv_obj_t* chev = makeText(back, LV_SYMBOL_LEFT, &lv_font_montserrat_12, kAccent, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    }
    // 2: real time (blank until NTP syncs -- see refresh()). Shown on every
    // page now, not just Home, per the requested ordering (back+time as a
    // pair, not mutually exclusive).
    m_statusTime = makeText(bar, "--:--", &lv_font_montserrat_10, kInk, LV_ALIGN_LEFT_MID, 28, 0);

    // 3: WiFi -- icon only, no separate bar graph. The icon's own color
    // carries signal strength (grey disconnected, orange weak, green
    // strong) -- see refresh().
    m_statusWifiIcon = makeText(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_10, kInkFaint, LV_ALIGN_LEFT_MID, 68, 0);

    // Headphone icon -- purely decorative, fills the space the WiFi bar
    // graph used to occupy, positioned just left of the sample rate.
    lv_obj_t* headphoneWrap = lv_obj_create(bar);
    lv_obj_set_size(headphoneWrap, 12, 12);
    lv_obj_align(headphoneWrap, LV_ALIGN_LEFT_MID, 88, 0);
    lv_obj_set_style_bg_opa(headphoneWrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(headphoneWrap, 0, 0);
    lv_obj_set_style_pad_all(headphoneWrap, 0, 0);
    lv_obj_clear_flag(headphoneWrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(headphoneWrap, LV_OBJ_FLAG_CLICKABLE);
    addHeadphoneIcon(headphoneWrap, kInkFaint);

    // 4: DAC sample rate/bit depth tag (also doubles as the mute/buffering
    // indicator, same as before -- those states are more urgent than the
    // sample rate and fully replace it when active).
    m_statusTag = makeText(bar, "", &lv_font_montserrat_10, kInkDim, LV_ALIGN_LEFT_MID, 108, 0);

    // 5: amp status as a bordered "DAC" tag (was a volume-style icon) --
    // box and text both go green when the amp is actually producing
    // audible sound (see refresh()); AMP_ENABLED itself is just held HIGH
    // for the board's whole lifetime (setupLvglRuntime()), so this reflects
    // real playback state, not the GPIO pin.
    m_statusAmpBox = lv_obj_create(bar);
    lv_obj_set_size(m_statusAmpBox, 32, 14);
    lv_obj_align(m_statusAmpBox, LV_ALIGN_LEFT_MID, 158, 0);
    lv_obj_set_style_radius(m_statusAmpBox, 3, 0);
    lv_obj_set_style_bg_opa(m_statusAmpBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(m_statusAmpBox, kInkFaint, 0);
    lv_obj_set_style_border_width(m_statusAmpBox, 1, 0);
    lv_obj_set_style_pad_all(m_statusAmpBox, 0, 0);
    lv_obj_clear_flag(m_statusAmpBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_statusAmpBox, LV_OBJ_FLAG_CLICKABLE);
    m_statusAmp = makeText(m_statusAmpBox, "DAC", &lv_font_montserrat_10, kInkFaint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(m_statusAmp, LV_OBJ_FLAG_CLICKABLE);

    // 6: codec tag (MP3/AAC/FLAC/...) -- stands in for the reference
    // design's "音质" (sound quality) slot; there's no real "gain mode"
    // setting in the audio backend to show instead, and the actual format
    // is genuinely more informative.
    m_statusCodec = makeText(bar, "", &lv_font_montserrat_10, kInkDim, LV_ALIGN_LEFT_MID, 196, 0);

    // 7: volume, packed from the right edge: percentage, then the 5-bar
    // graph, then a speaker icon -- reads "28% ▁▂▃▄▅ 🔊" right-to-left.
    m_statusVolPct = makeText(bar, "", &lv_font_montserrat_10, kInkDim, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_t* volWrap = lv_obj_create(bar);
    lv_obj_set_size(volWrap, 24, 12);
    lv_obj_align(volWrap, LV_ALIGN_RIGHT_MID, -32, 0);
    lv_obj_set_style_bg_opa(volWrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volWrap, 0, 0);
    lv_obj_set_style_pad_all(volWrap, 0, 0);
    lv_obj_clear_flag(volWrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(volWrap, LV_OBJ_FLAG_CLICKABLE);
    const int16_t heights[5] = {4, 6, 8, 10, 12};
    for (uint8_t i = 0; i < 5; ++i) {
        lv_obj_t* seg = lv_obj_create(volWrap);
        lv_obj_set_size(seg, 2, heights[i]);
        lv_obj_align(seg, LV_ALIGN_RIGHT_MID, -(4 - i) * 4, 0);
        lv_obj_set_style_bg_color(seg, kInkFaint, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_radius(seg, 1, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        m_volumeBars[i] = seg;
    }
    makeText(bar, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_10, kInkFaint, LV_ALIGN_RIGHT_MID, -58, 0);
}

lv_obj_t* HifiUi::makePanel(lv_obj_t* parent, int16_t x, int16_t y, int16_t width, int16_t height, bool highlight) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, kPanel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, highlight ? kAccent : kInkFaint, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

// One 64x46 hit-target column. Icon-only slots get a small caption underneath
// so button meaning is never ambiguous (this was a concrete complaint against
// the previous UI). The primary (play/pause) slot gets a ring mark instead.
lv_obj_t* HifiUi::makeControlSlot(lv_obj_t* parent, const char* symbol, uintptr_t action, int16_t slotIndex, bool primary) {
    lv_obj_t* slot = lv_btn_create(parent);
    lv_obj_set_pos(slot, slotIndex * 64, 0);
    lv_obj_set_size(slot, 64, 46);
    lv_obj_set_style_radius(slot, 10, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(slot, 0, 0);
    lv_obj_set_style_border_width(slot, 0, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    addPressFx(slot);
    lv_obj_add_event_cb(slot, onTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(action));

    if (primary) {
        lv_obj_t* ring = lv_obj_create(slot);
        lv_obj_set_size(ring, 40, 40);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, -3);
        lv_obj_set_style_radius(ring, 20, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, kAccent, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_shadow_width(ring, 0, 0); // no glow on the play button, by request
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        m_playRing = ring;
        m_playIcon = makeText(ring, symbol, &lv_font_montserrat_16, kAccentBright, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(m_playIcon, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t* icon = makeText(slot, symbol, &lv_font_montserrat_16, kInkDim, LV_ALIGN_CENTER, 0, -6);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
    return slot;
}

lv_obj_t* HifiUi::makeText(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color, lv_align_t align, int16_t x, int16_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_align(label, align, x, y);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

lv_obj_t* HifiUi::makeCard(lv_obj_t* parent, const char* icon, const char* label, Page page, int16_t x, int16_t y, int16_t width, int16_t height) {
    lv_obj_t* card = lv_btn_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_bg_color(card, kPanel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    const bool glow = page == Page::NowPlaying;
    lv_obj_set_style_border_color(card, glow ? kAccent : kInkFaint, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(card, kAccent, 0);
    lv_obj_set_style_shadow_width(card, glow ? 10 : 0, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    addPressFx(card);
    lv_obj_add_event_cb(card, onHomeAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(page)));

    // Icon badge: a small filled circle behind the glyph instead of a bare
    // symbol floating on the card -- reads as an intentional icon rather
    // than a placeholder character.
    lv_obj_t* badge = lv_obj_create(card);
    lv_obj_set_size(badge, 26, 26);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(badge, 13, 0);
    lv_obj_set_style_bg_color(badge, glow ? kAccentDeep : kPanelDeep, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, glow ? kAccentBright : kInkFaint, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* iconLabel = makeText(badge, icon, &lv_font_montserrat_16, glow ? kAccentBright : kInkDim, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(iconLabel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* text = makeText(card, label, &lv_font_cjk_13, kInk, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_long_mode(text, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(text, width - 6);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    return card;
}

void HifiUi::show(Page page) {
    // Genuinely entering the WiFi settings page from elsewhere (not
    // refreshSettingsWifi() rebuilding it in place after a saved-list change)
    // resets back to the plain saved-network list rather than leaving the
    // management QR sub-view showing from a previous visit.
    if (page == Page::SettingsWifi && m_page != Page::SettingsWifi) {
        m_wifiShowManageQr = false;
        m_wifiShowAddNetwork = false;
        m_wifiAddStage = WifiAddStage::ScanList;
        m_wifiLastSavedCount = 0xFF;
    }
    m_returnPage = m_page;
    m_page = page;
    m_title = m_detail = m_techLine = m_progress = m_elapsed = m_total = nullptr;
    m_statusTag = m_statusTime = nullptr;
    m_statusWifiIcon = m_statusAmpBox = nullptr;
    m_homeClockHour = m_homeClockMinute = m_homeClockDate = m_homeClockWeather = nullptr;
    m_weatherIconBody = m_weatherIconLobe = m_weatherIconDrop1 = m_weatherIconDrop2 = nullptr;
    m_cover = m_coverLabel = m_playIcon = nullptr;
    m_playRing = nullptr;
    for (auto& bar : m_ringBars) bar = nullptr;
    m_coverSpinning = false;
    m_statusAmp = m_statusCodec = m_statusVolPct = nullptr;
    m_homeNowTitle = m_homeNowDetail = m_homePlayIcon = m_homeProgress = nullptr;
    m_coverArtWrap = m_coverArtImg = nullptr; // widgets only -- m_coverArtPixels/Dsc survive the rebuild
    m_coverArtSpinning = false;
    for (auto& seg : m_volumeBars) seg = nullptr;
    m_specCanvas = nullptr; // widget only -- m_specCanvasBuf (PSRAM pixels) survives the rebuild, same as cover art
    m_specCols = m_specRows = 0;
    for (auto& lit : m_specLastLit) lit = 0xFF; // force a full repaint on first refresh after rebuild
    m_shuffleIcon = nullptr;
    m_seqIconWrap = nullptr;
    for (auto& bar : m_seqIconBars) bar = nullptr;
    m_cassetteReelL = m_cassetteReelR = m_cassetteNeedle = nullptr;
    m_cassetteSpinning = false;
    m_wifiQr = m_wifiStatusText = m_wifiHintText = nullptr;
    m_wifiQrLastContent[0] = '\0'; // force a fresh lv_qrcode_update on rebuild
    m_wifiNetworkList = nullptr;
    m_wifiAddPwField = m_wifiAddKeyboard = nullptr;
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, kBg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* oldScreen = lv_scr_act();
    lv_scr_load(screen); // instant switch, no transition animation, by request
    // Delete the outgoing screen BEFORE building the new one's content, not
    // after: with the old order, re-navigating to the same heavy page (e.g.
    // every transport button tap rebuilds Local Now Playing's 308-cell
    // dot-matrix spectrum from scratch) briefly held two full copies of that
    // page's widgets in LVGL's fixed memory pool at once -- the actual
    // cause of a StoreProhibited crash inside lv_obj_create on every
    // prev/next/play/pause tap. All the member widget pointers that could
    // reference the old screen's children were already nulled above, so
    // nothing dangles once it's gone.
    if (oldScreen && oldScreen != screen) lv_obj_del(oldScreen);
    if (page == Page::Home) buildHome();
    else if (page == Page::NowPlaying) buildMediaPage(false);
    else if (page == Page::Radio) buildMediaPage(true);
    else if (page == Page::RadioList) buildRadioList();
    else if (page == Page::Sd) buildLocalMusic();
    else if (page == Page::LocalNowPlaying) buildLocalNowPlaying();
    else if (page == Page::Clock) buildPlaceholder("CLOCK", "NTP time, alarm and sleep timer");
    else if (page == Page::Settings) buildSettings();
    else if (page == Page::SettingsWifi) buildSettingsWifi();
    else if (page == Page::FontPreview) buildFontPreview();
    else buildPlaceholder("SETTINGS / EQ", "Audio, EQ, network and sleep");
}

void HifiUi::buildHome() {
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen);

    // Clock is real (RTC via state.timeHM/dateStr). Weather is real too, via
    // Open-Meteo (state.weatherTempC/weatherDesc) -- see fetchWeatherOnce()
    // in main.cpp; both show a "--" placeholder until their respective
    // source (RTC/SNTP, WiFi weather fetch) has data.
    lv_obj_t* clock = makePanel(screen, 8, 24, 126, 80, false);
    lv_obj_t* hour = makePanel(clock, 13, 14, 45, 32, false);
    lv_obj_t* minute = makePanel(clock, 68, 14, 45, 32, false);
    m_homeClockHour = makeText(hour, "--", &lv_font_montserrat_28, kInk, LV_ALIGN_CENTER, 0, -1);
    m_homeClockMinute = makeText(minute, "--", &lv_font_montserrat_28, kInk, LV_ALIGN_CENTER, 0, -1);
    m_homeClockDate = makeText(clock, "", &lv_font_cjk_13, kInkDim, LV_ALIGN_BOTTOM_MID, 0, -19);

    // Weather row: a small procedural icon (no image decoder is enabled in
    // this LVGL build, see lv_conf.h -- LV_USE_PNG/SJPG are both 0) built
    // from plain circles, next to the real temperature. refresh() re-colors
    ///shows-hides the pieces per state.weatherIcon instead of rebuilding.
    lv_obj_t* weatherRow = lv_obj_create(clock);
    lv_obj_set_size(weatherRow, 108, 14);
    lv_obj_align(weatherRow, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(weatherRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weatherRow, 0, 0);
    lv_obj_set_style_pad_all(weatherRow, 0, 0);
    lv_obj_clear_flag(weatherRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(weatherRow, LV_OBJ_FLAG_CLICKABLE);

    m_weatherIconBody = lv_obj_create(weatherRow);
    lv_obj_set_size(m_weatherIconBody, 10, 10);
    lv_obj_set_pos(m_weatherIconBody, 0, 2);
    lv_obj_set_style_radius(m_weatherIconBody, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(m_weatherIconBody, 0, 0);
    lv_obj_set_style_pad_all(m_weatherIconBody, 0, 0);
    lv_obj_clear_flag(m_weatherIconBody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_weatherIconBody, LV_OBJ_FLAG_CLICKABLE);

    m_weatherIconLobe = lv_obj_create(weatherRow);
    lv_obj_set_size(m_weatherIconLobe, 7, 7);
    lv_obj_set_pos(m_weatherIconLobe, 6, 6);
    lv_obj_set_style_radius(m_weatherIconLobe, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(m_weatherIconLobe, kInkFaint, 0);
    lv_obj_set_style_border_width(m_weatherIconLobe, 0, 0);
    lv_obj_set_style_pad_all(m_weatherIconLobe, 0, 0);
    lv_obj_add_flag(m_weatherIconLobe, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m_weatherIconLobe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_weatherIconLobe, LV_OBJ_FLAG_CLICKABLE);

    m_weatherIconDrop1 = lv_obj_create(weatherRow);
    m_weatherIconDrop2 = lv_obj_create(weatherRow);
    int16_t dropX = 2;
    for (lv_obj_t* drop : {m_weatherIconDrop1, m_weatherIconDrop2}) {
        lv_obj_set_size(drop, 2, 3);
        lv_obj_set_pos(drop, dropX, 10);
        lv_obj_set_style_radius(drop, 1, 0);
        lv_obj_set_style_border_width(drop, 0, 0);
        lv_obj_set_style_pad_all(drop, 0, 0);
        lv_obj_add_flag(drop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(drop, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(drop, LV_OBJ_FLAG_CLICKABLE);
        dropX += 6;
    }

    m_homeClockWeather = makeText(weatherRow, "", &lv_font_cjk_13, kInkDim, LV_ALIGN_LEFT_MID, 16, 0);

    // Now-playing panel with quick transport controls (per reference: a tap
    // on prev/play/next here works without opening the full player).
    lv_obj_t* now = makePanel(screen, 142, 24, 170, 80, false);
    makeText(now, LV_SYMBOL_AUDIO " 正在播放", &lv_font_cjk_13, kAccentBright, LV_ALIGN_TOP_LEFT, 8, 6);

    // Mini spectrum ring around the mini disc, same shared logic (and same
    // m_ringBars/m_cover members) as the big NowPlaying disc -- see
    // refreshCoverSpin(). Local coords are relative to `now`, matching the
    // disc below.
    constexpr int16_t kHomeRingCx = 25, kHomeRingCy = 41, kHomeRingR = 20;
    m_ringCx = kHomeRingCx;
    m_ringCy = kHomeRingCy;
    m_ringR = kHomeRingR;
    for (uint8_t i = 0; i < 8; ++i) {
        const float a = (static_cast<float>(i) / 8.0f) * 2.0f * 3.14159265f;
        lv_obj_t* bar = lv_obj_create(now);
        lv_obj_set_size(bar, 2, 4);
        lv_obj_set_pos(bar, kHomeRingCx + static_cast<int16_t>(cosf(a) * kHomeRingR) - 1,
                        kHomeRingCy + static_cast<int16_t>(sinf(a) * kHomeRingR) - 2);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, i % 2 ? kMagenta : kAccent, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        m_ringBars[i] = bar;
    }
    for (uint8_t i = 8; i < 10; ++i) m_ringBars[i] = nullptr;

    m_cover = lv_obj_create(now);
    lv_obj_set_pos(m_cover, 8, 24);
    lv_obj_set_size(m_cover, 34, 34);
    lv_obj_set_style_radius(m_cover, 17, 0);
    lv_obj_set_style_bg_color(m_cover, kBg, 0);
    lv_obj_set_style_bg_grad_color(m_cover, kPanelDeep, 0);
    lv_obj_set_style_bg_grad_dir(m_cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(m_cover, kAccent, 0);
    lv_obj_set_style_border_width(m_cover, 1, 0);
    lv_obj_set_style_shadow_color(m_cover, kAccent, 0);
    lv_obj_set_style_shadow_width(m_cover, 8, 0);
    lv_obj_set_style_shadow_opa(m_cover, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(m_cover, 0, 0);
    lv_obj_set_style_transform_pivot_x(m_cover, 17, 0);
    lv_obj_set_style_transform_pivot_y(m_cover, 17, 0);
    lv_obj_clear_flag(m_cover, LV_OBJ_FLAG_SCROLLABLE);
    addVinylDetail(m_cover, 34);
    m_coverLabel = lv_label_create(now);
    lv_label_set_text(m_coverLabel, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(m_coverLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(m_coverLabel, kAccentBright, 0);
    lv_obj_align(m_coverLabel, LV_ALIGN_TOP_LEFT, 8 + 17 - 6, 24 + 17 - 7);
    lv_obj_clear_flag(m_coverLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m_coverLabel, LV_OBJ_FLAG_SCROLLABLE);
    m_homeNowTitle = makeText(now, "未在播放", &lv_font_cjk_13, kInk, LV_ALIGN_TOP_LEFT, 48, 24);
    lv_label_set_long_mode(m_homeNowTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_size(m_homeNowTitle, 114, 16);
    m_homeNowDetail = makeText(now, "电台 / 本地 / 网络", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_LEFT, 48, 40);
    lv_label_set_long_mode(m_homeNowDetail, LV_LABEL_LONG_DOT);
    lv_obj_set_size(m_homeNowDetail, 114, 14);
    m_homeProgress = lv_bar_create(now);
    lv_obj_set_pos(m_homeProgress, 8, 62);
    lv_obj_set_size(m_homeProgress, 100, 3);
    lv_bar_set_range(m_homeProgress, 0, 1000);
    lv_obj_set_style_radius(m_homeProgress, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(m_homeProgress, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m_homeProgress, kInkFaint, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_homeProgress, kAccent, LV_PART_INDICATOR);
    // quick prev/play/next -- same shared action ids as the full player
    lv_obj_t* qPrev = lv_btn_create(now);
    lv_obj_set_pos(qPrev, 112, 56);
    lv_obj_set_size(qPrev, 18, 18);
    lv_obj_set_style_radius(qPrev, 9, 0);
    lv_obj_set_style_bg_opa(qPrev, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(qPrev, 0, 0);
    lv_obj_set_style_shadow_width(qPrev, 0, 0);
    lv_obj_set_style_pad_all(qPrev, 0, 0);
    lv_obj_clear_flag(qPrev, LV_OBJ_FLAG_SCROLLABLE);
    addPressFx(qPrev);
    lv_obj_add_event_cb(qPrev, onTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionPrev));
    lv_obj_t* qPrevIcon = makeText(qPrev, LV_SYMBOL_PREV, &lv_font_montserrat_12, kInkDim, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(qPrevIcon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* qPlay = lv_btn_create(now);
    lv_obj_set_pos(qPlay, 134, 54);
    lv_obj_set_size(qPlay, 22, 22);
    lv_obj_set_style_radius(qPlay, 11, 0);
    lv_obj_set_style_bg_opa(qPlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(qPlay, kAccent, 0);
    lv_obj_set_style_border_width(qPlay, 1, 0);
    lv_obj_set_style_shadow_color(qPlay, kAccent, 0);
    lv_obj_set_style_shadow_width(qPlay, 6, 0);
    lv_obj_set_style_pad_all(qPlay, 0, 0);
    lv_obj_clear_flag(qPlay, LV_OBJ_FLAG_SCROLLABLE);
    addPressFx(qPlay);
    lv_obj_add_event_cb(qPlay, onTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionPlayPause));
    m_homePlayIcon = makeText(qPlay, LV_SYMBOL_PLAY, &lv_font_montserrat_12, kAccentBright, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(m_homePlayIcon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* qNext = lv_btn_create(now);
    lv_obj_set_pos(qNext, 160, 56);
    lv_obj_set_size(qNext, 18, 18);
    lv_obj_set_style_radius(qNext, 9, 0);
    lv_obj_set_style_bg_opa(qNext, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(qNext, 0, 0);
    lv_obj_set_style_shadow_width(qNext, 0, 0);
    lv_obj_set_style_pad_all(qNext, 0, 0);
    lv_obj_clear_flag(qNext, LV_OBJ_FLAG_SCROLLABLE);
    addPressFx(qNext);
    lv_obj_add_event_cb(qNext, onTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionNext));
    lv_obj_t* qNextIcon = makeText(qNext, LV_SYMBOL_NEXT, &lv_font_montserrat_12, kInkDim, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(qNextIcon, LV_OBJ_FLAG_CLICKABLE);

    // Touch log (2026-07-24) caught real taps for PLAY/RADIO landing at
    // y=169 -- 5px below the old height=52 bottom edge (164) -- while a tap
    // at y=148 hit LOCAL correctly. Extending to the screen edge (height 58)
    // removes that dead strip instead of asking users to aim more precisely.
    makeCard(screen, LV_SYMBOL_PLAY, "播放", Page::NowPlaying, 8, 112, 56, 58);
    makeCard(screen, LV_SYMBOL_AUDIO, "电台", Page::Radio, 72, 112, 56, 58);
    makeCard(screen, LV_SYMBOL_SD_CARD, "本地", Page::Sd, 136, 112, 56, 58);
    makeCard(screen, LV_SYMBOL_LIST, "解码", Page::Clock, 200, 112, 56, 58);
    makeCard(screen, LV_SYMBOL_SETTINGS, "设置", Page::Settings, 264, 112, 48, 58);
}

// Shared Now Playing / Radio skeleton -- one page grid for both sources, per
// UI_DESIGN_SPEC.md's field-mapping table. Only labels/semantics differ.
void HifiUi::buildMediaPage(bool isRadio) {
    m_mediaPageIsRadio = isRadio;
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen);

    // Vinyl treatment: a ring of real audio-reactive spectrum bars (driven
    // by state.vuLevel, not decorative) around a circular cover disc that
    // spins slowly while playing. No baked image asset yet -- see
    // docs/UI_DESIGN_SPEC.md -- but this reads far closer to it than a
    // static bordered square.
    constexpr int16_t kRingCx = 52, kRingCy = 72, kRingR = 40;
    m_ringCx = kRingCx;
    m_ringCy = kRingCy;
    m_ringR = kRingR;
    for (uint8_t i = 0; i < 10; ++i) {
        const float a = (static_cast<float>(i) / 10.0f) * 2.0f * 3.14159265f;
        lv_obj_t* bar = lv_obj_create(screen);
        lv_obj_set_size(bar, 3, 6);
        lv_obj_set_pos(bar, kRingCx + static_cast<int16_t>(cosf(a) * kRingR) - 1,
                        kRingCy + static_cast<int16_t>(sinf(a) * kRingR) - 3);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, i % 2 ? kMagenta : kAccent, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        m_ringBars[i] = bar;
    }

    // Vinyl disc: near-black body (not the panel gray) with baked-in groove
    // rings and a colored center label, via addVinylDetail -- see its
    // comment for why the grooves/label are children (they spin with the
    // disc) while the readable glyph below is kept a sibling.
    m_cover = lv_obj_create(screen);
    lv_obj_set_pos(m_cover, kRingCx - 32, kRingCy - 32);
    lv_obj_set_size(m_cover, 64, 64);
    lv_obj_set_style_radius(m_cover, 32, 0);
    lv_obj_set_style_bg_color(m_cover, kBg, 0);
    lv_obj_set_style_bg_grad_color(m_cover, kPanelDeep, 0);
    lv_obj_set_style_bg_grad_dir(m_cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(m_cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m_cover, kAccentDeep, 0);
    lv_obj_set_style_border_width(m_cover, 1, 0);
    lv_obj_set_style_border_opa(m_cover, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(m_cover, 0, 0);
    lv_obj_set_style_pad_all(m_cover, 0, 0);
    lv_obj_set_style_transform_pivot_x(m_cover, 32, 0);
    lv_obj_set_style_transform_pivot_y(m_cover, 32, 0);
    lv_obj_clear_flag(m_cover, LV_OBJ_FLAG_SCROLLABLE);
    addVinylDetail(m_cover, 64);

    // Real embedded cover art (see loadCoverArt(), only ever populated by
    // tapping a track in the Local Music browser) replaces the plain
    // groove-label puck when we have it. Kept a non-spinning sibling like
    // m_coverLabel below -- a spinning low-res photo at this size reads as
    // noise, not motion.
    const bool haveArt = !isRadio && m_coverArtPixels && m_coverArtDsc.header.w && m_coverArtDsc.header.h;
    if (haveArt) {
        m_coverArtWrap = lv_obj_create(screen);
        lv_obj_set_pos(m_coverArtWrap, kRingCx - 30, kRingCy - 30);
        lv_obj_set_size(m_coverArtWrap, 60, 60);
        lv_obj_set_style_radius(m_coverArtWrap, 30, 0);
        lv_obj_set_style_clip_corner(m_coverArtWrap, true, 0);
        lv_obj_set_style_bg_opa(m_coverArtWrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(m_coverArtWrap, 0, 0);
        lv_obj_set_style_pad_all(m_coverArtWrap, 0, 0);
        lv_obj_clear_flag(m_coverArtWrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(m_coverArtWrap, LV_OBJ_FLAG_CLICKABLE);
        m_coverArtImg = lv_img_create(m_coverArtWrap);
        lv_img_set_src(m_coverArtImg, &m_coverArtDsc);
        lv_obj_center(m_coverArtImg);
        lv_obj_clear_flag(m_coverArtImg, LV_OBJ_FLAG_CLICKABLE);
    }

    // The label is a sibling on top, not a child of the disc -- the disc's
    // grooves rotate to sell the spin, but the glyph/monogram must stay
    // upright and legible instead of spinning with it. Skipped when real
    // art is already showing, to avoid a glyph floating over the photo.
    if (!haveArt) {
        m_coverLabel = lv_label_create(screen);
        lv_label_set_text(m_coverLabel, isRadio ? "RADIO" : LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(m_coverLabel, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(m_coverLabel, kAccentBright, 0);
        lv_obj_align(m_coverLabel, LV_ALIGN_TOP_LEFT, kRingCx - 32, kRingCy - 10);
        lv_obj_set_width(m_coverLabel, 64);
        lv_obj_set_style_text_align(m_coverLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(m_coverLabel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(m_coverLabel, LV_OBJ_FLAG_SCROLLABLE);
    }

    m_title = makeText(screen, "", &lv_font_montserrat_16, kInk, LV_ALIGN_TOP_LEFT, 104, 20);
    lv_label_set_long_mode(m_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m_title, 206);
    m_detail = makeText(screen, "", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_LEFT, 104, 44);
    lv_label_set_long_mode(m_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m_detail, 206);
    m_techLine = makeText(screen, "", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_LEFT, 104, 62);
    lv_label_set_long_mode(m_techLine, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m_techLine, 206);

    m_progress = lv_bar_create(screen);
    lv_obj_set_pos(m_progress, 104, 86);
    lv_obj_set_size(m_progress, 206, 4);
    lv_bar_set_range(m_progress, 0, 1000);
    lv_obj_set_style_radius(m_progress, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(m_progress, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m_progress, kInkFaint, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_progress, isRadio ? kLive : kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(m_progress, isRadio ? kLive : kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(m_progress, 6, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(m_progress, LV_OPA_60, LV_PART_INDICATOR);

    m_elapsed = makeText(screen, "00:00", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_LEFT, 104, 92);
    m_total = makeText(screen, "00:00", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_RIGHT, -10, 92);

    lv_obj_t* controlBar = lv_obj_create(screen);
    lv_obj_set_pos(controlBar, 0, 124);
    lv_obj_set_size(controlBar, 320, 46);
    lv_obj_set_style_bg_opa(controlBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controlBar, 1, 0);
    lv_obj_set_style_border_side(controlBar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(controlBar, kInkFaint, 0);
    lv_obj_set_style_border_opa(controlBar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(controlBar, 0, 0);
    lv_obj_set_style_radius(controlBar, 0, 0);
    lv_obj_clear_flag(controlBar, LV_OBJ_FLAG_SCROLLABLE);

    makeControlSlot(controlBar, LV_SYMBOL_SETTINGS, kActionOpenSettings, 0);
    makeText(controlBar, "EQ", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_LEFT, 24, 32);
    makeControlSlot(controlBar, LV_SYMBOL_PREV, kActionPrev, 1);
    makeControlSlot(controlBar, LV_SYMBOL_PLAY, kActionPlayPause, 2, true);
    makeControlSlot(controlBar, LV_SYMBOL_NEXT, kActionNext, 3);
    makeControlSlot(controlBar, LV_SYMBOL_LIST, kActionOpenList, 4);
    makeText(controlBar, isRadio ? "STATIONS" : "LIST", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_RIGHT, -8, 32);
}

void HifiUi::buildRadioList() {
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen); // back chevron lives in the status bar now

    const uint16_t stationCount = playerService.radioStationCount();
    char header[48];
    snprintf(header, sizeof(header), "STATIONS  %u", stationCount);
    makeText(screen, header, &lv_font_montserrat_14, kInk, LV_ALIGN_TOP_LEFT, 56, 26);
    makeText(screen, "tap to play", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_RIGHT, -10, 27);

    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_set_pos(list, 8, 48);
    lv_obj_set_size(list, 304, 90);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    if (!stationCount) {
        makeText(list, "No stations. Import with Web UI.", &lv_font_montserrat_12, kInkDim, LV_ALIGN_CENTER, 0, 0);
    } else {
        const uint16_t visibleCount = std::min<uint16_t>(stationCount, 80);
        for (uint16_t i = 1; i <= visibleCount; ++i) {
            RadioStationItem station{};
            if (!playerService.radioStation(i, &station)) continue;
            const bool active = i == 1;
            const int16_t y = static_cast<int16_t>((i - 1) * 40); // 36px row + 4px gap, per the 36-40px row-height guide
            lv_obj_t* row = lv_btn_create(list);
            lv_obj_set_pos(row, 0, y);
            lv_obj_set_size(row, 278, 36);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_bg_color(row, active ? kAccentDeep : kPanel, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_shadow_color(row, kAccent, 0);
            lv_obj_set_style_shadow_width(row, active ? 8 : 0, 0);
            lv_obj_set_style_shadow_opa(row, LV_OPA_50, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            addPressFx(row);
            lv_obj_add_event_cb(row, onRadioStationAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

            lv_obj_t* name = makeText(row, station.name[0] ? station.name : "Unknown station", &lv_font_cjk_13, kInk, LV_ALIGN_LEFT_MID, 10, -7);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name, 210);
            lv_obj_t* country = makeText(row, station.country[0] ? station.country : "--", &lv_font_cjk_13, active ? kInkDim : kInkFaint, LV_ALIGN_LEFT_MID, 10, 8);
            lv_label_set_long_mode(country, LV_LABEL_LONG_DOT);
            lv_obj_set_width(country, 210);
            if (active) makeText(row, LV_SYMBOL_AUDIO, &lv_font_montserrat_12, kAccentBright, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }
}

void HifiUi::buildLocalMusic() {
    lv_obj_t* screen = lv_scr_act();
    // No status bar here, unlike every other page -- per feedback, this
    // screen is scrolled constantly and the extra ~20px of list space
    // matters more than the persistent clock/WiFi/volume readout. Back
    // navigation still works via the left-edge swipe gesture (handleGesture's
    // default EdgeBack case), same fallback the cassette view relies on.
    // Everything below is shifted up by the 20px the status bar used to take.

    // Tabs: Songs / Artists / Albums, per the "分类,比如歌曲/歌手/专辑" ask.
    // Tapping Artists/Albums lists unique names; tapping a name filters
    // Songs down to just that group (onMusicGroupAction).
    static const char* kTabLabels[3] = {"歌曲", "歌手", "专辑"};
    for (uint8_t i = 0; i < 3; ++i) {
        lv_obj_t* tab = lv_btn_create(screen);
        lv_obj_set_pos(tab, 8 + i * 60, 6);
        lv_obj_set_size(tab, 54, 24);
        lv_obj_set_style_radius(tab, 12, 0);
        lv_obj_set_style_bg_color(tab, m_musicTab == i ? kAccentDeep : kPanel, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_shadow_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(tab);
        lv_obj_add_event_cb(tab, onMusicTabAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_t* label = makeText(tab, kTabLabels[i], &lv_font_cjk_13, m_musicTab == i ? kInk : kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }

    const bool scanning = playerService.localLibraryScanning();
    const uint16_t trackCount = playerService.localLibraryCount();
    const bool filtered = m_musicTab == 0 && (m_musicFilterArtist[0] || m_musicFilterAlbum[0]);
    if (filtered) {
        lv_obj_t* clear = lv_btn_create(screen);
        lv_obj_set_pos(clear, 192, 6);
        lv_obj_set_size(clear, 120, 24);
        lv_obj_set_style_radius(clear, 12, 0);
        lv_obj_set_style_bg_color(clear, kPanel, 0);
        lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(clear, 0, 0);
        lv_obj_set_style_shadow_width(clear, 0, 0);
        lv_obj_set_style_pad_all(clear, 0, 0);
        lv_obj_clear_flag(clear, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(clear);
        lv_obj_add_event_cb(clear, onMusicClearFilterAction, LV_EVENT_CLICKED, nullptr);
        char chip[40];
        snprintf(chip, sizeof(chip), "%s " LV_SYMBOL_CLOSE, m_musicFilterArtist[0] ? m_musicFilterArtist : m_musicFilterAlbum);
        lv_obj_t* label = makeText(clear, chip, &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, 108);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    } else {
        char header[32];
        if (scanning) snprintf(header, sizeof(header), "扫描中… %u", trackCount);
        else snprintf(header, sizeof(header), "共 %u 首", trackCount);
        makeText(screen, header, &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_RIGHT, -10, 10);
    }

    // Narrower than before (304->286px) to make room for the draggable
    // scroll slider at x=296..312 (see below) -- LVGL's own built-in
    // scrollbar is a visual indicator only, not touch-draggable, so a real
    // lv_slider stands in for "designed for touch scrubbing".
    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_set_pos(list, 8, 36);
    lv_obj_set_size(list, 286, 128);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF); // replaced by the draggable slider

    m_musicGroupCount = 0;
    int32_t contentHeight = 0; // populated below, used to size the draggable scroll slider

    if (m_musicTab != 0) {
        // Artists / Albums: unique names collected from the scanned tracks.
        for (uint16_t i = 0; i < trackCount && m_musicGroupCount < kMaxMusicGroups; ++i) {
            LocalTrackItem item{};
            if (!playerService.localTrack(i, &item)) continue;
            const char* name = m_musicTab == 1 ? item.artist : item.album;
            if (!name[0]) continue;
            bool seen = false;
            for (uint8_t g = 0; g < m_musicGroupCount; ++g) {
                if (strcmp(m_musicGroupNames[g], name) == 0) { seen = true; break; }
            }
            if (!seen) strlcpy(m_musicGroupNames[m_musicGroupCount++], name, sizeof(m_musicGroupNames[0]));
        }
        if (!m_musicGroupCount) {
            makeText(list, scanning ? "扫描中…" : "没有找到标签信息", &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
        } else {
            for (uint8_t g = 0; g < m_musicGroupCount; ++g) {
                lv_obj_t* row = lv_btn_create(list);
                lv_obj_set_pos(row, 0, g * 40);
                lv_obj_set_size(row, 278, 36);
                lv_obj_set_style_radius(row, 10, 0);
                lv_obj_set_style_bg_color(row, kPanel, 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_shadow_width(row, 0, 0);
                lv_obj_set_style_pad_all(row, 0, 0);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
                addPressFx(row);
                lv_obj_add_event_cb(row, onMusicGroupAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(g)));
                lv_obj_t* label = makeText(row, m_musicGroupNames[g], &lv_font_cjk_13, kInk, LV_ALIGN_LEFT_MID, 10, 0);
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
                lv_obj_set_width(label, 232);
                lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
            }
            contentHeight = static_cast<int32_t>(m_musicGroupCount) * 40;
        }
    } else if (!trackCount) {
        makeText(list, scanning ? "正在扫描SD卡…" : "未找到音乐文件", &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
    } else {
        uint16_t row_y = 0;
        uint16_t shown = 0;
        for (uint16_t i = 0; i < trackCount; ++i) {
            LocalTrackItem item{};
            if (!playerService.localTrack(i, &item)) continue;
            if (m_musicFilterArtist[0] && strcmp(item.artist, m_musicFilterArtist) != 0) continue;
            if (m_musicFilterAlbum[0] && strcmp(item.album, m_musicFilterAlbum) != 0) continue;

            lv_obj_t* row = lv_btn_create(list);
            lv_obj_set_pos(row, 0, row_y);
            lv_obj_set_size(row, 278, 40);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_bg_color(row, kPanel, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            addPressFx(row);
            lv_obj_add_event_cb(row, onMusicTrackAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));

            const char* titleText = item.title[0] ? item.title : "未知曲目";
            lv_obj_t* title = makeText(row, titleText, &lv_font_cjk_13, kInk, LV_ALIGN_LEFT_MID, 10, -10);
            lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
            lv_obj_set_width(title, 178);
            // No bold variant of this baked CJK font exists (would need a
            // full font-asset regen), so a second copy of the same label
            // 1px to the right, redrawn on top, fakes heavier strokes --
            // a common cheap trick for embedded UIs without a bold font.
            lv_obj_t* titleBold = makeText(row, titleText, &lv_font_cjk_13, kInk, LV_ALIGN_LEFT_MID, 11, -10);
            lv_label_set_long_mode(titleBold, LV_LABEL_LONG_DOT);
            lv_obj_set_width(titleBold, 178);
            char sub[96];
            snprintf(sub, sizeof(sub), "%s%s%s", item.artist[0] ? item.artist : "未知艺术家", item.album[0] ? " · " : "", item.album);
            lv_obj_t* detail = makeText(row, sub, &lv_font_cjk_13, kInkFaint, LV_ALIGN_LEFT_MID, 10, 10);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            lv_obj_set_width(detail, 178);
            if (item.hasArt) makeText(row, LV_SYMBOL_IMAGE, &lv_font_montserrat_12, kAccentBright, LV_ALIGN_RIGHT_MID, -10, 0);

            row_y += 44;
            ++shown;
        }
        if (!shown) makeText(list, "该分类下没有曲目", &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
        contentHeight = static_cast<int32_t>(row_y);
    }

    // Draggable scroll slider, replacing LVGL's non-interactive built-in
    // scrollbar -- an lv_slider IS natively touch-draggable, so reusing one
    // here (instead of building a custom thumb from scratch) gets "designed
    // for touch scrubbing" almost for free. Only shown when there's
    // actually more content than fits, matching the old AUTO scrollbar's
    // own behavior of hiding when nothing needs scrolling.
    constexpr int32_t kListHeight = 128;
    if (contentHeight > kListHeight) {
        lv_obj_t* scrollSlider = lv_slider_create(screen);
        lv_obj_set_pos(scrollSlider, 300, 36);
        lv_obj_set_size(scrollSlider, 12, kListHeight);
        // Range is in scroll-Y pixels directly -- 0 (top) to how far the
        // list can actually scroll, so the slider's value always maps
        // 1:1 onto lv_obj_scroll_to_y() with no extra unit conversion.
        lv_slider_set_range(scrollSlider, 0, contentHeight - kListHeight);
        lv_obj_set_style_radius(scrollSlider, 6, LV_PART_MAIN);
        lv_obj_set_style_radius(scrollSlider, 6, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(scrollSlider, kPanel, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scrollSlider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scrollSlider, kAccentDeep, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(scrollSlider, kAccentBright, LV_PART_KNOB);
        // Oversized knob (touch target, not just a thin handle) -- the
        // whole point is this needs to be easy to grab and drag on a small
        // touchscreen, not just visually indicate position like the old
        // scrollbar did.
        lv_obj_set_style_pad_all(scrollSlider, 8, LV_PART_KNOB);
        lv_obj_add_event_cb(
            scrollSlider,
            [](lv_event_t* e) {
                auto* target = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
                lv_obj_t* slider = lv_event_get_target(e);
                lv_obj_scroll_to_y(target, lv_slider_get_value(slider), LV_ANIM_OFF);
            },
            LV_EVENT_VALUE_CHANGED, list);
        // Keep the slider in sync when the list is scrolled by a direct
        // finger-drag/flick on the list itself, not just via the slider.
        lv_obj_add_event_cb(
            list,
            [](lv_event_t* e) {
                auto* slider = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
                lv_obj_t* scrolled = lv_event_get_target(e);
                lv_slider_set_value(slider, lv_obj_get_scroll_y(scrolled), LV_ANIM_OFF);
            },
            LV_EVENT_SCROLL, scrollSlider);
    }
}

// Dedicated Now Playing screen for tracks opened from the Local Music
// browser -- distinct from buildMediaPage(), which stays the shared radio/
// generic "resume viewing playback" skeleton reached from Home's PLAY card.
// Bigger art + a wider spectrum ring, and prev/next walk the local library
// (respecting the current artist/album filter) instead of radio stations.
void HifiUi::buildLocalNowPlaying() {
    lv_obj_t* screen = lv_scr_act();

    if (m_localCassetteView) {
        // Full-screen, no status bar, no buttons at all -- purely visual,
        // per "除了磁带不需要任何按钮". Exit is a left-edge swipe-right only
        // (handleGesture's EdgeBack case special-cases this page+mode).
        buildCassetteVisual(screen);
        refreshLocalNowPlaying(playerService.snapshot());
        return;
    }

    buildStatusBar(screen);
    {
        // Flat "waveform card" player. Title/artist/duration at top; a
        // dot-matrix spectrum (classic LED-VU-meter look) below that; a
        // thick draggable seek slider at the bottom.
        // Card grew 94->106px (controlBar shrank to match, see below) to
        // give the play area more room and stop the progress slider/cover
        // art from crowding the card edges.
        lv_obj_t* card = makePanel(screen, 8, 26, 304, 106, false);
        // No visible card at all, not just no border -- makePanel's filled
        // rounded-rect background was still reading as a boxed region even
        // with the border removed. Fully transparent so the cover/spectrum/
        // progress content floats directly on the screen background.
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);

        // Plain square cover art, no vinyl framing and no border (per
        // feedback -- the turntable look was reverted). Grown 62->68px and
        // the whole spectrum/progress/title column shifted right by 8px
        // (see below) to give it the freed-up space on both sides.
        const bool haveArt = m_coverArtPixels && m_coverArtDsc.header.w && m_coverArtDsc.header.h;
        if (haveArt) {
            m_coverArtWrap = lv_obj_create(card);
            lv_obj_set_pos(m_coverArtWrap, 0, 5);
            lv_obj_set_size(m_coverArtWrap, 72, 72);
            lv_obj_set_style_radius(m_coverArtWrap, 0, 0); // square corners, per feedback
            lv_obj_set_style_clip_corner(m_coverArtWrap, true, 0);
            lv_obj_set_style_bg_color(m_coverArtWrap, kPanelDeep, 0);
            lv_obj_set_style_border_width(m_coverArtWrap, 0, 0);
            lv_obj_set_style_pad_all(m_coverArtWrap, 0, 0);
            lv_obj_clear_flag(m_coverArtWrap, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(m_coverArtWrap, LV_OBJ_FLAG_CLICKABLE);
            m_coverArtImg = lv_img_create(m_coverArtWrap);
            lv_img_set_src(m_coverArtImg, &m_coverArtDsc);
            lv_obj_center(m_coverArtImg);
            lv_obj_clear_flag(m_coverArtImg, LV_OBJ_FLAG_CLICKABLE);
        } else {
            // No art: a nice music-note glyph on a plain rounded panel,
            // instead of the old speaker-symbol placeholder.
            m_cover = lv_obj_create(card);
            lv_obj_set_pos(m_cover, 0, 5);
            lv_obj_set_size(m_cover, 72, 72);
            lv_obj_set_style_radius(m_cover, 0, 0); // square corners, per feedback
            lv_obj_set_style_bg_color(m_cover, kPanelDeep, 0);
            lv_obj_set_style_bg_grad_color(m_cover, kAccentDeep, 0);
            lv_obj_set_style_bg_grad_dir(m_cover, LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_border_width(m_cover, 0, 0);
            lv_obj_set_style_pad_all(m_cover, 0, 0);
            lv_obj_clear_flag(m_cover, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(m_cover, LV_OBJ_FLAG_CLICKABLE);
            addMusicNoteIcon(m_cover, 72, kInk);
        }

        // Title+artist combined into one looping marquee line (was two
        // static rows) -- frees a row for the lyric line below it. LVGL's
        // built-in circular scroll mode does the actual looping.
        // CJK font, not Montserrat (which has zero CJK glyphs and rendered
        // every non-ASCII title as tofu) -- cjk_13's line_height (17) matches
        // this slot exactly, so no layout shift needed.
        m_title = makeText(card, "", &lv_font_cjk_13, kInk, LV_ALIGN_TOP_LEFT, 80, 2);
        lv_label_set_long_mode(m_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(m_title, 216);

        // Current synchronized-lyrics line (see playerService.loadLyrics(),
        // ID3 SYLT frame) -- single line, replaced wholesale on each line
        // change rather than scrolled, per "单句显示快速切换". Doubles as a
        // "未找到歌词，点击重试" retry button when the online lookup came up
        // empty (see refreshLocalNowPlaying() and onLyricRetryAction()) --
        // harmless to leave clickable the rest of the time, since the
        // handler itself no-ops unless that state actually applies.
        m_detail = makeText(card, "", &lv_font_cjk_13, kAccentBright, LV_ALIGN_TOP_LEFT, 80, 19);
        lv_label_set_long_mode(m_detail, LV_LABEL_LONG_DOT);
        lv_obj_set_width(m_detail, 216);
        lv_obj_add_flag(m_detail, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(m_detail, onLyricRetryAction, LV_EVENT_CLICKED, nullptr);

        // Elapsed/total under the cover art, centered to match its new
        // 2..74px span (72px wide, grown from 68).
        m_elapsed = makeText(card, "00:00", &lv_font_montserrat_12, kInkDim, LV_ALIGN_TOP_LEFT, 0, 76);
        lv_obj_set_width(m_elapsed, 72);
        lv_obj_set_style_text_align(m_elapsed, LV_TEXT_ALIGN_CENTER, 0);
        m_total = makeText(card, "00:00", &lv_font_montserrat_12, kInkFaint, LV_ALIGN_TOP_LEFT, 0, 92);
        lv_obj_set_width(m_total, 72);
        lv_obj_set_style_text_align(m_total, LV_TEXT_ALIGN_CENTER, 0);

        // Dot-matrix spectrum: drawn as one lv_canvas instead of 308
        // individual "LED" cell objects (columns still map to the real
        // 6-band FFT, see refreshLocalNowPlaying -- only the rendering
        // changed, not what it shows). 308 separate lv_obj cells meant 308
        // separate invalidate/redraw pipelines; even with the "only touch
        // changed columns" optimization already in place, a typical tick
        // with several columns moving at once still queued many small,
        // scattered redraw regions, which is what actually caused the
        // visible stutter. A single canvas collapses that to one draw
        // buffer write + one invalidate per tick, however many columns
        // changed. Same geometry as before: 28 cols x 11 rows, 5x3px cells
        // on an 8x4px pitch, spanning x=80..301 (221px) / y=39..82 (43px).
        constexpr uint8_t kSpecCols = 28;
        constexpr uint8_t kSpecRows = 11;
        constexpr lv_coord_t kSpecCanvasW = 221; // last column: c=27 -> local x 216..221
        if (!m_specCanvasBuf) {
            m_specCanvasBuf = static_cast<lv_color_t*>(ps_malloc(kSpecCanvasW * kSpecCanvasH * sizeof(lv_color_t)));
        }
        m_specCanvas = lv_canvas_create(card);
        lv_obj_set_pos(m_specCanvas, 80, 39);
        lv_obj_clear_flag(m_specCanvas, LV_OBJ_FLAG_CLICKABLE);
        if (m_specCanvasBuf) {
            lv_canvas_set_buffer(m_specCanvas, m_specCanvasBuf, kSpecCanvasW, kSpecCanvasH, LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(m_specCanvas, kBg, LV_OPA_COVER); // card itself is transparent -- match the screen behind it
            for (uint8_t c = 0; c < kSpecCols; ++c) {
                for (uint8_t r = 0; r < kSpecRows; ++r) {
                    drawSpecDot(m_specCanvas, c, r, kSpecCanvasH, kInkFaint);
                }
            }
        }
        m_specCols = kSpecCols;
        m_specRows = kSpecRows;

        // Thick, draggable seek slider -- replaces the old thin read-only
        // bar. Position is only pushed from playback state when the user
        // isn't actively dragging it (see refreshLocalNowPlaying).
        // Width matches the spectrum grid's own span exactly (last dot's
        // right edge is 80 + 27*8 + 5 = 301), so both sit flush left and
        // right instead of the slider falling a few px short.
        m_progress = lv_slider_create(card);
        lv_obj_set_pos(m_progress, 80, 87);
        lv_obj_set_size(m_progress, 221, 12);
        lv_slider_set_range(m_progress, 0, 1000);
        lv_obj_set_style_radius(m_progress, 6, LV_PART_MAIN);
        lv_obj_set_style_radius(m_progress, 6, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(m_progress, kInkFaint, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(m_progress, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(m_progress, kMagenta, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_color(m_progress, kMagenta, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_width(m_progress, 4, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_opa(m_progress, LV_OPA_50, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(m_progress, kInk, LV_PART_KNOB);
        lv_obj_set_style_pad_all(m_progress, 6, LV_PART_KNOB);
        lv_obj_add_event_cb(m_progress, onLocalSeekAction, LV_EVENT_RELEASED, nullptr);
    }

    // Local-library-aware transport: prev/next walk s_localTracks (through
    // playerService), respecting whichever artist/album filter was active
    // in the browser -- NOT radio stations, unlike onTransportAction. Six
    // evenly-spaced slots, no overlap (the old list/toggle corner buttons
    // physically overlapped the next-track slot -- that was the "排列不
    // 整齐" clutter): shuffle / prev / play / next / list / cassette-toggle.
    // Narrower than before (46->36px) to give the card above more room --
    // the card grew by the same amount this bar gave up.
    lv_obj_t* controlBar = lv_obj_create(screen);
    lv_obj_set_pos(controlBar, 0, 134);
    lv_obj_set_size(controlBar, 320, 36);
    lv_obj_set_style_bg_opa(controlBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controlBar, 1, 0);
    lv_obj_set_style_border_side(controlBar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(controlBar, kInkFaint, 0);
    lv_obj_set_style_border_opa(controlBar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(controlBar, 0, 0);
    lv_obj_set_style_radius(controlBar, 0, 0);
    lv_obj_clear_flag(controlBar, LV_OBJ_FLAG_SCROLLABLE);

    constexpr uint8_t kSlotCount = 6;
    constexpr int16_t kSlotWidth = 320 / kSlotCount; // 53px
    static const char* kSlotSymbols[kSlotCount] = {LV_SYMBOL_SHUFFLE, LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_LIST, LV_SYMBOL_LOOP};
    for (uint8_t i = 0; i < kSlotCount; ++i) {
        const bool primary = i == 2; // play
        lv_obj_t* slot = lv_btn_create(controlBar);
        lv_obj_set_pos(slot, i * kSlotWidth, 0);
        lv_obj_set_size(slot, kSlotWidth, 36);
        lv_obj_set_style_radius(slot, 10, 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(slot, 0, 0);
        lv_obj_set_style_border_width(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(slot);
        if (primary) {
            lv_obj_add_event_cb(slot, onLocalTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionPlayPause));
            lv_obj_t* ring = lv_obj_create(slot);
            lv_obj_set_size(ring, 32, 32);
            lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_radius(ring, 20, 0);
            lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(ring, kMagenta, 0);
            lv_obj_set_style_border_width(ring, 2, 0);
            lv_obj_set_style_shadow_width(ring, 0, 0);
            lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
            m_playRing = ring;
            m_playIcon = makeText(ring, kSlotSymbols[i], &lv_font_montserrat_16, kAccentBright, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(m_playIcon, LV_OBJ_FLAG_CLICKABLE);
        } else if (i == 1) {
            lv_obj_add_event_cb(slot, onLocalTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionPrev));
            lv_obj_t* icon = makeText(slot, kSlotSymbols[i], &lv_font_montserrat_16, kInkDim, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        } else if (i == 3) {
            lv_obj_add_event_cb(slot, onLocalTransportAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(kActionNext));
            lv_obj_t* icon = makeText(slot, kSlotSymbols[i], &lv_font_montserrat_16, kInkDim, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        } else if (i == 0) {
            // Play-mode button: cycles Sequential -> RepeatAll -> RepeatOne
            // -> Shuffle on each tap (see onLocalPlayModeToggleAction),
            // driving both manual prev/next (findLocalTrack) and what
            // happens when a track ends on its own (refresh()'s eofCount
            // handling) -- not just a decorative shuffle-only toggle.
            lv_obj_add_event_cb(slot, onLocalPlayModeToggleAction, LV_EVENT_CLICKED, nullptr);
            {
                const bool isSequential = m_localPlayMode == LocalPlayMode::Sequential;
                const lv_color_t iconColor = isSequential ? kInkDim : kAccentBright;
                // Both representations are always built (custom ascending
                // bars for Sequential, text symbol for the other 3 modes)
                // and toggled via HIDDEN, rather than destroyed/recreated on
                // every mode change -- simpler and cheaper than rebuilding.
                m_seqIconWrap = lv_obj_create(slot);
                lv_obj_set_size(m_seqIconWrap, 14, 12);
                lv_obj_align(m_seqIconWrap, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_bg_opa(m_seqIconWrap, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(m_seqIconWrap, 0, 0);
                lv_obj_set_style_pad_all(m_seqIconWrap, 0, 0);
                lv_obj_clear_flag(m_seqIconWrap, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(m_seqIconWrap, LV_OBJ_FLAG_CLICKABLE);
                addSequentialIcon(m_seqIconWrap, iconColor, m_seqIconBars);
                if (!isSequential) lv_obj_add_flag(m_seqIconWrap, LV_OBJ_FLAG_HIDDEN);

                m_shuffleIcon = makeText(slot, localPlayModeSymbol(m_localPlayMode), &lv_font_montserrat_16, iconColor, LV_ALIGN_CENTER, 0, 0);
                lv_obj_clear_flag(m_shuffleIcon, LV_OBJ_FLAG_CLICKABLE);
                if (isSequential) lv_obj_add_flag(m_shuffleIcon, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (i == 4) {
            // NOT onTransportAction/kActionOpenList -- that opens the RADIO
            // station list, wrong list for a local-library-aware page.
            lv_obj_add_event_cb(slot, onMusicClearFilterAction, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* icon = makeText(slot, kSlotSymbols[i], &lv_font_montserrat_16, kInkDim, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        } else { // i == 5: cassette view toggle
            lv_obj_add_event_cb(slot, onLocalViewToggleAction, LV_EVENT_CLICKED, nullptr);
            addCassetteIcon(slot, kInkDim);
        }
    }

    refreshLocalNowPlaying(playerService.snapshot());
}

// Full-screen skeuomorphic cassette-tape alternate view for Local Now
// Playing. No status bar, no buttons of any kind -- purely visual, per
// explicit request. The only way out is a left-edge swipe-right (see
// handleGesture's EdgeBack case, special-cased for this page+mode instead
// of its usual "go Home"). Reels spin for real while playing, slowly (see
// refreshLocalNowPlaying) -- a cassette reel turns far slower than a vinyl
// disc.
void HifiUi::buildCassetteVisual(lv_obj_t* screen) {
    // Real bitmap art (see docs/cassette-design/) instead of hand-drawn
    // LVGL primitives -- procedural shapes can't get close to a
    // photographic cream/aged-plastic look. The body is one static
    // full-screen image; the reels are a separate small image so they can
    // actually rotate (lv_img_set_angle(), a properly-supported LVGL8
    // path) without the redraw glitch that hit the old composite-object
    // reels (rotating a parent obj with children didn't reliably
    // invalidate the children's area).
    static lv_img_dsc_t bodyDsc{};
    static lv_img_dsc_t reelDsc{};
    static bool descInit = false;
    if (!descInit) {
        bodyDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        bodyDsc.header.always_zero = 0;
        bodyDsc.header.w = cassette_body_w;
        bodyDsc.header.h = cassette_body_h;
        bodyDsc.data_size = static_cast<uint32_t>(cassette_body_w) * cassette_body_h * 2;
        bodyDsc.data = cassette_body_map;

        reelDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        reelDsc.header.always_zero = 0;
        reelDsc.header.w = cassette_reel_w;
        reelDsc.header.h = cassette_reel_h;
        reelDsc.data_size = static_cast<uint32_t>(cassette_reel_w) * cassette_reel_h * 2;
        reelDsc.data = cassette_reel_map;
        descInit = true;
    }

    lv_obj_t* bodyImg = lv_img_create(screen);
    lv_img_set_src(bodyImg, &bodyDsc);
    lv_obj_set_pos(bodyImg, 0, 0);
    lv_obj_clear_flag(bodyImg, LV_OBJ_FLAG_CLICKABLE);

    // Text positions match the cream label rect baked into the body image
    // (cassette_body.html: cassette margin 10px + .label at 26,24, size
    // 248x78 -> absolute 36,34..284,112).
    const lv_color_t kLabelInk = lv_color_hex(0x2b2620);
    const lv_color_t kLabelDim = lv_color_hex(0x8a8070);
    m_title = makeText(screen, "", &lv_font_cjk_13, kLabelInk, LV_ALIGN_TOP_LEFT, 36, 38);
    lv_label_set_long_mode(m_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m_title, 248);
    lv_obj_set_style_text_align(m_title, LV_TEXT_ALIGN_CENTER, 0);
    m_detail = makeText(screen, "", &lv_font_cjk_13, kLabelDim, LV_ALIGN_TOP_LEFT, 36, 54);
    lv_label_set_long_mode(m_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m_detail, 248);
    lv_obj_set_style_text_align(m_detail, LV_TEXT_ALIGN_CENTER, 0);

    // Tape-position needle: a real LVGL bar, not baked into the body image,
    // so it can actually move with playback ("随着音乐...左右移动") --
    // deflects right from a left rest position as real signal level rises,
    // like a classic VU meter, driven by the same 6-band FFT as the flat
    // card's spectrum (see refreshLocalNowPlaying). Window inner bounds are
    // x=76..244 (cassette_body.html's .window, absolute), travel kept a
    // little inside that at 84..228 so the bar never touches the edges.
    m_cassetteNeedle = lv_obj_create(screen);
    lv_obj_set_pos(m_cassetteNeedle, 83, 76);
    lv_obj_set_size(m_cassetteNeedle, 2, 30);
    lv_obj_set_style_bg_color(m_cassetteNeedle, lv_color_hex(0xff6a2b), 0);
    lv_obj_set_style_bg_opa(m_cassetteNeedle, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(m_cassetteNeedle, lv_color_hex(0xff6a2b), 0);
    lv_obj_set_style_shadow_width(m_cassetteNeedle, 4, 0);
    lv_obj_set_style_shadow_opa(m_cassetteNeedle, LV_OPA_70, 0);
    lv_obj_set_style_border_width(m_cassetteNeedle, 0, 0);
    lv_obj_set_style_radius(m_cassetteNeedle, 0, 0);
    lv_obj_set_style_pad_all(m_cassetteNeedle, 0, 0);
    lv_obj_clear_flag(m_cassetteNeedle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_cassetteNeedle, LV_OBJ_FLAG_CLICKABLE);

    // Reel windows: absolute 18,40 (left) / 250,40 (right), 52x52 -- matches
    // cassette_body.html's .reel positions (margin 10 + reel offset).
    for (uint8_t side = 0; side < 2; ++side) {
        lv_obj_t* wrap = lv_obj_create(screen);
        lv_obj_set_pos(wrap, side ? 250 : 18, 40);
        lv_obj_set_size(wrap, 52, 52);
        lv_obj_set_style_radius(wrap, 26, 0);
        lv_obj_set_style_clip_corner(wrap, true, 0);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* reelImg = lv_img_create(wrap);
        lv_img_set_src(reelImg, &reelDsc);
        lv_obj_center(reelImg);
        lv_obj_clear_flag(reelImg, LV_OBJ_FLAG_CLICKABLE);

        if (side == 0) m_cassetteReelL = reelImg;
        else m_cassetteReelR = reelImg;
    }
}

void HifiUi::refreshLocalNowPlaying(const PlayerSnapshot& state) {
    // Pick up a completed background online-lyrics fetch (see
    // player_service's lyricsOnlineReady / main.cpp's lyricsFetchTask):
    // only matters if it was for the track still on screen -- loadLyrics()
    // just re-checks its own cache, which the fetch task already populated
    // on success, so this is cheap even when it wasn't a match.
    uint16_t readyIdx = 0;
    if (playerService.lyricsOnlineReady(&readyIdx) && readyIdx == m_currentLocalTrackIndex) {
        m_hasLyrics = playerService.loadLyrics(readyIdx);
    }
    if (m_localCassetteView) {
        // Cassette label layout: title and artist are their own lines,
        // matching the physical cassette label's two printed rows.
        if (m_title) lv_label_set_text(m_title, state.title[0] ? state.title : "本地播放");
        if (m_detail) lv_label_set_text(m_detail, state.detail[0] ? state.detail : "");
    } else {
        // Flat card: title+artist combined into one looping marquee line,
        // and the row below shows the current synced-lyrics line instead
        // of a static artist repeat.
        char combinedTitle[160];
        if (state.title[0] && state.detail[0]) snprintf(combinedTitle, sizeof(combinedTitle), "%s - %s", state.title, state.detail);
        else snprintf(combinedTitle, sizeof(combinedTitle), "%s", state.title[0] ? state.title : "本地播放");
        if (m_title) lv_label_set_text(m_title, combinedTitle);
        if (m_detail) {
            if (m_hasLyrics) {
                const char* lyric = playerService.currentLyricLine(state.positionSeconds * 1000UL);
                lv_label_set_text(m_detail, (lyric && lyric[0]) ? lyric : "");
            } else {
                // Distinguishes "still looking" / "looked, found nothing" from
                // a blanket "没有歌词信息" -- see onLyricRetryAction() for the
                // tap-to-retry half of this.
                switch (playerService.lyricsFetchState(m_currentLocalTrackIndex)) {
                    case LyricFetchState::Pending: lv_label_set_text(m_detail, "正在联网查询歌词..."); break;
                    case LyricFetchState::NotFound: lv_label_set_text(m_detail, "未找到歌词，点击重试"); break;
                    default: lv_label_set_text(m_detail, "没有歌词信息"); break;
                }
            }
        }
    }
    if (m_elapsed) {
        char elapsed[16];
        formatTime(elapsed, sizeof(elapsed), state.positionSeconds);
        lv_label_set_text(m_elapsed, elapsed);
    }
    if (m_total) {
        char total[16];
        formatTime(total, sizeof(total), state.durationSeconds);
        lv_label_set_text(m_total, total);
    }

    const bool playing = state.transport == PlayerTransport::Playing || state.transport == PlayerTransport::Buffering;

    // Don't fight the user's finger: only push the playback position into
    // the slider while they aren't actively dragging it.
    if (m_progress && !lv_obj_has_state(m_progress, LV_STATE_PRESSED)) {
        const uint32_t value = state.durationSeconds ? (state.positionSeconds * 1000UL / state.durationSeconds) : 0;
        lv_slider_set_value(m_progress, value, LV_ANIM_OFF);
    }

    // Dot-matrix spectrum: real 6-band FFT (Audio::getSpectrumBands(),
    // bass..treble) -- each column reads its own band's actual level, so
    // the shape genuinely reflects low/mid/high content, not one uniform
    // pulse reused everywhere.
    // Skip the spectrum entirely while the slider is being dragged -- that's
    // exactly when touch processing is already busiest, and the spectrum
    // redraw isn't what the user is looking at in that moment anyway.
    const bool seeking = m_progress && lv_obj_has_state(m_progress, LV_STATE_PRESSED);
    if (!seeking && m_specCanvas && m_specCols && m_specRows) {
        // Rainbow row palette (bottom->top), classic spectrum-analyzer
        // "tech" look -- purely a row-index -> color lookup, not tied to
        // any new data (the real signal is still just lit-count per band).
        static const lv_color_t kRowColors[11] = {
            lv_color_hex(0x38BDF8), lv_color_hex(0x22D3EE), lv_color_hex(0x2DD4BF), lv_color_hex(0x34D399), lv_color_hex(0xA3E635),
            lv_color_hex(0xFACC15), lv_color_hex(0xFB923C), lv_color_hex(0xF97316), lv_color_hex(0xEF4444), lv_color_hex(0xEC4899),
            lv_color_hex(0xF43F5E),
        };
        // Was 308 separate lv_obj cells, each with its own invalidate/redraw
        // pipeline -- see buildLocalNowPlaying()'s comment for why this is
        // now one lv_canvas instead. Still cache each column's last
        // lit-count and skip columns that didn't change; the actual canvas
        // pixel writes for a changed column are cheap (a handful of
        // lv_canvas_draw_rect calls into a buffer already in memory), and
        // the one lv_obj_invalidate() at the end covers however many
        // columns changed this tick, instead of one invalidate per column.
        bool anyColumnChanged = false;
        for (uint8_t c = 0; c < m_specCols; ++c) {
            uint8_t lit = 0;
            if (playing) {
                // Linear-interpolate between the 6 real FFT bands across
                // all 28 columns instead of a hard nearest-band assignment.
                // With only 6 bands spread over 28 columns, ~4-5 adjacent
                // columns used to land on the exact same band and visibly
                // move in lockstep ("跳动是四到五组为单位") -- interpolating
                // gives every column its own blended value, closer to a
                // real per-column response and a smoother-looking ripple.
                const float bandPos = static_cast<float>(c) * 5.0f / (m_specCols - 1); // 0..5 across 6 bands
                const uint8_t bandLo = static_cast<uint8_t>(bandPos);
                const uint8_t bandHi = std::min<uint8_t>(bandLo + 1, 5);
                const float frac = bandPos - bandLo;
                const float level = state.spectrumBands[bandLo] * (1.0f - frac) + state.spectrumBands[bandHi] * frac;
                // Real audio rarely drives a band anywhere near the raw
                // 0-255 ceiling, so the top rows almost never lit up.
                // Boost before scaling to the row count instead of after
                // (scaling after would just make more LOW cells cross the
                // 1-row threshold, not make tall peaks taller).
                uint16_t boosted = static_cast<uint16_t>(level * 1.5f); // 1.5x -- 2x was too much
                if (boosted > 255) boosted = 255;
                lit = static_cast<uint8_t>((boosted * m_specRows) / 255);
                if (level > 10 && lit == 0) lit = 1; // any real signal shows at least one lit cell
            }
            if (c < 32 && m_specLastLit[c] == lit) continue; // unchanged column, skip entirely
            if (c < 32) m_specLastLit[c] = lit;
            anyColumnChanged = true;
            for (uint8_t r = 0; r < m_specRows; ++r) {
                drawSpecDot(m_specCanvas, c, r, kSpecCanvasH, r < lit ? kRowColors[r < 11 ? r : 10] : kInkFaint);
            }
        }
        if (anyColumnChanged) lv_obj_invalidate(m_specCanvas);
    }

    if (m_playIcon) lv_label_set_text(m_playIcon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    // Tape-position needle: real 6-band average level deflects it right
    // from a left rest position, like a classic VU meter -- same data
    // source as the flat card's spectrum, just condensed to one number.
    if (m_cassetteNeedle) {
        uint16_t x = 83;
        if (playing) {
            uint16_t sum = 0;
            for (uint8_t i = 0; i < 6; ++i) sum += state.spectrumBands[i];
            const uint8_t avg = static_cast<uint8_t>(sum / 6);
            x = 83 + (static_cast<uint16_t>(avg) * (228 - 83)) / 255;
        }
        lv_obj_set_x(m_cassetteNeedle, x);
    }

    // Cassette reels spin only while actually playing, same animation
    // budget rule as the vinyl disc (refreshCoverSpin) -- but slower, a
    // real cassette reel turns much lazier than a spinning vinyl disc
    // ("缓缓转动"). Uses lv_img_set_angle() (rotates the bitmap itself,
    // clipped circular by its wrapper obj) instead of the old composite-
    // object rotation, which didn't reliably invalidate its children and
    // made the whole cassette view flash then go blank.
    if (playing && m_cassetteReelL && !m_cassetteSpinning) {
        m_cassetteSpinning = true;
        for (lv_obj_t* reel : {m_cassetteReelL, m_cassetteReelR}) {
            if (!reel) continue;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, reel);
            lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) { lv_img_set_angle(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(v)); });
            lv_anim_set_values(&a, 0, 3600);
            lv_anim_set_time(&a, 14000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
        }
    } else if (!playing && m_cassetteReelL && m_cassetteSpinning) {
        m_cassetteSpinning = false;
        lv_anim_del(m_cassetteReelL, nullptr);
        if (m_cassetteReelR) lv_anim_del(m_cassetteReelR, nullptr);
    }

}

void HifiUi::playLocalTrackByIndex(uint16_t index) {
    if (!playerService.playLocalTrack(index)) return;
    m_currentLocalTrackIndex = index;
    loadCoverArt(index);
    m_hasLyrics = playerService.loadLyrics(index);
}

// Walks s_localTracks (via playerService) from `from`, respecting the
// current artist/album filter (same rule as the Songs tab), and returns the
// next/previous matching index. Returns -1 if the filtered set has 0 or 1
// tracks, or (wrap=false, forward search only) once it's walked off the end
// of the list without wrapping back to the start -- used by refresh()'s
// Sequential-mode auto-advance, which should stop rather than loop forever.
// Manual prev/next taps always pass wrap=true (the default) regardless of
// play mode, matching how every other player's transport buttons behave.
int32_t HifiUi::findLocalTrack(uint16_t from, bool forward, bool wrap) const {
    const uint16_t count = playerService.localLibraryCount();
    if (count < 2) return -1;
    if (m_localPlayMode == LocalPlayMode::Shuffle) {
        // Genuine shuffle: pick a uniformly random different track matching
        // the active filter, bounded retries instead of a play-history
        // stack -- both prev/next land on a fresh random pick.
        for (uint8_t attempt = 0; attempt < 20; ++attempt) {
            const uint16_t candidate = static_cast<uint16_t>(random(count));
            if (candidate == from) continue;
            LocalTrackItem item{};
            if (!playerService.localTrack(candidate, &item)) continue;
            if (m_musicFilterArtist[0] && strcmp(item.artist, m_musicFilterArtist) != 0) continue;
            if (m_musicFilterAlbum[0] && strcmp(item.album, m_musicFilterAlbum) != 0) continue;
            return candidate;
        }
        return -1;
    }
    const uint16_t maxStep = (!wrap && forward) ? (count - 1 - from) : count;
    for (uint16_t step = 1; step <= maxStep; ++step) {
        const int32_t candidate = forward ? (static_cast<int32_t>(from) + step) % count : (static_cast<int32_t>(from) - step + count * 2) % count;
        LocalTrackItem item{};
        if (!playerService.localTrack(static_cast<uint16_t>(candidate), &item)) continue;
        if (m_musicFilterArtist[0] && strcmp(item.artist, m_musicFilterArtist) != 0) continue;
        if (m_musicFilterAlbum[0] && strcmp(item.album, m_musicFilterAlbum) != 0) continue;
        if (candidate == from) break; // looped all the way around without finding another match
        return candidate;
    }
    return -1;
}

const char* HifiUi::localPlayModeSymbol(LocalPlayMode mode) {
    switch (mode) {
        case LocalPlayMode::Sequential: return LV_SYMBOL_RIGHT;
        case LocalPlayMode::RepeatAll: return LV_SYMBOL_LOOP;
        case LocalPlayMode::RepeatOne: return LV_SYMBOL_LOOP "1";
        case LocalPlayMode::Shuffle: return LV_SYMBOL_SHUFFLE;
    }
    return LV_SYMBOL_RIGHT;
}

void HifiUi::clearCoverArt() {
    if (m_coverArtPixels) {
        free(m_coverArtPixels);
        m_coverArtPixels = nullptr;
    }
    m_coverArtDsc = lv_img_dsc_t{};
}

void HifiUi::loadCoverArt(uint16_t trackIndex) {
    clearCoverArt();
    // Scale 8 (tjpgd's max downscale, 1/8) keeps a typical 300-500px cover
    // close to the ~64px disc size without needing lv_img zoom -- cheap to
    // decode and tiny in PSRAM (a few KB), at the cost of not being pixel-
    // perfect-cropped to the circle.
    uint16_t* pixels = nullptr;
    uint16_t w = 0, h = 0;
    const bool ok = playerService.decodeLocalTrackCover(trackIndex, 8, &pixels, &w, &h);
    printf("[COVER] loadCoverArt idx=%u ok=%d w=%u h=%u\n", trackIndex, ok, w, h);
    if (!ok || !pixels || !w || !h) return;
    m_coverArtPixels = pixels;
    m_coverArtDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    m_coverArtDsc.header.always_zero = 0;
    m_coverArtDsc.header.w = w;
    m_coverArtDsc.header.h = h;
    m_coverArtDsc.data_size = static_cast<uint32_t>(w) * h * 2;
    m_coverArtDsc.data = reinterpret_cast<const uint8_t*>(pixels);
}

void HifiUi::buildPlaceholder(const char* title, const char* detail) {
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen); // back chevron lives in the status bar now
    makeText(screen, title, &lv_font_montserrat_16, kInk, LV_ALIGN_CENTER, 0, -14);
    makeText(screen, detail, &lv_font_montserrat_12, kInkDim, LV_ALIGN_CENTER, 0, 8);
}

void HifiUi::buildSettings() {
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen);
    makeText(screen, "设置", &lv_font_cjk_16, kInk, LV_ALIGN_TOP_LEFT, 12, 28);

    lv_obj_t* wifiCard = makeCard(screen, LV_SYMBOL_WIFI, "WiFi", Page::SettingsWifi, 16, 56, 90, 76);
    (void)wifiCard;
    lv_obj_t* fontCard = makeCard(screen, LV_SYMBOL_EYE_OPEN, "字体", Page::FontPreview, 116, 56, 90, 76);
    (void)fontCard;
    // EQ/audio/sleep settings aren't built yet -- see Page::Settings's
    // buildPlaceholder fallback history; this hub only has WiFi so far,
    // more cards land here as those features are implemented.
}

void HifiUi::buildFontPreview() {
    lv_obj_t* screen = lv_scr_act();
    buildStatusBar(screen);

    addFontPreviewLabel(screen, "字体清晰度预览", &lv_font_cjk_13, kInk, 10, 25, 180);
    addFontPreviewLabel(screen, "看边缘锐度/灰雾/滚动残影", &lv_font_cjk_13, kInkDim, 168, 25, 144);

    // 2026-07-29: root-caused the systemic blur here -- NotoSansSC-wght.ttf's
    // unpinned/default instance turned out to be its named "Thin" (wght=100)
    // style, and every font baked from it all session (cjk_13/16 included)
    // had silently been Thin. lv_font_cjk_13/_16 are now baked from a real
    // Medium (wght=500) static instance instead, confirmed clearly sharper
    // than both plain Thin and the faux-bold (double-drawn) hack on the real
    // panel -- see docs/DEV_LOG_2026-07-29_font_preview.md. Kept the
    // remaining rows (contrast/color/scroll variants) since those are still
    // open questions; dropped the redundant "CJK13+B"/"MEDIUM" comparison
    // rows since the base font itself is Medium now.
    addFontPreviewRow(screen, 48, "CJK13", &lv_font_cjk_13, kInk, "中文歌词 ABC 123");
    addFontPreviewRow(screen, 68, "DIM", &lv_font_cjk_13, kInkDim, "歌曲标题 / 歌手 / 专辑");
    addFontPreviewRow(screen, 88, "PURPLE", &lv_font_cjk_13, kAccentBright, "同步歌词正在显示");
    addFontPreviewRow(screen, 108, "SCROLL", &lv_font_cjk_13, kInk,
                      "很长的本地音乐标题 - Artist / Album / Lyrics 123",
                      false, LV_LABEL_LONG_SCROLL_CIRCULAR);
    addFontPreviewRow(screen, 128, "CJK16", &lv_font_cjk_16, kInk, "设置 选择网络 ABC 123");
}

void HifiUi::buildSettingsWifi() {
    lv_obj_t* screen = lv_scr_act();
    const PlayerSnapshot state = playerService.snapshot();

    // PasswordEntry goes full-screen (no status bar) to give the keyboard
    // as much vertical room as this 170px-tall panel can spare; every other
    // stage keeps the normal status bar.
    const bool fullScreenKeyboard = m_wifiShowAddNetwork && m_wifiAddStage == WifiAddStage::PasswordEntry;
    if (!fullScreenKeyboard) buildStatusBar(screen);

    if (m_wifiShowAddNetwork && m_wifiAddStage == WifiAddStage::ScanList) {
        // Sub-view 1/2: on-demand scan results (see onWifiAddOpenAction --
        // the scan itself already ran by the time we get here). Tapping a
        // row moves to PasswordEntry; no scanning happens again until the
        // user leaves and reopens this screen.
        lv_obj_t* backBtn = lv_btn_create(screen);
        lv_obj_set_pos(backBtn, 4, 24);
        lv_obj_set_size(backBtn, 28, 24);
        lv_obj_set_style_bg_opa(backBtn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(backBtn, 0, 0);
        lv_obj_set_style_shadow_width(backBtn, 0, 0);
        lv_obj_set_style_pad_all(backBtn, 0, 0);
        lv_obj_clear_flag(backBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(backBtn);
        lv_obj_add_event_cb(backBtn, onWifiAddBackAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* backLabel = makeText(backBtn, LV_SYMBOL_LEFT, &lv_font_montserrat_14, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);

        makeText(screen, "选择网络", &lv_font_cjk_16, kInk, LV_ALIGN_TOP_LEFT, 34, 26);

        lv_obj_t* list = lv_obj_create(screen);
        lv_obj_set_pos(list, 8, 50);
        lv_obj_set_size(list, 304, 114);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_style_radius(list, 0, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

        if (m_wifiScanPending) {
            makeText(list, "正在扫描附近WiFi...", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_MID, 0, 8);
        } else if (m_wifiScanCount == 0) {
            makeText(list, "附近没有扫描到WiFi网络", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_MID, 0, 8);
        }
        for (uint8_t i = 0; i < m_wifiScanCount; ++i) {
            lv_obj_t* row = lv_btn_create(list);
            lv_obj_set_pos(row, 0, i * 34);
            lv_obj_set_size(row, 296, 30);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, kPanel, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            addPressFx(row);
            lv_obj_add_event_cb(row, onWifiScanRowAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

            lv_obj_t* nameLabel = makeText(row, m_wifiScanResults[i].ssid, &lv_font_cjk_13, kInk, LV_ALIGN_LEFT_MID, 10, 0);
            lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nameLabel, 220);
            lv_obj_clear_flag(nameLabel, LV_OBJ_FLAG_CLICKABLE);
            char rssiText[8];
            snprintf(rssiText, sizeof(rssiText), "%d", m_wifiScanResults[i].rssi);
            lv_obj_t* rssiLabel = makeText(row, rssiText, &lv_font_cjk_13, kInkFaint, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_clear_flag(rssiLabel, LV_OBJ_FLAG_CLICKABLE);
        }
        return; // no refreshSettingsWifi() call needed -- this sub-view is static
    }

    if (m_wifiShowAddNetwork && m_wifiAddStage == WifiAddStage::PasswordEntry) {
        // Sub-view 2/2: password entry for m_wifiAddSelectedSsid, full-screen
        // (see fullScreenKeyboard above) so the keyboard has real room.
        // Everything above the keyboard packed into one 20px row -- this
        // panel is only 170px tall total, and the keyboard needs every
        // spare pixel it can get (see its own comment below).
        lv_obj_t* backBtn = lv_btn_create(screen);
        lv_obj_set_pos(backBtn, 2, 0);
        lv_obj_set_size(backBtn, 18, 20);
        lv_obj_set_style_bg_opa(backBtn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(backBtn, 0, 0);
        lv_obj_set_style_shadow_width(backBtn, 0, 0);
        lv_obj_set_style_pad_all(backBtn, 0, 0);
        lv_obj_clear_flag(backBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(backBtn);
        lv_obj_add_event_cb(backBtn, onWifiAddBackAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* backLabel = makeText(backBtn, LV_SYMBOL_LEFT, &lv_font_montserrat_14, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* ssidLabel = makeText(screen, m_wifiAddSelectedSsid, &lv_font_cjk_13, kInk, LV_ALIGN_TOP_LEFT, 24, 2);
        lv_label_set_long_mode(ssidLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssidLabel, 82);

        m_wifiAddPwField = lv_textarea_create(screen);
        lv_obj_set_pos(m_wifiAddPwField, 110, 0);
        lv_obj_set_size(m_wifiAddPwField, 140, 20);
        lv_textarea_set_one_line(m_wifiAddPwField, true);
        lv_textarea_set_password_mode(m_wifiAddPwField, true);
        lv_textarea_set_placeholder_text(m_wifiAddPwField, "密码");
        lv_textarea_set_max_length(m_wifiAddPwField, 63);
        lv_obj_set_style_text_font(m_wifiAddPwField, &lv_font_cjk_13, 0);
        lv_obj_set_style_pad_ver(m_wifiAddPwField, 2, 0);

        lv_obj_t* saveBtn = lv_btn_create(screen);
        lv_obj_set_pos(saveBtn, 254, 0);
        lv_obj_set_size(saveBtn, 62, 20);
        lv_obj_set_style_radius(saveBtn, 8, 0);
        lv_obj_set_style_bg_color(saveBtn, kAccentBright, 0);
        lv_obj_set_style_bg_opa(saveBtn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(saveBtn, 0, 0);
        lv_obj_set_style_pad_all(saveBtn, 0, 0);
        lv_obj_clear_flag(saveBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(saveBtn);
        lv_obj_add_event_cb(saveBtn, onWifiAddSaveAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* saveLabel = makeText(saveBtn, "连接", &lv_font_cjk_13, lv_color_black(), LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(saveLabel, LV_OBJ_FLAG_CLICKABLE);

        // LVGL's btnmatrix always divides the widget's height evenly across
        // however many rows the map has (lv_btnmatrix.c: max_h/row_cnt) --
        // it does NOT shrink to fit if a row's natural content is taller,
        // it just draws every row at that same even share. The stock
        // keyboard map is 4 rows (qwerty, home row, zxcvbnm, a dedicated
        // space/arrow/mode row); on this 150px-tall budget that's ~37px a
        // row, and the last row's mostly-icon buttons came out too cramped
        // to read. Dropping to a custom 3-row map (no dedicated arrow row,
        // "123" folded into row 3 as the mode toggle, one wide space key)
        // gives each row ~50px instead -- comfortably bigger, "squarer"
        // keys with legible text, at the cost of no left/right cursor keys
        // (rarely needed typing a password once) and losing the def
        // Enter/OK) is right in the map instead of a separate control row.
        static const char* const kPasswordKbMap[] = {
            "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
            "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
            "1#", "z", "x", "c", "v", "b", "n", "m", " ", ""
        };
        // lv_keyboard internally does an unconditional memcpy from whatever
        // ctrl_map it's given (lv_keyboard.c's lv_keyboard_update_ctrl_map,
        // when popovers are off) -- passing nullptr here isn't "use the
        // default", it's a straight null-pointer read, and the exact crash
        // hit tapping a scan result (which is what first exercises this
        // custom map). Must supply a real array sized to match the map's
        // 30 buttons (11 + 10 + 9), even though none of them need anything
        // but the default width.
        static const lv_btnmatrix_ctrl_t kPasswordKbCtrl[30] = {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,       // row 1: qwertyuiop + wider backspace
            1, 1, 1, 1, 1, 1, 1, 1, 1, 2,          // row 2: asdfghjkl + wider enter
            1, 1, 1, 1, 1, 1, 1, 1, 4              // row 3: 1# zxcvbnm + wide space
        };
        m_wifiAddKeyboard = lv_keyboard_create(screen);
        lv_obj_set_pos(m_wifiAddKeyboard, 0, 20);
        lv_obj_set_size(m_wifiAddKeyboard, 320, 150);
        lv_obj_set_style_pad_all(m_wifiAddKeyboard, 0, 0);
        lv_obj_set_style_pad_row(m_wifiAddKeyboard, 2, 0);
        lv_obj_set_style_pad_column(m_wifiAddKeyboard, 2, 0);
        lv_keyboard_set_map(m_wifiAddKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kPasswordKbMap, kPasswordKbCtrl);
        lv_keyboard_set_textarea(m_wifiAddKeyboard, m_wifiAddPwField);
        lv_obj_add_event_cb(m_wifiAddKeyboard, onWifiAddSaveAction, LV_EVENT_READY, nullptr);

        return; // no refreshSettingsWifi() call needed -- this sub-view is static
    }

    if (m_wifiShowManageQr) {
        // Sub-view: QR code linking to the phone-friendly /wifi_manage admin
        // page (scan nearby networks, edit/delete saved ones) -- only
        // reachable once connected, since the QR encodes the device's own
        // LAN IP.
        lv_obj_t* backBtn = lv_btn_create(screen);
        lv_obj_set_pos(backBtn, 4, 24);
        lv_obj_set_size(backBtn, 28, 24);
        lv_obj_set_style_bg_opa(backBtn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(backBtn, 0, 0);
        lv_obj_set_style_shadow_width(backBtn, 0, 0);
        lv_obj_set_style_pad_all(backBtn, 0, 0);
        lv_obj_clear_flag(backBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(backBtn);
        lv_obj_add_event_cb(backBtn, onWifiManageBackAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* backLabel = makeText(backBtn, LV_SYMBOL_LEFT, &lv_font_montserrat_14, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);

        makeText(screen, "WiFi 后台管理", &lv_font_cjk_16, kInk, LV_ALIGN_TOP_LEFT, 34, 26);

        // Always create the widget regardless of state.wifiConnected here --
        // that's a cached snapshot that can be one tick stale right after
        // connecting, and gating creation on it risked a real race: build
        // sees not-yet-connected (skips creating m_wifiQr entirely), then
        // refresh sees connected a tick later but has no widget left to
        // show, permanently blank until the next full rebuild. Visibility
        // itself is still refresh's job (see qrContent[0] check below).
        // 80px QR anchored just under the title, hint text fixed below it
        // with its own reserved band -- rather than pinning the hint to the
        // screen bottom, which overlapped the QR whenever the hint text
        // wrapped to two lines on this 170px-tall panel.
#if LV_USE_QRCODE
        m_wifiQr = lv_qrcode_create(screen, 80, lv_color_black(), lv_color_white());
        lv_obj_align(m_wifiQr, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_set_style_border_color(m_wifiQr, kInkFaint, 0);
        lv_obj_set_style_border_width(m_wifiQr, 4, 0);
        lv_obj_clear_flag(m_wifiQr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(m_wifiQr, LV_OBJ_FLAG_HIDDEN); // see build comment above, filled in by refresh
#endif
        m_wifiHintText = makeText(screen, "", &lv_font_cjk_13, kInkFaint, LV_ALIGN_TOP_MID, 0, 132);
        lv_obj_set_width(m_wifiHintText, 300);
        lv_label_set_long_mode(m_wifiHintText, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(m_wifiHintText, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        // List mode -- saved networks only (from NVS), no on-device
        // scanning: discovering/adding a brand-new network is now a
        // phone-only job via the "后台管理" QR (see onWifiManageOpenAction),
        // which does its own on-demand scan when the page loads. Tapping a
        // saved row here just nudges wifiMulti to retry it now (see
        // onWifiSavedRowAction) -- no password re-entry needed, since the
        // credential is already stored.
        makeText(screen, "WiFi 设置", &lv_font_cjk_16, kInk, LV_ALIGN_TOP_LEFT, 12, 26);

        lv_obj_t* manageBtn = lv_btn_create(screen);
        lv_obj_align(manageBtn, LV_ALIGN_TOP_RIGHT, -8, 24);
        lv_obj_set_size(manageBtn, 70, 22);
        lv_obj_set_style_radius(manageBtn, 8, 0);
        lv_obj_set_style_bg_color(manageBtn, kPanel, 0);
        lv_obj_set_style_bg_opa(manageBtn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(manageBtn, kInkFaint, 0);
        lv_obj_set_style_border_width(manageBtn, 1, 0);
        lv_obj_set_style_shadow_width(manageBtn, 0, 0);
        lv_obj_set_style_pad_all(manageBtn, 0, 0);
        lv_obj_clear_flag(manageBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(manageBtn);
        lv_obj_add_event_cb(manageBtn, onWifiManageOpenAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* manageLabel = makeText(manageBtn, "后台管理", &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(manageLabel, LV_OBJ_FLAG_CLICKABLE);

        // Manual add-network entry -- see m_wifiShowAddNetwork's comment for
        // why this exists alongside the phone-only QR page.
        lv_obj_t* addBtn = lv_btn_create(screen);
        lv_obj_align(addBtn, LV_ALIGN_TOP_RIGHT, -86, 24);
        lv_obj_set_size(addBtn, 70, 22);
        lv_obj_set_style_radius(addBtn, 8, 0);
        lv_obj_set_style_bg_color(addBtn, kPanel, 0);
        lv_obj_set_style_bg_opa(addBtn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(addBtn, kInkFaint, 0);
        lv_obj_set_style_border_width(addBtn, 1, 0);
        lv_obj_set_style_shadow_width(addBtn, 0, 0);
        lv_obj_set_style_pad_all(addBtn, 0, 0);
        lv_obj_clear_flag(addBtn, LV_OBJ_FLAG_SCROLLABLE);
        addPressFx(addBtn);
        lv_obj_add_event_cb(addBtn, onWifiAddOpenAction, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* addLabel = makeText(addBtn, "手动添加", &lv_font_cjk_13, kInkDim, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(addLabel, LV_OBJ_FLAG_CLICKABLE);

        m_wifiStatusText = makeText(screen, "", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_LEFT, 12, 50);
        lv_obj_set_width(m_wifiStatusText, 296);
        lv_label_set_long_mode(m_wifiStatusText, LV_LABEL_LONG_DOT);

        m_wifiNetworkList = lv_obj_create(screen);
        lv_obj_set_pos(m_wifiNetworkList, 8, 68);
        lv_obj_set_size(m_wifiNetworkList, 304, 96);
        lv_obj_set_style_bg_opa(m_wifiNetworkList, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(m_wifiNetworkList, 0, 0);
        lv_obj_set_style_pad_all(m_wifiNetworkList, 0, 0);
        lv_obj_set_style_radius(m_wifiNetworkList, 0, 0);
        lv_obj_add_flag(m_wifiNetworkList, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(m_wifiNetworkList, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(m_wifiNetworkList, LV_SCROLLBAR_MODE_AUTO);

        const uint8_t count = playerService.wifiSavedCount();
        m_wifiLastSavedCount = count;
        if (count == 0) {
            makeText(m_wifiNetworkList, "还没有保存的网络，用手机扫码后台管理添加", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_MID, 0, 8);
        }
        for (uint8_t i = 0; i < count; ++i) {
            char ssid[33];
            bool isDefault = false;
            if (!playerService.wifiSavedInfo(i, ssid, sizeof(ssid), &isDefault)) continue;
            const bool isCurrent = state.wifiConnected && strcmp(ssid, state.wifiSsid) == 0;

            lv_obj_t* row = lv_btn_create(m_wifiNetworkList);
            lv_obj_set_pos(row, 0, i * 34);
            lv_obj_set_size(row, 296, 30);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, kPanel, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, isCurrent ? 1 : 0, 0);
            lv_obj_set_style_border_color(row, kAccentBright, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            addPressFx(row);
            lv_obj_add_event_cb(row, onWifiSavedRowAction, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

            lv_obj_t* nameLabel = makeText(row, ssid, &lv_font_cjk_13, isCurrent ? kAccentBright : kInk, LV_ALIGN_LEFT_MID, 10, 0);
            lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nameLabel, 190);
            lv_obj_clear_flag(nameLabel, LV_OBJ_FLAG_CLICKABLE);
            const char* tagText = isCurrent ? "已连接" : (isDefault ? "出厂默认" : "点击连接");
            lv_obj_t* tag = makeText(row, tagText, &lv_font_cjk_13, isCurrent ? kAccentBright : kInkFaint, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    refreshSettingsWifi(state);
}

void HifiUi::refreshSettingsWifi(const PlayerSnapshot& state) {
    if (m_wifiShowAddNetwork) {
        // Neither add-network sub-view rebuilds live -- but ScanList needs
        // to notice when the background scan (started by
        // onWifiAddOpenAction) finishes, since that scan is asynchronous
        // now (a blocking WiFi.scanNetworks() call right in the button's
        // click handler used to starve the audio/UI task long enough to
        // reboot the device -- see playerCoreWifiScanStart()'s comment).
        if (m_wifiAddStage == WifiAddStage::ScanList && m_wifiScanPending && !playerService.wifiScanInProgress()) {
            m_wifiScanPending = false;
            m_wifiScanCount = playerService.wifiScanResults(m_wifiScanResults, kWifiScanMaxItems);
            show(Page::SettingsWifi);
        }
        return;
    }
    if (!m_wifiShowManageQr) {
        // List mode -- rebuild once when the saved-slot count actually
        // changes (e.g. a network was added/removed from the phone admin
        // page while this screen happened to be open); the list itself
        // isn't live-patched, same "rebuild via show()" pattern as the
        // local-music list's filter changes.
        const uint8_t savedCount = playerService.wifiSavedCount();
        if (savedCount != m_wifiLastSavedCount) {
            show(Page::SettingsWifi);
            return;
        }
        char status[96];
        if (state.wifiApFallbackActive) snprintf(status, sizeof(status), "未连接 -- 已开启配网热点 %s", state.wifiApSsid);
        else if (state.wifiConnected) snprintf(status, sizeof(status), "已连接: %s (%d dBm)", state.wifiSsid, state.wifiRssi);
        else snprintf(status, sizeof(status), "未连接WiFi");
        if (m_wifiStatusText) lv_label_set_text(m_wifiStatusText, status);
        return;
    }

    // Management QR sub-view -- encodes the device's own LAN IP, so it only
    // makes sense once connected.
    char qrContent[128];
    char hint[160];
    if (state.wifiConnected) {
        snprintf(qrContent, sizeof(qrContent), "http://%s/wifi_manage", state.wifiIp);
        snprintf(hint, sizeof(hint), "同一WiFi下用手机扫码，查看附近网络、连接或删除已保存的网络");
    } else {
        qrContent[0] = '\0';
        snprintf(hint, sizeof(hint), "请先连接WiFi后再使用后台管理");
    }
    if (m_wifiHintText) lv_label_set_text(m_wifiHintText, hint);
#if LV_USE_QRCODE
    if (m_wifiQr) {
        if (qrContent[0]) {
            lv_obj_clear_flag(m_wifiQr, LV_OBJ_FLAG_HIDDEN);
            if (strcmp(qrContent, m_wifiQrLastContent) != 0) {
                lv_qrcode_update(m_wifiQr, qrContent, strlen(qrContent));
                strlcpy(m_wifiQrLastContent, qrContent, sizeof(m_wifiQrLastContent));
            }
        } else {
            lv_obj_add_flag(m_wifiQr, LV_OBJ_FLAG_HIDDEN);
            m_wifiQrLastContent[0] = '\0';
        }
    }
#endif
}

// Builds the quick-settings drawer once, on lv_layer_top() (LVGL's always-
// on-top layer, unaffected by show()'s screen swaps) so it can be pulled
// down over any page rather than being its own Page. Starts hidden.
void HifiUi::buildQuickPanel() {
    m_quickPanel = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(m_quickPanel, 0, 0);
    lv_obj_set_size(m_quickPanel, 320, 170);
    lv_obj_set_style_bg_color(m_quickPanel, kBg, 0);
    lv_obj_set_style_bg_opa(m_quickPanel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(m_quickPanel, 0, 0);
    lv_obj_set_style_radius(m_quickPanel, 0, 0);
    lv_obj_set_style_pad_all(m_quickPanel, 0, 0);
    lv_obj_clear_flag(m_quickPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m_quickPanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* grabber = lv_obj_create(m_quickPanel);
    lv_obj_set_size(grabber, 40, 4);
    lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(grabber, 2, 0);
    lv_obj_set_style_bg_color(grabber, kInkFaint, 0);
    lv_obj_set_style_border_width(grabber, 0, 0);
    lv_obj_clear_flag(grabber, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(grabber, LV_OBJ_FLAG_CLICKABLE);

    makeText(m_quickPanel, "快捷设置", &lv_font_cjk_13, kInk, LV_ALIGN_TOP_MID, 0, 10);

    // One consistent row shape reused for volume/brightness/EQ: a short
    // label on the left, a slider in the middle, a live value on the right.
    auto makeRow = [this](const char* label, int16_t y, lv_color_t indicatorColor, lv_obj_t** outSlider, lv_obj_t** outValueLabel) {
        makeText(m_quickPanel, label, &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_LEFT, 6, y + 3);
        lv_obj_t* slider = lv_slider_create(m_quickPanel);
        lv_obj_set_pos(slider, 54, y);
        lv_obj_set_size(slider, 190, 10);
        lv_obj_set_style_radius(slider, 5, LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, kInkFaint, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, indicatorColor, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, kInk, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
        if (outSlider) *outSlider = slider;
        if (outValueLabel) *outValueLabel = makeText(m_quickPanel, "", &lv_font_montserrat_10, kInkDim, LV_ALIGN_TOP_LEFT, 250, y + 1);
    };

    makeRow("音量", 30, kMagenta, &m_quickVolumeSlider, &m_quickVolumeLabel);
    lv_slider_set_range(m_quickVolumeSlider, 0, 100);
    lv_obj_add_event_cb(m_quickVolumeSlider, onQuickVolumeAction, LV_EVENT_VALUE_CHANGED, nullptr);

    makeRow("亮度", 56, kAccent, &m_quickBrightnessSlider, &m_quickBrightnessLabel);
    lv_slider_set_range(m_quickBrightnessSlider, 5, 100); // don't allow dragging the screen unreadably dark
    lv_obj_add_event_cb(m_quickBrightnessSlider, onQuickBrightnessAction, LV_EVENT_VALUE_CHANGED, nullptr);

    makeText(m_quickPanel, "均衡器", &lv_font_cjk_13, kInkDim, LV_ALIGN_TOP_LEFT, 6, 80);
    static const char* kEqLabels[3] = {"低音", "中音", "高音"};
    for (uint8_t i = 0; i < 3; ++i) {
        makeRow(kEqLabels[i], 96 + i * 22, kAccentBright, &m_quickEqSliders[i], &m_quickEqLabels[i]);
        lv_slider_set_range(m_quickEqSliders[i], -12, 12);
        lv_obj_add_event_cb(m_quickEqSliders[i], onQuickEqAction, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }

    makeText(m_quickPanel, "从底部边缘上滑关闭", &lv_font_cjk_13, kInkFaint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void HifiUi::setQuickPanelOpen(bool open) {
    if (!m_quickPanel || open == m_quickPanelOpen) return;
    m_quickPanelOpen = open;
    if (open) {
        lv_obj_clear_flag(m_quickPanel, LV_OBJ_FLAG_HIDDEN);
        refreshQuickPanel();
    } else {
        lv_obj_add_flag(m_quickPanel, LV_OBJ_FLAG_HIDDEN);
    }
}

void HifiUi::refreshQuickPanel() {
    const PlayerSnapshot state = playerService.snapshot();
    if (m_quickVolumeSlider) {
        const uint8_t pct = state.volumeSteps ? static_cast<uint8_t>(state.volume * 100 / state.volumeSteps) : 0;
        lv_slider_set_value(m_quickVolumeSlider, pct, LV_ANIM_OFF);
        if (m_quickVolumeLabel) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u%%", pct);
            lv_label_set_text(m_quickVolumeLabel, buf);
        }
    }
    if (m_quickBrightnessSlider) {
        const uint8_t pct = m_port.backlightPercent();
        lv_slider_set_value(m_quickBrightnessSlider, pct, LV_ANIM_OFF);
        if (m_quickBrightnessLabel) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u%%", pct);
            lv_label_set_text(m_quickBrightnessLabel, buf);
        }
    }
    const int8_t eq[3] = {m_eqLow, m_eqMid, m_eqHigh};
    for (uint8_t i = 0; i < 3; ++i) {
        if (!m_quickEqSliders[i]) continue;
        lv_slider_set_value(m_quickEqSliders[i], eq[i], LV_ANIM_OFF);
        if (m_quickEqLabels[i]) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%+ddB", eq[i]);
            lv_label_set_text(m_quickEqLabels[i], buf);
        }
    }
}

void HifiUi::onQuickVolumeAction(lv_event_t* event) {
    if (!s_instance || !s_instance->m_quickVolumeSlider) return;
    const int32_t pct = lv_slider_get_value(s_instance->m_quickVolumeSlider);
    const PlayerSnapshot state = playerService.snapshot();
    const uint8_t steps = state.volumeSteps ? state.volumeSteps : 21;
    playerService.setVolume(static_cast<uint8_t>(pct * steps / 100));
}

void HifiUi::onQuickBrightnessAction(lv_event_t* event) {
    if (!s_instance || !s_instance->m_quickBrightnessSlider) return;
    const int32_t pct = lv_slider_get_value(s_instance->m_quickBrightnessSlider);
    s_instance->m_port.setBacklightPercent(static_cast<uint8_t>(pct));
}

void HifiUi::onQuickEqAction(lv_event_t* event) {
    if (!s_instance) return;
    const uintptr_t band = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (band > 2 || !s_instance->m_quickEqSliders[band]) return;
    const int32_t db = lv_slider_get_value(s_instance->m_quickEqSliders[band]);
    if (band == 0) s_instance->m_eqLow = static_cast<int8_t>(db);
    else if (band == 1) s_instance->m_eqMid = static_cast<int8_t>(db);
    else s_instance->m_eqHigh = static_cast<int8_t>(db);
    playerService.setTone(s_instance->m_eqLow, s_instance->m_eqMid, s_instance->m_eqHigh);
    if (s_instance->m_quickEqLabels[band]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%+ddB", static_cast<int>(db));
        lv_label_set_text(s_instance->m_quickEqLabels[band], buf);
    }
}

void HifiUi::refresh() {
    const PlayerSnapshot state = playerService.snapshot();

    // Local-track auto-advance: Audio::evt_eof (radio disconnect OR a local
    // file reaching its end) doesn't say which happened, and by the time
    // this snapshot is read state.source has already flipped away from Sd
    // either way -- m_lastSourceSeen holds what it was on the PREVIOUS
    // tick, before this same eof event took effect, which is what actually
    // distinguishes the two cases.
    if (state.eofCount != m_lastEofCount) {
        m_lastEofCount = state.eofCount;
        if (m_lastSourceSeen == PlayerSource::Sd) {
            if (m_localPlayMode == LocalPlayMode::RepeatOne) {
                playLocalTrackByIndex(m_currentLocalTrackIndex);
            } else {
                const bool wrap = m_localPlayMode != LocalPlayMode::Sequential;
                const int32_t next = findLocalTrack(m_currentLocalTrackIndex, true, wrap);
                if (next >= 0) playLocalTrackByIndex(static_cast<uint16_t>(next));
                // else: Sequential mode reached the end of the list -- stop,
                // same as a normal player with repeat off.
            }
        }
    }
    m_lastSourceSeen = state.source;

    if (m_statusTime) lv_label_set_text(m_statusTime, state.timeHM[0] ? state.timeHM : "--:--");
    // The WiFi icon's own color now carries signal strength (no separate
    // bar graph, per feedback that it looked like a second volume meter):
    // grey disconnected, orange weak, green strong.
    if (m_statusWifiIcon) {
        lv_color_t wifiColor = kInkFaint;
        if (state.wifiConnected) wifiColor = state.wifiRssi >= -67 ? kLive : kMute;
        lv_obj_set_style_text_color(m_statusWifiIcon, wifiColor, 0);
    }
    if (m_statusTag) {
        if (state.muted) {
            lv_label_set_text(m_statusTag, "MUTE");
            lv_obj_set_style_text_color(m_statusTag, kMute, 0);
        } else if (state.transport == PlayerTransport::Buffering) {
            lv_label_set_text(m_statusTag, "BUFF");
            lv_obj_set_style_text_color(m_statusTag, kLive, 0);
        } else if (state.sampleRate) {
            char tag[24];
            snprintf(tag, sizeof(tag), "%lu.%luk/%u", static_cast<unsigned long>(state.sampleRate / 1000),
                     static_cast<unsigned long>((state.sampleRate / 100) % 10), state.bitsPerSample);
            lv_label_set_text(m_statusTag, tag);
            lv_obj_set_style_text_color(m_statusTag, kInkDim, 0);
        } else {
            lv_label_set_text(m_statusTag, "");
        }
    }
    // Amp "on" = actually producing audible sound right now (playing and
    // not muted), not the AMP_ENABLED GPIO state, which is just held HIGH
    // for the board's whole lifetime and never reflects real playback. Both
    // the box border and the "DAC" text go green together.
    if (m_statusAmp && m_statusAmpBox) {
        const bool ampOn = !state.muted && state.transport == PlayerTransport::Playing;
        const lv_color_t ampColor = ampOn ? kLive : kInkFaint;
        lv_obj_set_style_text_color(m_statusAmp, ampColor, 0);
        lv_obj_set_style_border_color(m_statusAmpBox, ampColor, 0);
    }
    if (m_statusCodec) lv_label_set_text(m_statusCodec, state.codec[0] ? state.codec : "");
    const uint8_t volumeLevel = state.volumeSteps ? static_cast<uint8_t>((state.muted ? 0 : state.volume) * 5 / state.volumeSteps) : 0;
    for (uint8_t i = 0; i < 5; ++i) {
        if (!m_volumeBars[i]) continue;
        lv_obj_set_style_bg_color(m_volumeBars[i], i < volumeLevel ? kInk : kInkFaint, 0);
    }
    if (m_statusVolPct) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%u%%", state.volumeSteps ? (state.muted ? 0 : state.volume) * 100 / state.volumeSteps : 0);
        lv_label_set_text(m_statusVolPct, pct);
    }

    if (m_homeClockHour || m_homeClockMinute) {
        char hourBuf[3] = "--";
        char minuteBuf[3] = "--";
        if (state.timeHM[0] && state.timeHM[2] == ':') {
            hourBuf[0] = state.timeHM[0];
            hourBuf[1] = state.timeHM[1];
            hourBuf[2] = '\0';
            minuteBuf[0] = state.timeHM[3];
            minuteBuf[1] = state.timeHM[4];
            minuteBuf[2] = '\0';
        }
        if (m_homeClockHour) lv_label_set_text(m_homeClockHour, hourBuf);
        if (m_homeClockMinute) lv_label_set_text(m_homeClockMinute, minuteBuf);
    }
    if (m_homeClockDate) lv_label_set_text(m_homeClockDate, state.dateStr);
    if (m_homeClockWeather) {
        if (state.weatherTempC > -900) {
            char weather[24];
            snprintf(weather, sizeof(weather), "%s %d℃", state.weatherDesc[0] ? state.weatherDesc : "--",
                     static_cast<int>(state.weatherTempC));
            lv_label_set_text(m_homeClockWeather, weather);
        } else {
            lv_label_set_text(m_homeClockWeather, "");
        }
    }
    if (m_weatherIconBody) {
        lv_color_t bodyColor = kInkFaint;
        lv_color_t dropColor = kAccent;
        bool showLobe = false;
        bool showDrops = false;
        switch (state.weatherIcon) {
            case 0: // clear
                bodyColor = lv_color_hex(0xFBBF24);
                break;
            case 1: // partly cloudy
                bodyColor = lv_color_hex(0xFBBF24);
                showLobe = true;
                break;
            case 2: // overcast
            case 3: // fog
                showLobe = true;
                break;
            case 4: // rain
                showLobe = true;
                showDrops = true;
                dropColor = lv_color_hex(0x38BDF8);
                break;
            case 5: // snow
                showLobe = true;
                showDrops = true;
                dropColor = kInk;
                break;
            case 6: // thunder
                showLobe = true;
                showDrops = true;
                dropColor = kMagenta;
                break;
            default: // no reading yet
                break;
        }
        lv_obj_set_style_bg_color(m_weatherIconBody, bodyColor, 0);
        if (m_weatherIconLobe) {
            if (showLobe) lv_obj_clear_flag(m_weatherIconLobe, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(m_weatherIconLobe, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* drop : {m_weatherIconDrop1, m_weatherIconDrop2}) {
            if (!drop) continue;
            if (showDrops) {
                lv_obj_clear_flag(drop, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(drop, dropColor, 0);
            } else {
                lv_obj_add_flag(drop, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (m_homeNowTitle) lv_label_set_text(m_homeNowTitle, state.title[0] ? state.title : stateText(state.transport));
    if (m_homeNowDetail) lv_label_set_text(m_homeNowDetail, state.detail[0] ? state.detail : "电台 / 本地 / 网络");
    const bool active = state.transport == PlayerTransport::Playing || state.transport == PlayerTransport::Buffering;
    if (m_homePlayIcon) lv_label_set_text(m_homePlayIcon, active ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (m_homeProgress) {
        const uint32_t value = state.durationSeconds ? (state.positionSeconds * 1000UL / state.durationSeconds) : 0;
        lv_bar_set_value(m_homeProgress, value, LV_ANIM_OFF);
    }

    if (m_page == Page::NowPlaying || m_page == Page::Radio) refreshMediaPage(state);
    else if (m_page == Page::LocalNowPlaying) refreshLocalNowPlaying(state);
    else if (m_page == Page::SettingsWifi) refreshSettingsWifi(state);
    refreshCoverSpin(state);
}

void HifiUi::refreshMediaPage(const PlayerSnapshot& state) {
    // Live/duration semantics follow the actual playing source, not which
    // card (PLAY vs RADIO) was used to reach this page -- a radio stream
    // opened via PLAY should still show LIVE, not a bogus 00:00 duration.
    const bool isRadioSource = state.source == PlayerSource::Radio;
    const bool isLive = isRadioSource && state.durationSeconds == 0;

    if (m_title) lv_label_set_text(m_title, state.title[0] ? state.title : stateText(state.transport));
    if (m_detail) lv_label_set_text(m_detail, state.detail[0] ? state.detail : (state.error[0] ? state.error : "Select from Home"));
    if (m_techLine) {
        char tech[64];
        if (state.codec[0] && state.sampleRate) {
            snprintf(tech, sizeof(tech), "%s %lu k  %lu.%luk %ubit", state.codec, static_cast<unsigned long>(state.bitRate / 1000),
                     static_cast<unsigned long>(state.sampleRate / 1000), static_cast<unsigned long>((state.sampleRate / 100) % 10),
                     state.bitsPerSample);
        } else {
            snprintf(tech, sizeof(tech), "%s", stateText(state.transport));
        }
        lv_label_set_text(m_techLine, tech);
    }
    if (m_coverLabel && isRadioSource && state.title[0]) {
        char monogram[4];
        snprintf(monogram, sizeof(monogram), "%.3s", state.title);
        lv_label_set_text(m_coverLabel, monogram);
    }
    if (m_progress) {
        lv_obj_set_style_bg_color(m_progress, isLive ? kLive : kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_color(m_progress, isLive ? kLive : kAccent, LV_PART_INDICATOR);
    }

    char elapsed[16];
    formatTime(elapsed, sizeof(elapsed), state.positionSeconds);
    if (m_elapsed) lv_label_set_text(m_elapsed, elapsed);
    if (isLive) {
        if (m_total) {
            lv_label_set_text(m_total, "LIVE");
            lv_obj_set_style_text_color(m_total, kLive, 0);
        }
        // Real input-buffer occupancy, not a VU-driven fake animation --
        // this is a number the firmware actually knows, per the "only show
        // verifiable state" rule.
        if (m_progress) lv_bar_set_value(m_progress, state.bufferFillPercent * 10, LV_ANIM_OFF);
    } else {
        char total[16];
        formatTime(total, sizeof(total), state.durationSeconds);
        if (m_total) {
            lv_label_set_text(m_total, total);
            lv_obj_set_style_text_color(m_total, kInkDim, 0);
        }
        if (m_progress) {
            const uint32_t value = state.durationSeconds ? (state.positionSeconds * 1000UL / state.durationSeconds) : 0;
            lv_bar_set_value(m_progress, value, LV_ANIM_OFF);
        }
    }

    const bool playing = state.transport == PlayerTransport::Playing || state.transport == PlayerTransport::Buffering;
    if (m_playIcon) lv_label_set_text(m_playIcon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// Disc spin + spectrum ring, shared by whichever page currently owns
// m_cover/m_ringBars (Home's mini player or the NowPlaying/Radio big disc)
// -- called unconditionally from refresh() so both animate, using
// m_ringCx/m_ringCy/m_ringR set by whichever build*() created the ring.
void HifiUi::refreshCoverSpin(const PlayerSnapshot& state) {
    const bool playing = state.transport == PlayerTransport::Playing || state.transport == PlayerTransport::Buffering;

    // Local Now Playing's turntable m_cover is a static disc base (its
    // rotation is handled separately in refreshLocalNowPlaying, spinning
    // just the cover-art image via lv_img_set_angle) -- it must never get
    // whole-object transform_angle here, since m_cover there can have a
    // real photo child and rotating that composite tree doesn't reliably
    // redraw (see the cover-art/cassette-reel bug history).
    if (m_page == Page::LocalNowPlaying) return;

    // Disc spin: only while actually playing, per the animation budget --
    // idle/paused stays still rather than looping forever in the background.
    if (playing && m_cover && !m_coverSpinning) {
        m_coverSpinning = true;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, m_cover);
        lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
            lv_obj_set_style_transform_angle(static_cast<lv_obj_t*>(obj), static_cast<int16_t>(v), 0);
        });
        lv_anim_set_values(&a, 0, 3600);
        lv_anim_set_time(&a, 8000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    } else if (!playing && m_cover && m_coverSpinning) {
        m_coverSpinning = false;
        lv_anim_del(m_cover, nullptr);
    }

    // Spectrum ring: real state.vuLevel drives bar height, with a slow
    // rotating phase so the pattern visibly travels around the ring (reads
    // as motion even though these are simple rects, not a baked texture).
    // Idle/paused: bars settle to a low resting height, no animation.
    const uint8_t vu = playing ? std::max<uint8_t>(state.vuLevel, 40) : 0;
    const uint32_t now = millis();
    const uint8_t barCount = std::count_if(std::begin(m_ringBars), std::end(m_ringBars), [](lv_obj_t* b) { return b != nullptr; });
    for (uint8_t i = 0; i < 10; ++i) {
        if (!m_ringBars[i]) continue;
        uint8_t h = 5;
        if (playing) {
            const uint32_t phase = (now / 60 + i * 26) % 100;
            const uint8_t wave = phase < 50 ? phase : 100 - phase;
            h = std::min<uint8_t>(16, std::max<uint8_t>(4, static_cast<uint8_t>(vu * (10 + wave) / 255)));
        }
        lv_obj_set_height(m_ringBars[i], h);
        const float a = (static_cast<float>(i) / static_cast<float>(barCount ? barCount : 10)) * 2.0f * 3.14159265f;
        lv_obj_set_pos(m_ringBars[i], m_ringCx + static_cast<int16_t>(cosf(a) * m_ringR) - 1,
                        m_ringCy + static_cast<int16_t>(sinf(a) * m_ringR) - h / 2);
    }
}

void HifiUi::onHomeAction(lv_event_t* event) {
    if (!s_instance) return;
    const auto page = static_cast<Page>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    printf("[LVGL] home action page=%u\n", static_cast<unsigned>(page));
    s_instance->show(page);
}

void HifiUi::onTransportAction(lv_event_t* event) {
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    printf("[LVGL] transport action=%lu\n", static_cast<unsigned long>(action));
    if (action == kActionPrev) {
        playerService.previousStation();
        if (s_instance) s_instance->clearCoverArt(); // moving to a radio station -- any cached local-track art is now stale
    } else if (action == kActionPlayPause) {
        const PlayerSnapshot state = playerService.snapshot();
        const bool idle = state.transport == PlayerTransport::Stopped || state.transport == PlayerTransport::Error ||
                          state.source == PlayerSource::None;
        bool showPause;
        if (idle) {
            if (playerService.radioStationCount()) playerService.playRadioStation(1);
            else playerService.playRadioUrl(kDefaultRadioUrl);
            if (s_instance) s_instance->clearCoverArt();
            showPause = true; // starting playback
        } else {
            showPause = !playerService.togglePause();
        }
        // Flip the glyph now instead of waiting up to 100ms for the next
        // refresh -- the press should feel immediate. refresh() reconciles
        // with the real transport state on its next tick regardless. Both
        // icons are checked since either the media page's or Home's quick
        // control may be the one that was tapped.
        if (s_instance) {
            const char* glyph = showPause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
            if (s_instance->m_playIcon) lv_label_set_text(s_instance->m_playIcon, glyph);
            if (s_instance->m_homePlayIcon) lv_label_set_text(s_instance->m_homePlayIcon, glyph);
        }
    } else if (action == kActionNext) {
        playerService.nextStation();
        if (s_instance) s_instance->clearCoverArt();
    } else if (action == kActionOpenSettings && s_instance) {
        s_instance->show(Page::Settings);
    } else if (action == kActionOpenList && s_instance) {
        s_instance->show(Page::RadioList);
    } else if (action == kActionBack && s_instance) {
        s_instance->show(s_instance->m_returnPage);
    }
}

void HifiUi::onRadioStationAction(lv_event_t* event) {
    const uint16_t stationIndex = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (!stationIndex) return;
    playerService.playRadioStation(stationIndex);
    if (s_instance) {
        s_instance->clearCoverArt();
        s_instance->show(Page::Radio);
    }
}

void HifiUi::onMusicTabAction(lv_event_t* event) {
    if (!s_instance) return;
    const uint8_t tab = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_instance->m_musicTab = tab;
    s_instance->m_musicFilterArtist[0] = '\0';
    s_instance->m_musicFilterAlbum[0] = '\0';
    s_instance->show(Page::Sd);
}

void HifiUi::onMusicTrackAction(lv_event_t* event) {
    if (!s_instance) return;
    const uint16_t trackIndex1 = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (!trackIndex1) return;
    s_instance->playLocalTrackByIndex(trackIndex1 - 1);
    s_instance->show(Page::LocalNowPlaying);
}

void HifiUi::onLocalTransportAction(lv_event_t* event) {
    if (!s_instance) return;
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    printf("[LOCAL] transport action=%lu curIdx=%u\n", static_cast<unsigned long>(action), s_instance->m_currentLocalTrackIndex);
    if (action == kActionPrev || action == kActionNext) {
        const int32_t next = s_instance->findLocalTrack(s_instance->m_currentLocalTrackIndex, action == kActionNext);
        printf("[LOCAL] findLocalTrack -> %ld\n", static_cast<long>(next));
        if (next >= 0) {
            s_instance->playLocalTrackByIndex(static_cast<uint16_t>(next));
            s_instance->show(Page::LocalNowPlaying); // full rebuild -- the cover art image needs a fresh lv_img_dsc_t
        }
    } else if (action == kActionPlayPause) {
        const bool showPause = !playerService.togglePause();
        printf("[LOCAL] playPause -> showPause=%d\n", showPause);
        if (s_instance->m_playIcon) lv_label_set_text(s_instance->m_playIcon, showPause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

void HifiUi::onLocalViewToggleAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    s_instance->m_localCassetteView = !s_instance->m_localCassetteView;
    printf("[LOCAL] view toggle -> cassette=%d\n", s_instance->m_localCassetteView);
    s_instance->show(Page::LocalNowPlaying);
}

void HifiUi::onLocalSeekAction(lv_event_t* event) {
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int32_t value = lv_slider_get_value(slider); // 0-1000
    const PlayerSnapshot state = playerService.snapshot();
    if (!state.durationSeconds) return;
    const uint32_t targetSeconds = static_cast<uint32_t>((static_cast<uint64_t>(value) * state.durationSeconds) / 1000);
    playerService.seekTo(targetSeconds);
}

void HifiUi::onLyricRetryAction(lv_event_t* event) {
    (void)event;
    if (!s_instance || s_instance->m_localCassetteView) return; // cassette view repurposes m_detail, not a retry button there
    if (playerService.lyricsFetchState(s_instance->m_currentLocalTrackIndex) != LyricFetchState::NotFound) return;
    playerService.retryLyricsFetch(s_instance->m_currentLocalTrackIndex);
    lv_label_set_text(s_instance->m_detail, "正在联网查询歌词...");
}

void HifiUi::onLocalPlayModeToggleAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    switch (s_instance->m_localPlayMode) {
        case LocalPlayMode::Sequential: s_instance->m_localPlayMode = LocalPlayMode::RepeatAll; break;
        case LocalPlayMode::RepeatAll: s_instance->m_localPlayMode = LocalPlayMode::RepeatOne; break;
        case LocalPlayMode::RepeatOne: s_instance->m_localPlayMode = LocalPlayMode::Shuffle; break;
        case LocalPlayMode::Shuffle: s_instance->m_localPlayMode = LocalPlayMode::Sequential; break;
    }
    const bool isSequential = s_instance->m_localPlayMode == LocalPlayMode::Sequential;
    const lv_color_t iconColor = isSequential ? kInkDim : kAccentBright;
    if (s_instance->m_shuffleIcon) {
        lv_label_set_text(s_instance->m_shuffleIcon, localPlayModeSymbol(s_instance->m_localPlayMode));
        lv_obj_set_style_text_color(s_instance->m_shuffleIcon, iconColor, 0);
        if (isSequential) lv_obj_add_flag(s_instance->m_shuffleIcon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_instance->m_shuffleIcon, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_instance->m_seqIconWrap) {
        if (isSequential) lv_obj_clear_flag(s_instance->m_seqIconWrap, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_instance->m_seqIconWrap, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t* bar : s_instance->m_seqIconBars) {
        if (bar) lv_obj_set_style_bg_color(bar, iconColor, 0);
    }
}

void HifiUi::onWifiManageOpenAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    s_instance->m_wifiShowManageQr = true;
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiManageBackAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    s_instance->m_wifiShowManageQr = false;
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiAddOpenAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    // Non-blocking -- the actual WiFi.scanNetworks() runs on its own task
    // (see playerCoreWifiScanStart()'s comment on why: doing it right here
    // used to block the UI/audio task for several seconds, long enough to
    // reboot the device). refreshSettingsWifi() polls for completion.
    s_instance->m_wifiScanCount = 0;
    s_instance->m_wifiScanPending = true;
    playerService.wifiScanStart();
    s_instance->m_wifiShowAddNetwork = true;
    s_instance->m_wifiAddStage = WifiAddStage::ScanList;
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiAddBackAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    if (s_instance->m_wifiAddStage == WifiAddStage::PasswordEntry) {
        // Back from password entry -> the scan list, not all the way out.
        s_instance->m_wifiAddStage = WifiAddStage::ScanList;
    } else {
        s_instance->m_wifiShowAddNetwork = false;
    }
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiScanRowAction(lv_event_t* event) {
    if (!s_instance) return;
    const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (index >= s_instance->m_wifiScanCount) return;
    strlcpy(s_instance->m_wifiAddSelectedSsid, s_instance->m_wifiScanResults[index].ssid, sizeof(s_instance->m_wifiAddSelectedSsid));
    s_instance->m_wifiAddStage = WifiAddStage::PasswordEntry;
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiAddSaveAction(lv_event_t* event) {
    (void)event;
    if (!s_instance || !s_instance->m_wifiAddPwField) return;
    if (!s_instance->m_wifiAddSelectedSsid[0]) return;
    const char* pw = lv_textarea_get_text(s_instance->m_wifiAddPwField);
    playerService.wifiAddNetwork(s_instance->m_wifiAddSelectedSsid, pw);
    s_instance->m_wifiShowAddNetwork = false;
    s_instance->show(Page::SettingsWifi);
}

void HifiUi::onWifiSavedRowAction(lv_event_t* event) {
    if (!s_instance) return;
    const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    char ssid[33];
    bool isDefault = false;
    if (!playerService.wifiSavedInfo(index, ssid, sizeof(ssid), &isDefault)) return;
    const PlayerSnapshot state = playerService.snapshot();
    if (state.wifiConnected && strcmp(ssid, state.wifiSsid) == 0) return; // already on this one
    // The credential is already saved (wifiMulti has every saved AP added
    // since boot, see connectToWiFi() in main.cpp) -- no password needed,
    // just nudge it to retry now instead of waiting for the next
    // opportunistic reconnect tick.
    playerService.wifiReconnect();
    if (s_instance->m_wifiStatusText) lv_label_set_text(s_instance->m_wifiStatusText, "正在尝试连接...");
}

void HifiUi::onMusicGroupAction(lv_event_t* event) {
    if (!s_instance) return;
    const uint8_t groupIndex = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (groupIndex >= s_instance->m_musicGroupCount) return;
    const char* name = s_instance->m_musicGroupNames[groupIndex];
    if (s_instance->m_musicTab == 1) strlcpy(s_instance->m_musicFilterArtist, name, sizeof(s_instance->m_musicFilterArtist));
    else strlcpy(s_instance->m_musicFilterAlbum, name, sizeof(s_instance->m_musicFilterAlbum));
    s_instance->m_musicTab = 0;
    s_instance->show(Page::Sd);
}

void HifiUi::onMusicClearFilterAction(lv_event_t* event) {
    (void)event;
    if (!s_instance) return;
    s_instance->m_musicFilterArtist[0] = '\0';
    s_instance->m_musicFilterAlbum[0] = '\0';
    s_instance->show(Page::Sd);
}

void HifiUi::handleGesture(TouchGesture gesture) {
    if (gesture == TouchGesture::EdgeBack) {
        printf("[LOCAL] EdgeBack gesture, page=%d cassette=%d\n", static_cast<int>(m_page), m_localCassetteView);
        // Cassette view has no buttons at all -- its only exit is this
        // left-edge swipe, back to the flat card, not all the way Home.
        if (m_page == Page::LocalNowPlaying && m_localCassetteView) {
            m_localCassetteView = false;
            show(Page::LocalNowPlaying);
            return;
        }
        if (m_page != Page::Home) show(Page::Home);
        return;
    }
    if (gesture == TouchGesture::EdgeTopOpen) {
        setQuickPanelOpen(true);
        return;
    }
    if (gesture == TouchGesture::EdgeBottomClose) {
        setQuickPanelOpen(false);
        return;
    }
}
