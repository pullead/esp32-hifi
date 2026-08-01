#include "player_service.h"

#include <algorithm>
#include <cstring>

extern bool playerCorePlayRadioUrl(const char* url);
extern bool playerCorePlaySdFile(const char* path, uint32_t positionSeconds);
extern void playerCoreStop();
extern bool playerCoreTogglePause();
extern void playerCoreSetVolume(uint8_t volume);
extern void playerCoreSetMuted(bool muted);
extern void playerCoreSetTone(int8_t low, int8_t mid, int8_t high);
extern AudioToneSettings playerCoreToneSettings();
extern void playerCoreSetToneSettings(const AudioToneSettings& settings, bool persist);
extern void playerCoreSaveSettings();
extern AudioOutputPolicy playerCoreOutputPolicy();
extern void playerCoreSetOutputPolicy(AudioOutputPolicy policy, bool persist);
extern uint8_t playerCoreWifiSavedCount();
extern bool playerCoreWifiSavedInfo(uint8_t index, char* outSsid, size_t ssidSize, bool* outIsDefault);
extern void playerCoreWifiReconnect();
extern void playerCoreWifiAddNetwork(const char* ssid, const char* password);
extern void playerCoreWifiScanStart();
extern bool playerCoreWifiScanInProgress();
extern uint8_t playerCoreWifiScanResults(WifiScanItem* items, uint8_t maxItems);
extern void playerCoreNextStation();
extern void playerCorePreviousStation();
extern void playerCoreReadSnapshot(PlayerSnapshot* snapshot);
extern uint16_t playerCoreRadioStationCount();
extern bool playerCoreRadioStation(uint16_t index, RadioStationItem* item);
extern bool playerCorePlayRadioStation(uint16_t index);
extern bool playerCoreLocalLibraryScanning();
extern uint16_t playerCoreLocalLibraryCount();
extern bool playerCoreLocalTrack(uint16_t index, LocalTrackItem* item);
extern bool playerCoreDecodeLocalTrackCover(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight);
extern UsbStorageState playerCoreUsbStorageState();
extern bool playerCoreUsbStorageStats(UsbStorageStats* out);
extern bool playerCoreUsbStorageMount();
extern bool playerCoreUsbStorageUnmount();
extern void playerCoreRadioIconSyncStart();
extern bool playerCoreRadioIconSyncInProgress();
extern bool playerCoreDecodeRadioIcon(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight);
extern bool playerCoreSeekTo(uint32_t positionSeconds);
extern bool playerCoreLoadLyrics(uint16_t index);
extern const char* playerCoreCurrentLyricLine(uint32_t positionMs);
extern bool playerCoreLyricsOnlineReady(uint16_t* outIndex);
extern uint8_t playerCoreLyricsFetchState(uint16_t index);
extern void playerCoreRetryLyricsFetch(uint16_t index);

PlayerService playerService;

namespace {
void copyText(char* destination, size_t destinationSize, const char* source) {
    if (!source) source = "";
    strlcpy(destination, source, destinationSize);
}
} // namespace

void PlayerService::begin() { tick(); }

void PlayerService::tick() { playerCoreReadSnapshot(&m_snapshot); }

PlayerSnapshot PlayerService::snapshot() const { return m_snapshot; }

bool PlayerService::playRadioUrl(const char* url) {
    // Radio and local playback share one real decoder (see PlayerSnapshot::
    // source) -- there's no per-source background-pause state, so actually
    // starting a radio stream while a local track was still playing must
    // stop it first, or the "wrong" source keeps running underneath whatever
    // page is on screen. Only triggered here, at the actual moment playback
    // starts -- just opening/looking at the Radio page must NOT stop
    // anything the user hasn't asked to replace yet.
    if (m_snapshot.source == PlayerSource::Sd) stop();
    const bool started = playerCorePlayRadioUrl(url);
    if (started) {
        m_snapshot.source = PlayerSource::Radio;
        m_snapshot.transport = PlayerTransport::Buffering;
        copyText(m_snapshot.detail, sizeof(m_snapshot.detail), url);
        m_snapshot.error[0] = '\0';
    }
    return started;
}

bool PlayerService::playSdFile(const char* path, uint32_t positionSeconds) {
    // See playRadioUrl()'s comment -- same single-decoder reasoning, mirror
    // image: starting local playback stops a radio stream still running.
    if (m_snapshot.source == PlayerSource::Radio) stop();
    const bool started = playerCorePlaySdFile(path, positionSeconds);
    if (started) {
        m_snapshot.source = PlayerSource::Sd;
        m_snapshot.transport = PlayerTransport::Buffering;
        copyText(m_snapshot.detail, sizeof(m_snapshot.detail), path);
        m_snapshot.error[0] = '\0';
    }
    return started;
}

void PlayerService::stop() {
    playerCoreStop();
    m_snapshot.transport = PlayerTransport::Stopped;
}

bool PlayerService::togglePause() {
    const bool paused = playerCoreTogglePause();
    m_snapshot.transport = paused ? PlayerTransport::Paused : PlayerTransport::Playing;
    return paused;
}

void PlayerService::setVolume(uint8_t volume) { playerCoreSetVolume(volume); }

void PlayerService::setMuted(bool muted) { playerCoreSetMuted(muted); }

void PlayerService::setTone(int8_t low, int8_t mid, int8_t high) { playerCoreSetTone(low, mid, high); }

AudioToneSettings PlayerService::toneSettings() const { return playerCoreToneSettings(); }

void PlayerService::setToneSettings(const AudioToneSettings& settings, bool persist) {
    playerCoreSetToneSettings(settings, persist);
}

void PlayerService::saveToneSettings() { playerCoreSaveSettings(); }

AudioOutputPolicy PlayerService::outputPolicy() const { return playerCoreOutputPolicy(); }

void PlayerService::setOutputPolicy(AudioOutputPolicy policy, bool persist) {
    playerCoreSetOutputPolicy(policy, persist);
}

uint8_t PlayerService::wifiSavedCount() const { return playerCoreWifiSavedCount(); }

bool PlayerService::wifiSavedInfo(uint8_t index, char* outSsid, size_t ssidSize, bool* outIsDefault) const {
    return playerCoreWifiSavedInfo(index, outSsid, ssidSize, outIsDefault);
}

void PlayerService::wifiReconnect() { playerCoreWifiReconnect(); }

void PlayerService::wifiAddNetwork(const char* ssid, const char* password) { playerCoreWifiAddNetwork(ssid, password); }

void PlayerService::wifiScanStart() { playerCoreWifiScanStart(); }
bool PlayerService::wifiScanInProgress() const { return playerCoreWifiScanInProgress(); }
uint8_t PlayerService::wifiScanResults(WifiScanItem* items, uint8_t maxItems) const { return playerCoreWifiScanResults(items, maxItems); }

void PlayerService::nextStation() {
    playerCoreNextStation();
    m_snapshot.source = PlayerSource::Radio;
    m_snapshot.transport = PlayerTransport::Buffering;
}

void PlayerService::previousStation() {
    playerCorePreviousStation();
    m_snapshot.source = PlayerSource::Radio;
    m_snapshot.transport = PlayerTransport::Buffering;
}

uint16_t PlayerService::radioStationCount() const { return playerCoreRadioStationCount(); }

bool PlayerService::radioStation(uint16_t index, RadioStationItem* item) const { return playerCoreRadioStation(index, item); }

bool PlayerService::playRadioStation(uint16_t index) {
    // See playRadioUrl()'s comment.
    if (m_snapshot.source == PlayerSource::Sd) stop();
    const bool started = playerCorePlayRadioStation(index);
    if (started) {
        m_snapshot.source = PlayerSource::Radio;
        m_snapshot.transport = PlayerTransport::Buffering;
        m_snapshot.error[0] = '\0';
    }
    return started;
}

void PlayerService::radioIconSyncStart() { playerCoreRadioIconSyncStart(); }
bool PlayerService::radioIconSyncInProgress() const { return playerCoreRadioIconSyncInProgress(); }
bool PlayerService::decodeRadioIcon(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) const {
    return playerCoreDecodeRadioIcon(index, scaleFactor, outPixels, outWidth, outHeight);
}

bool PlayerService::localLibraryScanning() const { return playerCoreLocalLibraryScanning(); }

uint16_t PlayerService::localLibraryCount() const { return playerCoreLocalLibraryCount(); }

bool PlayerService::localTrack(uint16_t index, LocalTrackItem* item) const { return playerCoreLocalTrack(index, item); }

UsbStorageState PlayerService::usbStorageState() const { return playerCoreUsbStorageState(); }

bool PlayerService::usbStorageStats(UsbStorageStats* out) const { return playerCoreUsbStorageStats(out); }

bool PlayerService::usbStorageMount() { return playerCoreUsbStorageMount(); }

bool PlayerService::usbStorageUnmount() { return playerCoreUsbStorageUnmount(); }

bool PlayerService::playLocalTrack(uint16_t index) {
    LocalTrackItem item;
    if (!playerCoreLocalTrack(index, &item)) return false;
    const bool started = playSdFile(item.path);
    if (started) {
        // playSdFile() above sets `detail` to the raw path -- overwrite
        // with the real ID3 title/artist/album immediately rather than
        // waiting on live playback metadata, which for local FS files
        // (unlike radio ICY streams) never actually reaches onMetadata().
        copyText(m_snapshot.title, sizeof(m_snapshot.title), item.title[0] ? item.title : "未知曲目");
        char detail[96];
        if (item.artist[0] && item.album[0]) snprintf(detail, sizeof(detail), "%s · %s", item.artist, item.album);
        else if (item.artist[0]) strlcpy(detail, item.artist, sizeof(detail));
        else if (item.album[0]) strlcpy(detail, item.album, sizeof(detail));
        else detail[0] = '\0';
        copyText(m_snapshot.detail, sizeof(m_snapshot.detail), detail[0] ? detail : "未知艺术家");
    }
    return started;
}

bool PlayerService::seekTo(uint32_t positionSeconds) { return playerCoreSeekTo(positionSeconds); }

bool PlayerService::loadLyrics(uint16_t index) { return playerCoreLoadLyrics(index); }

const char* PlayerService::currentLyricLine(uint32_t positionMs) const { return playerCoreCurrentLyricLine(positionMs); }

bool PlayerService::lyricsOnlineReady(uint16_t* outIndex) const { return playerCoreLyricsOnlineReady(outIndex); }

LyricFetchState PlayerService::lyricsFetchState(uint16_t index) const {
    return static_cast<LyricFetchState>(playerCoreLyricsFetchState(index));
}

void PlayerService::retryLyricsFetch(uint16_t index) { playerCoreRetryLyricsFetch(index); }

bool PlayerService::decodeLocalTrackCover(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) const {
    return playerCoreDecodeLocalTrackCover(index, scaleFactor, outPixels, outWidth, outHeight);
}

void PlayerService::onMetadata(const char* station, const char* title) {
    if (station && station[0]) copyText(m_snapshot.title, sizeof(m_snapshot.title), station);
    if (title && title[0]) copyText(m_snapshot.detail, sizeof(m_snapshot.detail), title);
    if (m_snapshot.transport == PlayerTransport::Buffering) m_snapshot.transport = PlayerTransport::Playing;
}

void PlayerService::onError(const char* error) {
    copyText(m_snapshot.error, sizeof(m_snapshot.error), error);
    m_snapshot.transport = PlayerTransport::Error;
}

void PlayerService::onEndOfFile() { m_snapshot.transport = PlayerTransport::Stopped; }
