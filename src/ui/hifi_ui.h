#pragma once

#include "player_service.h"
#include "waveshare_lvgl_port.h"

class HifiUi {
  public:
    bool begin();
    void tick();
    // USB storage mode: show the USB page (status/debug/unmount) directly,
    // without going through the normal page stack.
    static void showUsbStoragePage();

  private:
    // 320x170 design spec: docs/UI_DESIGN_SPEC.md. RadioList is the station
    // browser reached from the Radio page's List slot; NowPlaying/Radio
    // themselves share one skeleton (buildMediaPage) per that spec.
    enum class Page : uint8_t {
        Home,
        NowPlaying,
        Radio,
        RadioList,
        Sd,
        LocalNowPlaying,
        Clock,
        Settings,
        SettingsWifi,
        UsbStorage,
        FontPreview,
        AudioHome,
        AudioDecode,
        AudioOutputDetails,
        AudioOutputPolicy,
        AudioEq,
        AudioEqBand,
        AudioEffects,
        AudioDac,
        // Phase 2 of the online-music (在线音乐/网易云) feature: gateway
        // URL + device key entry and connection/wake test only -- no
        // search/browse/playback pages yet (those land in later phases).
        CloudMusicSettings,
        // Phase 3: browse-only (hot playlists / search / playlist detail),
        // no playback wiring yet -- see docs spec's phased plan.
        CloudMusicHome,
        CloudMusicSearch,
        CloudMusicPlaylist,
        // Phase 5: dedicated online-music player (same core as radio/local)
        // plus the category sub-pages it links to: hot playlists (with
        // cover thumbnails), song-ranking charts, and new-song arrivals.
        CloudNowPlaying,
        CloudHotPlaylists,
        CloudRankings,
        CloudNewSongs,
        CloudLanguage
    };
    // Cycled by tapping the play-mode button: 顺序播放 (stop at the end of
    // the filtered list) -> 列表循环 (wrap back to the start) -> 单曲循环
    // (replay the same track) -> 随机播放 (genuine random pick) -> back to
    // Sequential. Drives both findLocalTrack()'s search behavior and the
    // auto-advance-on-track-end logic in refresh() (see m_lastEofCount).
    enum class LocalPlayMode : uint8_t { Sequential, RepeatAll, RepeatOne, Shuffle };
    // CloudMusicSettings sub-views -- same one-field-at-a-time shape as
    // WifiAddStage (see buildCloudMusicSettings()): editing either field
    // gets the field its own full-width row and the keyboard its full
    // 150px, instead of both fields + keyboard fighting for room on one
    // screen at once (which is what produced garbled/clipped input before).
    enum class CloudMusicConfigStage : uint8_t { Overview, EditBaseUrl, EditDeviceKey };

    static void onHomeAction(lv_event_t* event);
    static void onHomeNowPlayingAction(lv_event_t* event);
    static void onTransportAction(lv_event_t* event);
    static void onRadioStationAction(lv_event_t* event);
    static void onMusicTabAction(lv_event_t* event);
    static void onMusicTrackAction(lv_event_t* event);
    static void onMusicGroupAction(lv_event_t* event);
    static void onMusicClearFilterAction(lv_event_t* event);
    static void onLocalTransportAction(lv_event_t* event);
    static void onLocalViewToggleAction(lv_event_t* event);
    static void onRadioViewToggleAction(lv_event_t* event);
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
    static void onUsbStorageAction(lv_event_t* event);
    static void onCloudMusicEditBaseUrlAction(lv_event_t* event);
    static void onCloudMusicEditDeviceKeyAction(lv_event_t* event);
    static void onCloudMusicConfigBackAction(lv_event_t* event);
    static void onCloudMusicSaveAction(lv_event_t* event);
    static void onCloudMusicTestAction(lv_event_t* event);
    static void onCloudMusicQrAction(lv_event_t* event);
    static void onCloudMusicHistoryAction(lv_event_t* event);
    static void onCloudHistoryUseAction(lv_event_t* event);
    static void onCloudHistoryDeleteAction(lv_event_t* event);
    static void onCloudMusicPlaylistOpenAction(lv_event_t* event);
    static void onCloudMusicSearchOpenAction(lv_event_t* event);
    static void onCloudMusicSearchGoAction(lv_event_t* event);
    static void onCloudMusicTrackRowAction(lv_event_t* event);
    static void onCloudCategoryAction(lv_event_t* event);
    static void onCloudRankingRowAction(lv_event_t* event);
    static void onCloudLanguageAction(lv_event_t* event);
    static void onCloudTransportAction(lv_event_t* event);
    static void onCloudLyricRetryAction(lv_event_t* event);
    static void onQuickVolumeAction(lv_event_t* event);
    static void onQuickBrightnessAction(lv_event_t* event);
    static void onQuickEqAction(lv_event_t* event);
    static void onAudioEqPresetAction(lv_event_t* event);
    static void onAudioEqSliderAction(lv_event_t* event);
    static void onAudioEqBandOpenAction(lv_event_t* event);
    static void onAudioEqBandAdjustAction(lv_event_t* event);
    static void onAudioOutputPolicyAction(lv_event_t* event);

    void show(Page page);
    // Like show(), but pushes the current page unconditionally instead of
    // collapsing the stack when the target is an ancestor -- used by the
    // cloud player's LIST/HOME slots so swiping back from the list returns
    // to the player (normal show() would truncate the stack to the
    // ancestor and make back land on Home).
    void showKeepingStack(Page page);
    void navigateBack();
    void buildHome();
    void buildMediaPage(bool isRadio);
    void buildRadioList();
    void buildRadioNowPlaying();
    void refreshRadioNowPlaying(const struct PlayerSnapshot& state);
    void loadRadioIcon(uint16_t stationIndex);
    void clearRadioIcon();
    void buildLocalMusic();
    void buildLocalNowPlaying();
    void buildCassetteVisual(lv_obj_t* screen);
    void loadCoverArt(uint16_t trackIndex);
    void clearCoverArt();
    void playLocalTrackByIndex(uint16_t index, uint32_t positionSeconds = 0);
    int32_t findLocalTrackByPath(const char* path) const;
    int32_t findLocalTrack(uint16_t from, bool forward, bool wrap = true) const;
    static const char* localPlayModeSymbol(LocalPlayMode mode);
    void refreshLocalNowPlaying(const struct PlayerSnapshot& state);
    void handleGesture(TouchGesture gesture);
    void buildPlaceholder(const char* title, const char* detail);
    void buildSettings();
    void buildSettingsWifi();
    void buildUsbStorage();
    void refreshUsbStorage();
    void buildCloudMusicSettings();
    void refreshCloudMusicSettings();
    void buildCloudMusicHome();
    void refreshCloudMusicHome();
    void buildCloudMusicSearch();
    void refreshCloudMusicSearch();
    void buildCloudMusicPlaylist();
    void refreshCloudMusicPlaylist();
    void buildCloudHotPlaylists();
    void refreshCloudHotPlaylists();
    void buildCloudRankings();
    void refreshCloudRankings();
    void buildCloudNewSongs();
    void refreshCloudNewSongs();
    void buildCloudLanguage();
    void buildCloudNowPlaying();
    void refreshCloudNowPlaying(const struct PlayerSnapshot& state);
    void loadCloudCover();
    void clearCloudRowThumbs();
    // Shared by refreshCloudMusicSearch()/refreshCloudMusicPlaylist(): a
    // resolve triggered by tapping a track row overlays Loading/Error text
    // on m_cloudListHint without touching the list itself. Returns true if
    // it changed anything (caller can use this to know whether the rest of
    // its own refresh logic should still run this tick).
    bool refreshCloudResolveOverlay();
    void buildFontPreview();
    void buildAudioHome();
    void buildAudioDecode();
    void buildAudioOutputDetails();
    void buildAudioOutputPolicy();
    void buildAudioEq();
    void buildAudioEqBand();
    void buildAudioEffects();
    void buildAudioDac();
    void buildAudioTopBar(const char* title, const char* rightText = nullptr, bool rightOk = false, const char* rightIcon = nullptr);
    lv_obj_t* makeAudioRow(lv_obj_t* parent, int16_t y, const char* icon, const char* label, const char* value, Page page);
    lv_obj_t* makeAudioNavTile(lv_obj_t* parent, int16_t x, int16_t y, int16_t width, const char* icon, const char* title,
                               const char* subtitle, Page page, bool selected = false);
    void syncAudioToneFromService();
    void applyAudioTone(bool force = false);
    void scheduleAudioToneSave();
    void processDeferredAudioTone();
    void refreshAudioEqControls();
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
    static constexpr uint8_t kPageStackCapacity = 8;
    Page m_pageStack[kPageStackCapacity]{};
    uint8_t m_pageStackDepth = 0;
    bool m_navigatingBack = false;
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
    lv_obj_t* m_homeNowLyric = nullptr;
    lv_obj_t* m_homeProgress = nullptr;
    // Home's own square cover slot (not the vinyl-disc m_cover/m_ringBars
    // used by the big Now Playing pages) -- shows the real art already
    // decoded into the shared m_coverArtPixels/m_coverArtDsc (see
    // loadCoverArt()/loadRadioIcon()) if some page loaded it, else a plain
    // glyph placeholder. Widgets only, rebuilt every time; the underlying
    // pixel buffer is the same cross-page one and isn't owned here.
    lv_obj_t* m_homeCoverWrap = nullptr;
    lv_obj_t* m_homeCoverImg = nullptr;
    lv_obj_t* m_homeCoverPlaceholder = nullptr;
    // Home's mini dot-matrix spectrum -- same drawSpecDot() cell drawing as
    // Local/Radio Now Playing's full-size one, just far fewer rows and a
    // tighter column pitch to fit this much smaller strip (see buildHome).
    lv_obj_t* m_homeSpecCanvas = nullptr;
    lv_color_t* m_homeSpecCanvasBuf = nullptr; // persists across rebuilds, same pattern as m_specCanvasBuf
    uint8_t m_homeSpecLastLit[28]{};
    // Only scroll m_homeNowTitle (LV_LABEL_LONG_SCROLL_CIRCULAR) while a real
    // title is showing -- switching modes every refresh tick regardless of
    // content made the idle "Ready" placeholder marquee-scroll forever, which
    // reads as a bug since there's nothing to actually scroll. Tracks which
    // mode is currently set so refresh() only calls lv_label_set_long_mode on
    // an actual idle<->playing transition, not every tick.
    bool m_homeTitleHasContent = false;
    // lv_label_set_text() unconditionally re-measures the text and restarts
    // the SCROLL_CIRCULAR offset animation (see lv_label_refr_text() in
    // lv_label.c) even when called with identical text -- calling it every
    // refresh() tick regardless of whether the content actually changed
    // made the marquee visibly stutter/reset. These cache the last string
    // actually applied so refresh() only calls set_text on a real change.
    char m_homeTitleLastText[96]{};
    char m_homeLyricLastText[96]{};
    // 9-segment VU ladder shown instead of m_homeProgress when the source is
    // Radio (no seek position to show for a live stream) -- mirrors Radio
    // Now Playing's m_vfdSegments (see buildRadioNowPlaying), just placed in
    // the progress bar's slot here.
    lv_obj_t* m_homeVfdSegments[9]{};
    // Mini metrics row (peak hold / buffer % / bitrate), above the spectrum
    // -- only meaningful for a live radio stream, blank for local playback.
    // Peak hold decay tracking is a local copy of Radio Now Playing's own
    // (see m_peakHoldRawL there), not shared -- this is a single combined
    // peak, not per-channel.
    // Whole-row container (icons + value labels) so refresh() can hide/show
    // the entire row with one flag instead of just blanking the value text
    // -- blanking alone left the icons themselves permanently visible for
    // local playback, still eating into the lyric line's space beneath it.
    lv_obj_t* m_homeMetricsRow = nullptr;
    lv_obj_t* m_homeMetricPeak = nullptr;
    lv_obj_t* m_homeMetricBuffer = nullptr;
    lv_obj_t* m_homeMetricRate = nullptr;
    uint8_t m_homePeakHoldRaw = 0;
    uint32_t m_homePeakHoldTimestamp = 0;
    // Which of the two layouts buildHome() actually built (Radio: metrics
    // row + VU ladder; anything else: lyric line + progress bar) -- only
    // one is ever constructed, not both-with-one-hidden, so lyric and
    // metrics never fight over the same vertical space again. refresh()
    // compares this against the live state.source and calls show(Page::
    // Home) to rebuild with the other layout the moment the actual playing
    // source changes while Home is on screen.
    PlayerSource m_homeLayoutSource = PlayerSource::None;
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
    // Radio Now Playing's equivalent toggle (see onRadioViewToggleAction) --
    // same buildCassetteVisual()/m_title/m_detail/m_cassetteReelL/R/m_cassetteNeedle
    // widgets as local, just fed station name / ICY StreamTitle instead of
    // track title / artist.
    bool m_radioCassetteView = false;
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

    // Radio Now Playing -- shares the flat-card layout/spectrum/cover-art
    // slot with Local Now Playing (see buildRadioNowPlaying(), which reuses
    // m_coverArtPixels/m_coverArtDsc/m_specCanvas etc. via loadRadioIcon()
    // instead of duplicating that machinery). Pieces genuinely unique to
    // radio:
    //   - a 9-segment classic VU/VFD bargraph instead of elapsed/total time
    //     (radio has no duration to show), driven by state.vuLevel.
    //   - a thin 2-row green dot-matrix "buffer activity" bar instead of the
    //     draggable seek slider (radio can't seek; this is the same
    //     state.bufferFillPercent the old simple radio page showed as a
    //     plain filled bar, just restyled to match the LED aesthetic).
    lv_obj_t* m_vfdSegments[9]{};
    lv_obj_t* m_radioBufCanvas = nullptr;
    lv_color_t* m_radioBufCanvasBuf = nullptr; // persists across rebuilds, same pattern as m_specCanvasBuf
    uint8_t m_radioBufLastLit = 0xFF;          // forces the first draw after a rebuild
    uint16_t m_currentRadioIconIndex = 0;      // which station's icon is currently decoded into m_coverArtPixels, 0 = none
    // L/R peak-hold readout, in the narrow strip directly below the VFD
    // ladder (see buildRadioNowPlaying) -- real per-channel data from
    // Audio::getVUlevel() (state.vuLevel = left, state.vuRight = right),
    // not a synthesized stereo effect. Still no true dBFS calibration
    // though: the dB figure is an approximation (20*log10(raw/255)), not a
    // calibrated measurement. Classic peak-hold behavior per channel: jumps
    // up immediately on a new peak, holds, then decays.
    lv_obj_t* m_peakLLabel = nullptr;
    lv_obj_t* m_peakRLabel = nullptr;
    uint8_t m_peakHoldRawL = 0;
    uint8_t m_peakHoldRawR = 0;
    uint32_t m_peakHoldTimestampL = 0;
    uint32_t m_peakHoldTimestampR = 0;
    // "已保存的网络电台" list (buildRadioList()) shows each station's logo
    // once radioIconSyncStart() has cached it -- decoded once per row when
    // the list is (re)built (a local SD JPEG read+decode, not a network
    // call at this point) and kept alive as long as the list is on screen.
    // Capped at 30 rows' worth of decoded icons; stations beyond that still
    // list fine, just without a logo, same graceful-degradation rule as a
    // logo radio-browser couldn't find.
    static constexpr uint8_t kMaxRadioListIcons = 30;
    uint16_t* m_radioListIconPixels[kMaxRadioListIcons]{};

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
    lv_obj_t* m_usbStorageStatus = nullptr;
    lv_obj_t* m_usbStorageDetail = nullptr;
    lv_obj_t* m_usbStorageFormat = nullptr;
    lv_obj_t* m_usbStorageCapacity = nullptr;
    lv_obj_t* m_usbStorageHint = nullptr;
    lv_obj_t* m_usbStorageDebug = nullptr;
    lv_obj_t* m_usbStorageButton = nullptr;
    lv_obj_t* m_usbStorageButtonLabel = nullptr;
    UsbStorageState m_lastUsbStorageState = UsbStorageState::Unsupported;
    // Mount confirmation: the first tap on 挂载 arms the button, a second
    // tap within 5s actually mounts (mount now stops playback and reboots
    // into USB storage mode). Auto-expires in refreshUsbStorage().
    bool m_usbStorageConfirmArmed = false;
    uint32_t m_usbStorageConfirmArmedAt = 0;
    bool m_lastUsbStorageConfirmArmed = false;

    // Settings > 在线音乐 (Cloud Music gateway config -- phase 2 scope, see
    // buildCloudMusicSettings()/refreshCloudMusicSettings()).
    // One field at a time (see CloudMusicConfigStage) -- EditBaseUrl/
    // EditDeviceKey each create exactly one textarea here, never both at
    // once, so a single shared pointer is enough (same idea as
    // m_coverArtPixels being shared between radio/local art).
    lv_obj_t* m_cloudEditField = nullptr;
    lv_obj_t* m_cloudKeyboard = nullptr;
    lv_obj_t* m_cloudEditError = nullptr; // one-line save-failure reason under the edit field
    lv_obj_t* m_cloudStatusLabel = nullptr;
    lv_obj_t* m_cloudHintLabel = nullptr;
    // Phone-input sub-view: QR encoding http://<ip>/cloud_config -- the
    // phone opens a web form, types the gateway URL + device key there, and
    // the board picks the result up from NVS on return. Same pattern as the
    // WiFi manage QR (m_wifiShowManageQr).
    bool m_cloudShowConfigQr = false;
    lv_obj_t* m_cloudQr = nullptr;
    char m_cloudQrLastContent[128]{};
    // History sub-view: up to 5 previously saved gateway configs, newest
    // first, tap to reuse / 删除 to remove. Survives app-only reflashes.
    bool m_cloudShowHistory = false;
    CloudServiceState m_lastCloudServiceState = CloudServiceState::Unknown;
    // Deferred page navigation: show() must not run from inside a page's
    // refresh() (it deletes the widgets that refresh() is still touching ->
    // use-after-free crash). Set this during refresh and apply it at the end
    // of refresh() instead.
    Page m_pendingNavigate = Page::Home;
    bool m_pendingNavigateSet = false;
    CloudMusicConfigStage m_cloudConfigStage = CloudMusicConfigStage::Overview;
    // Phase 3 browse pages (hot playlists / search / playlist detail).
    // m_cloudListArea is reused across all three build functions' own list
    // container (only one of these pages is ever on screen at once, same
    // reasoning as m_coverArtPixels being shared between radio/local art).
    lv_obj_t* m_cloudListArea = nullptr;
    lv_obj_t* m_cloudListHint = nullptr;
    CloudMusicRequestState m_lastCloudHotState = CloudMusicRequestState::Idle;
    CloudMusicRequestState m_lastCloudRankingState = CloudMusicRequestState::Idle;
    CloudMusicRequestState m_lastCloudNewSongState = CloudMusicRequestState::Idle;
    lv_obj_t* m_cloudSearchField = nullptr;
    lv_obj_t* m_cloudSearchKeyboard = nullptr;
    CloudMusicRequestState m_lastCloudSearchState = CloudMusicRequestState::Idle;
    char m_cloudSelectedPlaylistId[24]{};
    char m_cloudSelectedPlaylistName[96]{};
    // Language-classification context (语言分类): the selected NetEase
    // playlist category tag and its display name. Cleared when the plain
    // 热门歌单 entry is opened directly.
    char m_cloudLanguageCat[16]{};
    char m_cloudLanguageName[24]{};
    // Cover of whichever playlist/ranking the current cloud track came
    // from -- used as the now-playing cover fallback when the track itself
    // has no album art (set in onCloudMusicPlaylistOpenAction /
    // onCloudRankingRowAction).
    char m_cloudCurrentPlaylistCover[200]{};
    CloudMusicRequestState m_lastCloudPlaylistState = CloudMusicRequestState::Idle;
    // Tracked separately from m_lastCloudSearchState/m_lastCloudPlaylistState
    // -- a resolve (triggered by tapping a track row) overlays its own
    // Loading/Error text on m_cloudListHint without touching the list
    // itself, see refreshCloudMusicSearch()/refreshCloudMusicPlaylist().
    CloudMusicRequestState m_lastCloudResolveState = CloudMusicRequestState::Idle;

    // Which list the cloud player's prev/next walks, set when a track row
    // is tapped (see onCloudMusicTrackRowAction) and used by
    // onCloudTransportAction -- mirrors m_currentLocalTrackIndex's role for
    // local playback, except the cloud "queue" is just the page the user
    // is browsing (search results / playlist detail / new-song arrivals),
    // since the core keeps those arrays alive while their page is open.
    enum class CloudQueueSource : uint8_t { None, Search, Playlist, NewSongs };
    CloudQueueSource m_cloudQueueSource = CloudQueueSource::None;
    uint8_t m_cloudQueueIndex = 0;
    // Edge-detect "the background cover-thumbnail sync just finished" so
    // the hot-playlists/rankings pages rebuild their rows one extra time
    // (rows built before a thumb arrived otherwise keep their placeholder).
    bool m_cloudThumbSyncing = false;
    // Decoded cover thumbnails for the hot-playlist / ranking list rows --
    // owned heap allocations, freed in show() alongside m_radioListIconPixels.
    static constexpr uint8_t kCloudRowThumbMax = 12;
    uint16_t* m_cloudRowThumbPixels[kCloudRowThumbMax]{};
    // Which cloud track's cover is (or is being) loaded into the shared
    // m_coverArtPixels slot, and whether it's actually decoded yet -- the
    // now-playing page starts a background fetch when a new track starts
    // and rebuilds itself once the cover lands (see loadCloudCover()).
    char m_cloudCoverTrackId[24]{};
    bool m_cloudCoverReady = false;
    uint32_t m_lastCloudCoverRetry = 0;
    // Which cloud track's lyrics were last requested (mirrors
    // m_cloudCoverTrackId -- used to trigger the async fetch once per
    // track change).
    char m_cloudLyricsTrackId[24]{};

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
    AudioToneSettings m_audioTone{};
    lv_obj_t* m_audioEqSliders[4] = {nullptr, nullptr, nullptr, nullptr}; // low, mid, high, balance
    lv_obj_t* m_audioEqValueLabels[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* m_audioEqPresetButtons[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    uint8_t m_audioEqBandIndex = 0;
    uint32_t m_audioToneLastApplyMs = 0;
    uint32_t m_audioToneSaveDueMs = 0;
    bool m_audioTonePendingApply = false;
    bool m_audioEqRefreshing = false;
};
