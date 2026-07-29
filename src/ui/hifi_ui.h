#pragma once

#include "player_service.h"
#include "waveshare_lvgl_port.h"

class HifiUi {
  public:
    bool begin();
    void tick();

  private:
    // 320x170 design spec: docs/UI_DESIGN_SPEC.md. RadioList is the station
    // browser reached from the Radio page's List slot; NowPlaying/Radio
    // themselves share one skeleton (buildMediaPage) per that spec.
    enum class Page : uint8_t { Home, NowPlaying, Radio, RadioList, Sd, LocalNowPlaying, Clock, Settings, SettingsWifi, FontPreview };
    // Cycled by tapping the play-mode button: 顺序播放 (stop at the end of
    // the filtered list) -> 列表循环 (wrap back to the start) -> 单曲循环
    // (replay the same track) -> 随机播放 (genuine random pick) -> back to
    // Sequential. Drives both findLocalTrack()'s search behavior and the
    // auto-advance-on-track-end logic in refresh() (see m_lastEofCount).
    enum class LocalPlayMode : uint8_t { Sequential, RepeatAll, RepeatOne, Shuffle };

    static void onHomeAction(lv_event_t* event);
    static void onTransportAction(lv_event_t* event);
    static void onRadioStationAction(lv_event_t* event);
    static void onMusicTabAction(lv_event_t* event);
    static void onMusicTrackAction(lv_event_t* event);
    static void onMusicGroupAction(lv_event_t* event);
    static void onMusicClearFilterAction(lv_event_t* event);
    static void onLocalTransportAction(lv_event_t* event);
    static void onLocalViewToggleAction(lv_event_t* event);
    static void onLocalSeekAction(lv_event_t* event);
    static void onLocalPlayModeToggleAction(lv_event_t* event);
    static void onLyricRetryAction(lv_event_t* event);
    static void onWifiSavedRowAction(lv_event_t* event);
    static void onWifiManageOpenAction(lv_event_t* event);
    static void onWifiManageBackAction(lv_event_t* event);
    static void onWifiAddOpenAction(lv_event_t* event);
    static void onWifiAddBackAction(lv_event_t* event);
    static void onWifiScanRowAction(lv_event_t* event);
    static void onWifiAddSaveAction(lv_event_t* event);
    static void onQuickVolumeAction(lv_event_t* event);
    static void onQuickBrightnessAction(lv_event_t* event);
    static void onQuickEqAction(lv_event_t* event);

    void show(Page page);
    void buildHome();
    void buildMediaPage(bool isRadio);
    void buildRadioList();
    void buildLocalMusic();
    void buildLocalNowPlaying();
    void buildCassetteVisual(lv_obj_t* screen);
    void loadCoverArt(uint16_t trackIndex);
    void clearCoverArt();
    void playLocalTrackByIndex(uint16_t index);
    int32_t findLocalTrack(uint16_t from, bool forward, bool wrap = true) const;
    static const char* localPlayModeSymbol(LocalPlayMode mode);
    void refreshLocalNowPlaying(const struct PlayerSnapshot& state);
    void handleGesture(TouchGesture gesture);
    void buildPlaceholder(const char* title, const char* detail);
    void buildSettings();
    void buildSettingsWifi();
    void buildFontPreview();
    void refreshSettingsWifi(const struct PlayerSnapshot& state);
    void buildStatusBar(lv_obj_t* screen);
    void buildQuickPanel();
    void setQuickPanelOpen(bool open);
    void refreshQuickPanel();
    lv_obj_t* makePanel(lv_obj_t* parent, int16_t x, int16_t y, int16_t width, int16_t height, bool highlight = false);
    lv_obj_t* makeControlSlot(lv_obj_t* parent, const char* symbol, uintptr_t action, int16_t slotIndex, bool primary = false);
    void refresh();
    void refreshMediaPage(const struct PlayerSnapshot& state);
    void refreshCoverSpin(const struct PlayerSnapshot& state);
    lv_obj_t* makeCard(lv_obj_t* parent, const char* icon, const char* label, Page page, int16_t x, int16_t y, int16_t width, int16_t height);
    lv_obj_t* makeText(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color, lv_align_t align, int16_t x, int16_t y);

    WaveshareLvglPort m_port;
    static HifiUi* s_instance;
    Page m_page = Page::Home;
    Page m_returnPage = Page::Home;
    bool m_mediaPageIsRadio = false;
    lv_obj_t* m_title = nullptr;
    lv_obj_t* m_detail = nullptr;
    lv_obj_t* m_techLine = nullptr;
    lv_obj_t* m_progress = nullptr;
    lv_obj_t* m_elapsed = nullptr;
    lv_obj_t* m_total = nullptr;
    lv_obj_t* m_statusTag = nullptr;
    lv_obj_t* m_statusTime = nullptr;
    lv_obj_t* m_statusWifiIcon = nullptr; // color itself carries signal strength, no separate bar graph
    lv_obj_t* m_statusAmpBox = nullptr;   // bordered "DAC" tag; box+text both go green when amp is live
    lv_obj_t* m_statusAmp = nullptr;
    lv_obj_t* m_statusCodec = nullptr;
    lv_obj_t* m_statusVolPct = nullptr;
    lv_obj_t* m_volumeBars[5]{};
    lv_obj_t* m_homeClockHour = nullptr;
    lv_obj_t* m_homeClockMinute = nullptr;
    lv_obj_t* m_homeClockDate = nullptr;
    lv_obj_t* m_homeClockWeather = nullptr;
    lv_obj_t* m_weatherIconBody = nullptr;
    lv_obj_t* m_weatherIconLobe = nullptr;
    lv_obj_t* m_weatherIconDrop1 = nullptr;
    lv_obj_t* m_weatherIconDrop2 = nullptr;
    lv_obj_t* m_cover = nullptr;
    lv_obj_t* m_coverLabel = nullptr;
    lv_obj_t* m_playIcon = nullptr;
    lv_obj_t* m_playRing = nullptr;
    lv_obj_t* m_ringBars[10]{};
    int16_t m_ringCx = 0;
    int16_t m_ringCy = 0;
    int16_t m_ringR = 0;
    bool m_coverSpinning = false;
    lv_obj_t* m_homeNowTitle = nullptr;
    lv_obj_t* m_homeNowDetail = nullptr;
    lv_obj_t* m_homePlayIcon = nullptr;
    lv_obj_t* m_homeProgress = nullptr;
    uint32_t m_lastRefresh = 0;

    // Local Music browser state -- persists across rebuilds (tab taps and
    // filter taps both just call show(Page::Sd) again), unlike the widget
    // pointers above which show() resets every time.
    uint8_t m_musicTab = 0; // 0 songs, 1 artists, 2 albums
    char m_musicFilterArtist[48]{};
    char m_musicFilterAlbum[48]{};
    static constexpr uint8_t kMaxMusicGroups = 24;
    char m_musicGroupNames[kMaxMusicGroups][48]{};
    uint8_t m_musicGroupCount = 0;
    uint16_t m_currentLocalTrackIndex = 0;
    bool m_hasLyrics = false; // whether loadLyrics() found any lines for the current track
    // Local Now Playing has two interchangeable visual modes, toggled by a
    // tap (see onLocalViewToggleAction) -- persists across rebuilds like
    // the tab/filter state above.
    bool m_localCassetteView = false;
    // Dot-matrix spectrum, drawn as one lv_canvas rather than 308 individual
    // lv_obj cells (see buildLocalNowPlaying()'s comment) -- each of those
    // objects had its own invalidate/redraw pipeline, which is what made the
    // spectrum visibly stutter. m_specCanvasBuf is a PSRAM pixel buffer that
    // outlives any single screen rebuild (sized once, reused every time this
    // page is (re)entered) -- only the lv_obj_t* canvas wrapper itself needs
    // recreating each time, via show()'s screen delete/rebuild.
    lv_obj_t* m_specCanvas = nullptr;
    lv_color_t* m_specCanvasBuf = nullptr;
    uint8_t m_specCols = 0;
    uint8_t m_specRows = 0;
    uint8_t m_specLastLit[32]{}; // per-column lit-count cache so refresh only touches cells that actually changed
    // Cycled by tapping the play-mode button: 顺序播放 (stop at the end of
    LocalPlayMode m_localPlayMode = LocalPlayMode::Sequential;
    lv_obj_t* m_shuffleIcon = nullptr;    // LV_SYMBOL_LOOP/LOOP+"1"/SHUFFLE text, for the non-Sequential modes
    lv_obj_t* m_seqIconWrap = nullptr;    // parent of the 3 bars, toggled HIDDEN as a whole
    lv_obj_t* m_seqIconBars[3]{};         // custom ascending-bars glyph, shown only for Sequential (see addSequentialIcon)
    // eofCount edge-detection for local-track auto-advance -- Audio::evt_eof
    // fires for radio disconnects too, so this is only acted on while
    // m_currentLocalTrackIndex reflects an actual local-playback session
    // (see refresh()'s handling).
    uint32_t m_lastEofCount = 0;
    PlayerSource m_lastSourceSeen = PlayerSource::None;
    lv_obj_t* m_cassetteReelL = nullptr;
    lv_obj_t* m_cassetteReelR = nullptr;
    lv_obj_t* m_cassetteNeedle = nullptr;
    bool m_cassetteSpinning = false;

    // Real embedded cover art for whichever local track was tapped in
    // buildLocalMusic() (see loadCoverArt()) -- only ever set from that one
    // path, since it's the only place that both knows the track index and
    // is about to navigate to NowPlaying, so the art is guaranteed to match
    // what's about to play. Cleared on radio/next/prev (see
    // clearCoverArt() call sites) since those never carry known art.
    uint16_t* m_coverArtPixels = nullptr;
    lv_img_dsc_t m_coverArtDsc{};
    lv_obj_t* m_coverArtWrap = nullptr;
    lv_obj_t* m_coverArtImg = nullptr;
    bool m_coverArtSpinning = false; // local flat-card turntable: spins the cover photo itself, not its wrap

    // Settings > WiFi screen: QR content is only regenerated when the
    // underlying string actually changes (connect/disconnect, AP fallback
    // toggling) -- lv_qrcode_update() re-renders the whole matrix, wasteful
    // to do every refresh tick when nothing changed.
    lv_obj_t* m_wifiQr = nullptr;
    lv_obj_t* m_wifiStatusText = nullptr;
    lv_obj_t* m_wifiHintText = nullptr;
    char m_wifiQrLastContent[128]{};

    // Settings > WiFi list mode: saved networks only (from NVS) -- no
    // on-device scanning. Discovering/adding brand-new networks moved to
    // the phone-only /wifi_manage admin page (see onWifiManageOpenAction),
    // which does its own on-demand WiFi.scanNetworks() when loaded --
    // scanning from the device itself was extra background-task/heap
    // pressure competing with audio's I2S DMA and WiFi's own
    // esp_wifi_init() at boot (see setupLvglRuntime() in main.cpp) for no
    // real benefit, since the phone page covers the same need.
    lv_obj_t* m_wifiNetworkList = nullptr;
    uint8_t m_wifiLastSavedCount = 0xFF; // forces a rebuild the first time refreshSettingsWifi() sees a real count
    // Sub-view: a QR code linking to the phone-friendly /wifi_manage web
    // page (scan + saved-network admin), reached via a small button in the
    // list header (see onWifiManageOpenAction/onWifiManageBackAction).
    bool m_wifiShowManageQr = false;
    // Sub-view: manual on-screen "add a network" flow -- for a location the
    // device has never seen before (no saved network to retry, and the QR
    // page itself needs an existing connection to have an IP to encode, so
    // it can't bootstrap a brand-new network either). Two stages:
    //   1. ScanList -- an on-demand (not automatic/boot-time) WiFi.scanNetworks(),
    //      triggered only by opening this screen (see onWifiAddOpenAction).
    //   2. PasswordEntry -- full-screen (status bar hidden, see
    //      buildSettingsWifi()) keyboard entry for the tapped network's
    //      password; "connect" persists + retries via the same
    //      playerCoreWifiAddNetwork() path the phone page already used.
    enum class WifiAddStage : uint8_t { ScanList, PasswordEntry };
    bool m_wifiShowAddNetwork = false;
    WifiAddStage m_wifiAddStage = WifiAddStage::ScanList;
    static constexpr uint8_t kWifiScanMaxItems = 12;
    WifiScanItem m_wifiScanResults[kWifiScanMaxItems]{};
    uint8_t m_wifiScanCount = 0;
    // True from the moment onWifiAddOpenAction kicks off the background
    // scan until refreshSettingsWifi() notices playerService.wifiScanInProgress()
    // has gone false and pulls the results -- see that comment.
    bool m_wifiScanPending = false;
    char m_wifiAddSelectedSsid[33]{};
    lv_obj_t* m_wifiAddPwField = nullptr;
    lv_obj_t* m_wifiAddKeyboard = nullptr;

    // Quick-settings drawer: lives on lv_layer_top(), independent of
    // show()/Page navigation, so it can be pulled down over whatever screen
    // is currently active (see handleGesture's EdgeTopOpen/EdgeBottomClose)
    // instead of being its own Page. Built lazily on first open, then just
    // shown/hidden -- avoids fighting the LVGL memory pool the way rebuilding
    // Local Now Playing's dot-matrix grid on every screen swap used to (see
    // LV_MEM_SIZE's history in lv_conf.h).
    lv_obj_t* m_quickPanel = nullptr;
    lv_obj_t* m_quickVolumeSlider = nullptr;
    lv_obj_t* m_quickVolumeLabel = nullptr;
    lv_obj_t* m_quickBrightnessSlider = nullptr;
    lv_obj_t* m_quickBrightnessLabel = nullptr;
    lv_obj_t* m_quickEqSliders[3] = {nullptr, nullptr, nullptr}; // low, mid, high
    lv_obj_t* m_quickEqLabels[3] = {nullptr, nullptr, nullptr};
    bool m_quickPanelOpen = false;
    // audio.setTone() is write-only (no getter), so the current gains have
    // to be tracked here to seed the sliders and compute new values.
    int8_t m_eqLow = 0;
    int8_t m_eqMid = 0;
    int8_t m_eqHigh = 0;
};
