#pragma once

#include <Arduino.h>

// The UI only sees this value object.  It never calls the decoder directly.
enum class PlayerSource : uint8_t { None, Radio, Sd, Dlna };
enum class PlayerTransport : uint8_t { Stopped, Buffering, Playing, Paused, Error };
// Online-lyrics-lookup lifecycle for whichever local track is loaded --
// defined here (not main.cpp) since both sides need it: main.cpp's
// lyricsFetchTask/playerCoreLoadLyrics() set it, hifi_ui.cpp reads it via
// PlayerService::lyricsFetchState() to show "查询中"/"点击重试" instead of a
// blanket "没有歌词信息".
enum class LyricFetchState : uint8_t { Idle, Pending, Found, NotFound };
enum class UsbStorageState : uint8_t { Idle, Mounting, Mounted, Restoring, Scanning, Error, Unsupported };

// Render Music Gateway connection lifecycle (see docs spec section 8.1).
// Unknown: no health check attempted yet this boot / since config changed.
// Waking: first (or a retry) health check is in flight or the free-tier
// service looked asleep -- see cloudMusicWakeStart()'s comment for the
// retry/backoff shape. Ready: last health check succeeded. Offline: gave up
// after ~70s of retries, or the config is missing/invalid.
enum class CloudServiceState : uint8_t { Unknown, Waking, Ready, Offline };

// Settings > 在线音乐 config -- gateway base URL (no trailing slash, e.g.
// "https://esp32-ncm-gateway.onrender.com") and the X-Device-Key it expects.
// Neither value is ever a NetEase account credential (see the gateway's own
// README) -- this is only this device's own key to the gateway, analogous
// to a saved WiFi password, and is stored the same way (NVS via
// Preferences, see main.cpp's lockPreferences()).
struct CloudMusicConfig {
    char baseUrl[128]{};
    char deviceKey[80]{};
    bool configured = false; // true once both fields have been saved at least once
};

// One track as returned by the gateway's /esp/v1/search, /esp/v1/playlists/*
// endpoints (see services/ncm-gateway/src/ncmProvider.js's buildSearchItem).
// `id` is a NetEase track id, kept as text (it's only ever round-tripped
// back to the gateway in a resolve/lyrics URL, never arithmetic on it).
struct CloudTrackItem {
    char id[24]{};
    char title[80]{};
    char artist[64]{};
    char album[64]{};
    uint32_t durationMs = 0;
    char coverUrl[200]{};
    bool playableHint = true;
};

struct CloudPlaylistItem {
    char id[24]{};
    char name[96]{};
    char creator[48]{};
    char coverUrl[200]{};
    uint16_t trackCount = 0;
};

// Generic request lifecycle for the three phase-3 background lookups
// (search / hot playlists / playlist detail) -- same shape as
// LyricFetchState, one instance per lookup kind rather than a single shared
// one, since e.g. opening a playlist while a search is still loading must
// not clobber the search's own state.
enum class CloudMusicRequestState : uint8_t { Idle, Loading, Loaded, Error };

struct UsbStorageStats {
    uint32_t readCount = 0;
    uint32_t writeCount = 0;
    uint32_t readFailCount = 0;
    uint32_t writeFailCount = 0;
    uint32_t lastLba = 0;
    uint32_t minLba = 0;
    uint32_t maxLba = 0;
    uint32_t lastOffset = 0;
    uint32_t lastSize = 0;
    uint32_t maxSize = 0;
    int32_t lastResult = 0;
};

struct UsbStorageFormatInfo {
    bool valid = false;
    bool fat32 = false;
    bool recommendedAllocation = false;
    uint16_t bytesPerSector = 0;
    uint8_t sectorsPerCluster = 0;
    uint32_t allocationUnitBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint32_t partitionStartLba = 0;
};

struct PlayerSnapshot {
    PlayerSource source = PlayerSource::None;
    PlayerTransport transport = PlayerTransport::Stopped;
    bool wifiConnected = false;
    bool muted = false;
    uint8_t volume = 0;
    uint8_t volumeSteps = 21;
    uint8_t vuLevel = 0; // left-channel peak, 0-255 (see Audio::getVUlevel() -- low byte is left)
    uint8_t vuRight = 0; // right-channel peak, 0-255 (getVUlevel()'s high byte; unused before this was split out)
    uint8_t spectrumBands[6]{}; // real bass..treble FFT levels 0-255, see Audio::getSpectrumBands()
    uint16_t radioStationIndex = 0; // 1-based, matches radioStation()/RadioStationItem::index; 0 when not on radio
    uint32_t positionSeconds = 0;
    uint32_t durationSeconds = 0;
    char title[96]{};
    char detail[96]{};
    char error[96]{};
    char codec[16]{};
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 0;
    uint32_t bitRate = 0;
    uint8_t bufferFillPercent = 0; // real input-buffer occupancy, radio "progress" bar
    char timeHM[6]{};    // "HH:MM", empty until RTC/NTP has synced
    char dateStr[20]{};  // "周二 08 Jul", empty until RTC/NTP has synced
    int8_t wifiRssi = 0; // dBm, 0 when disconnected
    int16_t weatherTempC = -999; // Open-Meteo current temp, -999 sentinel until first fetch succeeds
    char weatherDesc[8]{};       // short condition label ("晴"/"多云"/...), empty until fetched
    int8_t weatherIcon = -1;     // 0 clear,1 partly cloudy,2 cloudy,3 fog,4 rain,5 snow,6 thunder, -1 unknown
    char wifiSsid[33]{};         // connected network name, empty if not connected
    char wifiIp[16]{};           // dotted-quad, empty if not connected
    bool wifiApFallbackActive = false; // no known network reachable -- device is broadcasting its own setup hotspot
    char wifiApSsid[33]{};       // the fallback hotspot's own SSID (see wifiApFallbackActive)
    char wifiApIp[16]{};         // the fallback hotspot's own IP (normally 192.168.4.1)
    // Increments once per Audio::evt_eof (radio disconnect OR local file
    // reaching its end -- the event doesn't distinguish which). The UI
    // watches for this changing while it knows a local track was playing to
    // drive auto-advance (see HifiUi's play-mode handling); a plain bool
    // "just ended" flag would miss back-to-back ends within one refresh tick.
    uint32_t eofCount = 0;
};

// One entry from an on-demand WiFi scan (see PlayerService::wifiScan()) --
// triggered only when the user opens the manual add-network screen, never
// automatically at boot (see wifiSavedCount()'s comment on why scanning was
// moved off the always-on boot path).
struct WifiScanItem {
    char ssid[33]{};
    int8_t rssi = 0;
};

struct RadioStationItem {
    uint16_t index = 0;
    bool favorite = false;
    char country[8]{};
    char name[80]{};
    char url[160]{};
};

// One entry in the scanned SD music library. title/artist/album come from
// ID3v2 (MP3 only, see scanLocalMusicLibrary() in main.cpp) -- files without
// readable tags fall back to their filename as title, artist/album empty.
struct LocalTrackItem {
    char path[160]{};
    char title[64]{};
    char artist[48]{};
    char album[48]{};
    bool hasArt = false;
};

struct AudioToneSettings {
    int8_t low = 0;
    int8_t mid = 0;
    int8_t high = 0;
    int8_t balance = 0;
};

enum class AudioOutputPolicy : uint8_t { Source = 0, Fixed44100 = 1, Fixed48000 = 2 };

// Small, stable API used by LVGL and deliberately independent from its widgets.
class PlayerService {
  public:
    void begin();
    void tick();
    PlayerSnapshot snapshot() const;

    bool playRadioUrl(const char* url);
    bool playSdFile(const char* path, uint32_t positionSeconds = 0);
    void stop();
    bool togglePause();
    void setVolume(uint8_t volume);
    void setMuted(bool muted);
    // 3-band EQ, -12..+12 dB each (clamped by Audio::setTone() itself).
    void setTone(int8_t low, int8_t mid, int8_t high);
    AudioToneSettings toneSettings() const;
    void setToneSettings(const AudioToneSettings& settings, bool persist);
    void saveToneSettings();
    AudioOutputPolicy outputPolicy() const;
    void setOutputPolicy(AudioOutputPolicy policy, bool persist);

    // Settings > WiFi screen: saved-network list only (from NVS) -- no
    // on-device scanning, see hifi_ui.cpp's buildSettingsWifi() comment.
    // Discovering/adding new networks is a phone-only job via the
    // /wifi_manage web page (main.cpp), reached from the device via a QR.
    uint8_t wifiSavedCount() const;
    bool wifiSavedInfo(uint8_t index, char* outSsid, size_t ssidSize, bool* outIsDefault) const;
    // Nudges wifiMulti to retry its saved APs right now (tapping a saved
    // row) instead of waiting for the next opportunistic reconnect tick.
    void wifiReconnect();
    // On-screen "manually add a network" entry (see HifiUi's WiFi settings
    // add-network sub-view) -- persists to the next free NVS slot and asks
    // wifiMulti to try it immediately. No scanning involved: the user types
    // the SSID/password themselves, same as the phone-only /wifi_manage
    // page's "connect" action already did.
    void wifiAddNetwork(const char* ssid, const char* password);
    // Asynchronous on-demand scan -- only triggered when the user opens the
    // manual add-network screen. wifiScanStart() returns immediately (the
    // actual WiFi.scanNetworks() runs on its own task); poll
    // wifiScanInProgress() and once it's false, wifiScanResults() has the
    // de-duplicated (by SSID, strongest RSSI kept), RSSI-descending list.
    void wifiScanStart();
    bool wifiScanInProgress() const;
    uint8_t wifiScanResults(WifiScanItem* items, uint8_t maxItems) const;
    void nextStation();
    void previousStation();
    uint16_t currentRadioStationIndex() const;
    bool playCurrentRadioStation();
    uint16_t radioStationCount() const;
    bool radioStation(uint16_t index, RadioStationItem* item) const;
    bool playRadioStation(uint16_t index);

    // Station logos: fetched from radio-browser.info (name search -> its
    // favicon field), cached to SD as /logo/radio_<index>.jpg, decoded via
    // the same tftLib JPEG decoder local cover art already uses -- see
    // decodeLocalTrackCover()'s comment on ownership (caller frees
    // *outPixels with free()). Non-JPEG favicons (PNG/ICO are common on
    // radio-browser) fail to decode and are treated the same as "no logo
    // available" -- falls back to the generic RADIO glyph, never blocks.
    //
    // radioIconSyncStart() is asynchronous and only triggered by opening
    // the saved-stations list (see onWifiAddOpenAction's comment on why a
    // synchronous version of this kind of thing once froze the device):
    // it walks every station missing a cached logo, on its own task, and
    // is a no-op if a sync is already running.
    void radioIconSyncStart();
    bool radioIconSyncInProgress() const;
    bool decodeRadioIcon(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) const;

    // SD music library: background-scanned (see scanLocalMusicLibrary() in
    // main.cpp), so localLibraryScanning() stays true and the count grows
    // while it runs -- the UI should show that rather than an empty list.
    bool localLibraryScanning() const;
    uint16_t localLibraryCount() const;
    bool localTrack(uint16_t index, LocalTrackItem* item) const;
    const char* lastLocalFilePath() const;
    uint32_t lastLocalFilePosition() const;
    bool playLocalTrack(uint16_t index, uint32_t positionSeconds = 0);
    UsbStorageState usbStorageState() const;
    bool usbStorageStats(UsbStorageStats* out) const;
    bool usbStorageFormatInfo(UsbStorageFormatInfo* out) const;
    bool usbStorageMount();
    bool usbStorageUnmount();
    bool seekTo(uint32_t positionSeconds);
    // Synchronized lyrics (ID3 SYLT, milliseconds format only). Loaded
    // lazily per track like cover art; currentLyricLine() returns "" if
    // none are loaded or playback hasn't reached the first line yet.
    bool loadLyrics(uint16_t index);
    const char* currentLyricLine(uint32_t positionMs) const;
    // Consume-once: true once per completed background online-lyrics fetch
    // (see main.cpp's lyricsFetchTask), whichever track it was for. The UI
    // uses this to know when to re-check loadLyrics()'s cache for whatever
    // track it's currently displaying.
    bool lyricsOnlineReady(uint16_t* outIndex) const;
    // Idle/Pending/Found/NotFound for whichever track loadLyrics() most
    // recently loaded (Idle if index isn't that track). Lets the UI show
    // "查询中"/"未找到，点击重试" instead of a blanket "没有歌词信息".
    LyricFetchState lyricsFetchState(uint16_t index) const;
    // Re-queues an online lookup for a track that came back NotFound (e.g.
    // no WiFi the first time, or the user wants to retry after fixing tags).
    // No-op if playback has moved on, or there's still no network.
    void retryLyricsFetch(uint16_t index);
    // Decodes a track's embedded cover art (if hasArt) into a small PSRAM
    // RGB565 buffer. Caller owns *outPixels and must free() it. scaleFactor
    // is tjpgd's downscale divisor: 1/2/4/8.
    bool decodeLocalTrackCover(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) const;

    // Render Music Gateway (在线音乐/网易云) -- phase 2 scope: config storage
    // and connection/wake testing only, no search/playback yet (see
    // src/cloud_music.h). baseUrl/deviceKey are trimmed and persisted to NVS
    // on save; an empty baseUrl or deviceKey is rejected (returns false)
    // rather than silently saving an unusable config.
    CloudMusicConfig cloudMusicConfig() const;
    bool setCloudMusicConfig(const char* baseUrl, const char* deviceKey);
    CloudServiceState cloudServiceState() const;
    // Starts (or restarts, if already Waking/Offline) the health-check/wake
    // sequence -- see cloudMusicWakeTask()'s comment in main.cpp for the
    // retry schedule. No-op if cloudMusicConfig().configured is false.
    void cloudMusicWakeStart();

    // Phase 3: browse-only (search/hot playlists/playlist detail), no
    // playback wiring yet. Each *Start() enqueues a command for the single
    // background controller task (main.cpp's cloudMusicControllerTask, see
    // its own comment for the command/generation/cancellation scheme) and
    // returns immediately; poll the matching *State() from the LVGL tick.
    bool cloudMusicSearchStart(const char* query);
    CloudMusicRequestState cloudMusicSearchState() const;
    uint8_t cloudMusicSearchResultCount() const;
    bool cloudMusicSearchResult(uint8_t index, CloudTrackItem* item) const;
    bool cloudMusicSearchHasMore() const;
    const char* cloudMusicLastError() const; // shared across search/hot/playlist -- only one of them is ever Error at a time in this phase's single-request-at-a-time UI

    void cloudMusicHotPlaylistsStart();
    CloudMusicRequestState cloudMusicHotPlaylistsState() const;
    uint8_t cloudMusicHotPlaylistCount() const;
    bool cloudMusicHotPlaylist(uint8_t index, CloudPlaylistItem* item) const;

    bool cloudMusicPlaylistDetailStart(const char* playlistId);
    CloudMusicRequestState cloudMusicPlaylistDetailState() const;
    CloudPlaylistItem cloudMusicPlaylistDetailInfo() const;
    uint8_t cloudMusicPlaylistTrackCount() const;
    bool cloudMusicPlaylistTrack(uint8_t index, CloudTrackItem* item) const;

    // Called by the MiniWebRadio audio callback, never from an LVGL handler.
    void onMetadata(const char* station, const char* title);
    void onError(const char* error);
    void onEndOfFile();

  private:
    PlayerSnapshot m_snapshot{};
};

extern PlayerService playerService;
