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
extern uint16_t playerCoreCurrentRadioStationNumber();
extern uint16_t playerCoreRadioStationCount();
extern bool playerCoreRadioStation(uint16_t index, RadioStationItem* item);
extern bool playerCorePlayRadioStation(uint16_t index);
extern bool playerCoreLocalLibraryScanning();
extern uint16_t playerCoreLocalLibraryCount();
extern bool playerCoreLocalTrack(uint16_t index, LocalTrackItem* item);
extern bool playerCoreTrackFavorite(uint16_t index);
extern uint32_t playerCoreTrackImportedAt(uint16_t index);
extern void playerCoreSetTrackFavorite(uint16_t index, bool on);
extern bool playerCoreDecodeLocalTrackCover(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight);
extern UsbStorageState playerCoreUsbStorageState();
extern bool playerCoreUsbStorageStats(UsbStorageStats* out);
extern bool playerCoreUsbStorageFormatInfo(UsbStorageFormatInfo* out);
extern bool playerCoreUsbStorageMount();
extern bool playerCoreUsbStorageUnmount();
extern void playerCoreRadioIconSyncStart();
extern bool playerCoreRadioIconSyncInProgress();
extern bool playerCoreDecodeRadioIcon(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight);
extern bool playerCoreSeekTo(uint32_t positionSeconds);
extern const char* playerCoreLastLocalFilePath();
extern uint32_t playerCoreLastLocalFilePosition();
extern bool playerCoreLoadLyrics(uint16_t index);
extern const char* playerCoreCurrentLyricLine(uint32_t positionMs);
extern bool playerCoreLyricsOnlineReady(uint16_t* outIndex);
extern uint8_t playerCoreLyricsFetchState(uint16_t index);
extern void playerCoreRetryLyricsFetch(uint16_t index);
extern CloudMusicConfig playerCoreCloudMusicConfig();
extern bool playerCoreSetCloudMusicConfig(const char* baseUrl, const char* deviceKey);
extern uint8_t playerCoreCloudMusicHistoryCount();
extern bool playerCoreCloudMusicHistoryEntry(uint8_t index, CloudMusicHistoryEntry* entry);
extern bool playerCoreCloudMusicHistoryDelete(uint8_t index);
extern uint8_t playerCoreCloudServiceState();
extern void playerCoreCloudMusicWakeStart();
extern bool playerCoreCloudMusicSearchStart(const char* query);
extern uint8_t playerCoreCloudMusicSearchState();
extern uint8_t playerCoreCloudMusicSearchResultCount();
extern bool playerCoreCloudMusicSearchResult(uint8_t index, CloudTrackItem* item);
extern bool playerCoreCloudMusicSearchHasMore();
extern const char* playerCoreCloudMusicLastError();
extern void playerCoreCloudMusicHotPlaylistsStart(const char* cat);
extern uint8_t playerCoreCloudMusicHotPlaylistsState();
extern uint8_t playerCoreCloudMusicHotPlaylistCount();
extern bool playerCoreCloudMusicHotPlaylist(uint8_t index, CloudPlaylistItem* item);
extern bool playerCoreCloudMusicPlaylistDetailStart(const char* playlistId);
extern uint8_t playerCoreCloudMusicPlaylistDetailState();
extern CloudPlaylistItem playerCoreCloudMusicPlaylistDetailInfo();
extern uint8_t playerCoreCloudMusicPlaylistTrackCount();
extern bool playerCoreCloudMusicPlaylistTrack(uint8_t index, CloudTrackItem* item);
extern void playerCoreCloudMusicRankingsStart();
extern uint8_t playerCoreCloudMusicRankingsState();
extern uint8_t playerCoreCloudMusicRankingCount();
extern bool playerCoreCloudMusicRanking(uint8_t index, CloudRankingItem* item);
extern void playerCoreCloudMusicNewSongsStart();
extern uint8_t playerCoreCloudMusicNewSongsState();
extern uint8_t playerCoreCloudMusicNewSongCount();
extern bool playerCoreCloudMusicNewSong(uint8_t index, CloudTrackItem* item);
extern void playerCoreCloudThumbSyncStart();
extern bool playerCoreCloudThumbSyncInProgress();
extern bool playerCoreCloudThumbDecode(uint8_t kind, uint8_t index, uint8_t scaleFactor, uint16_t** outPixels,
                                       uint16_t* outWidth, uint16_t* outHeight);
extern void playerCoreCloudNowPlayingCoverStart(const char* fallbackUrl);
extern bool playerCoreCloudNowPlayingCoverDecode(uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth,
                                                 uint16_t* outHeight);
extern bool playerCoreCloudMusicNowPlayingTrack(CloudTrackItem* item);
extern void playerCoreCloudMusicLyricsStart(const char* trackId);
extern uint8_t playerCoreCloudMusicLyricsState();
extern bool playerCoreCloudMusicLyricsForTrack(const char* trackId);
extern const char* playerCoreCloudMusicCurrentLyricLine(uint32_t positionMs);
extern bool playerCoreCloudMusicPlayTrackStart(const char* trackId);
extern uint8_t playerCoreCloudMusicResolveState();
extern bool playerCoreCloudMusicJustStarted();
extern uint8_t playerCoreLastSource();
extern void playerCoreSetLastSource(uint8_t source);
extern bool playerCoreCloudMusicConsumeNowPlaying(CloudTrackItem* outTrack);

PlayerService playerService;

namespace {
void copyText(char* destination, size_t destinationSize, const char* source) {
    if (!source) source = "";
    strlcpy(destination, source, destinationSize);
}
} // namespace

void PlayerService::begin() { tick(); }

void PlayerService::tick() {
    playerCoreReadSnapshot(&m_snapshot);
    // Cloud Music phase 4: a resolve that just succeeded already told
    // main.cpp to connecttohost() the CDN URL directly (see
    // cloudMusicControllerTask) -- playerCoreReadSnapshot() above already
    // picks up source=CloudMusic from that (via s_cloudMusicPlaying). All
    // that's left is the metadata a plain CDN file URL never sends as ICY
    // tags: title/artist/duration came from the gateway's resolve response
    // instead, applied here once per completed resolve.
    CloudTrackItem nowPlaying{};
    if (playerCoreCloudMusicConsumeNowPlaying(&nowPlaying)) {
        copyText(m_snapshot.title, sizeof(m_snapshot.title), nowPlaying.title);
        copyText(m_snapshot.detail, sizeof(m_snapshot.detail), nowPlaying.artist);
        m_snapshot.durationSeconds = nowPlaying.durationMs / 1000;
        m_snapshot.error[0] = '\0';
    }

    // 记下最后一次真正播放过的音源，供重启后首页"正在播放"决定跳到哪一页。
    // 放在这里而不是各个 play*() 里：playerCoreReadSnapshot() 是所有音源的统一
    // 入口——云音乐的 source 是从 main.cpp 的 s_cloudMusicPlaying 推出来的，
    // 根本不经过本类的任何 play*() 方法，只有这里能全覆盖。
    // playerCoreSetLastSource() 内部自带"没变就不写"的判断，不会磨损 flash。
    if (m_snapshot.source != PlayerSource::None) {
        playerCoreSetLastSource(static_cast<uint8_t>(m_snapshot.source));
    }
}

PlayerSnapshot PlayerService::snapshot() const { return m_snapshot; }

bool PlayerService::playRadioUrl(const char* url) {
    // Radio and local playback share one real decoder (see PlayerSnapshot::
    // source) -- there's no per-source background-pause state, so actually
    // starting a radio stream while a local track was still playing must
    // stop it first, or the "wrong" source keeps running underneath whatever
    // page is on screen. Only triggered here, at the actual moment playback
    // starts -- just opening/looking at the Radio page must NOT stop
    // anything the user hasn't asked to replace yet.
    if (m_snapshot.source == PlayerSource::Sd || m_snapshot.source == PlayerSource::CloudMusic) stop();
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
    if (m_snapshot.source == PlayerSource::Radio || m_snapshot.source == PlayerSource::CloudMusic) stop();
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
    if (m_snapshot.source == PlayerSource::Sd) stop();
    playerCoreNextStation();
    m_snapshot.source = PlayerSource::Radio;
    m_snapshot.transport = PlayerTransport::Buffering;
}

void PlayerService::previousStation() {
    if (m_snapshot.source == PlayerSource::Sd) stop();
    playerCorePreviousStation();
    m_snapshot.source = PlayerSource::Radio;
    m_snapshot.transport = PlayerTransport::Buffering;
}

uint16_t PlayerService::radioStationCount() const { return playerCoreRadioStationCount(); }

bool PlayerService::radioStation(uint16_t index, RadioStationItem* item) const { return playerCoreRadioStation(index, item); }

uint16_t PlayerService::currentRadioStationIndex() const { return playerCoreCurrentRadioStationNumber(); }

bool PlayerService::playCurrentRadioStation() {
    uint16_t index = currentRadioStationIndex();
    if (!index && radioStationCount()) index = 1;
    return index ? playRadioStation(index) : false;
}

bool PlayerService::playRadioStation(uint16_t index) {
    // See playRadioUrl()'s comment.
    RadioStationItem item{};
    const bool haveStationInfo = playerCoreRadioStation(index, &item);
    if (m_snapshot.source == PlayerSource::Sd) stop();
    const bool started = playerCorePlayRadioStation(index);
    if (started) {
        m_snapshot.source = PlayerSource::Radio;
        m_snapshot.transport = PlayerTransport::Buffering;
        m_snapshot.radioStationIndex = index;
        copyText(m_snapshot.title, sizeof(m_snapshot.title), haveStationInfo && item.name[0] ? item.name : "Radio");
        copyText(m_snapshot.detail, sizeof(m_snapshot.detail), haveStationInfo && item.url[0] ? item.url : "");
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
bool PlayerService::localTrackFavorite(uint16_t index) const { return playerCoreTrackFavorite(index); }
uint32_t PlayerService::localTrackImportedAt(uint16_t index) const { return playerCoreTrackImportedAt(index); }
void PlayerService::setLocalTrackFavorite(uint16_t index, bool on) { playerCoreSetTrackFavorite(index, on); }

PlayerSource PlayerService::lastSource() const { return static_cast<PlayerSource>(playerCoreLastSource()); }

const char* PlayerService::lastLocalFilePath() const { return playerCoreLastLocalFilePath(); }

uint32_t PlayerService::lastLocalFilePosition() const { return playerCoreLastLocalFilePosition(); }

UsbStorageState PlayerService::usbStorageState() const { return playerCoreUsbStorageState(); }

bool PlayerService::usbStorageStats(UsbStorageStats* out) const { return playerCoreUsbStorageStats(out); }

bool PlayerService::usbStorageFormatInfo(UsbStorageFormatInfo* out) const { return playerCoreUsbStorageFormatInfo(out); }

bool PlayerService::usbStorageMount() { return playerCoreUsbStorageMount(); }

bool PlayerService::usbStorageUnmount() { return playerCoreUsbStorageUnmount(); }

bool PlayerService::playLocalTrack(uint16_t index, uint32_t positionSeconds) {
    LocalTrackItem item;
    if (!playerCoreLocalTrack(index, &item)) return false;
    const bool started = playSdFile(item.path, positionSeconds);
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

CloudMusicConfig PlayerService::cloudMusicConfig() const { return playerCoreCloudMusicConfig(); }

bool PlayerService::setCloudMusicConfig(const char* baseUrl, const char* deviceKey) {
    return playerCoreSetCloudMusicConfig(baseUrl, deviceKey);
}

uint8_t PlayerService::cloudMusicHistoryCount() const { return playerCoreCloudMusicHistoryCount(); }

bool PlayerService::cloudMusicHistoryEntry(uint8_t index, CloudMusicHistoryEntry* entry) const {
    return playerCoreCloudMusicHistoryEntry(index, entry);
}

bool PlayerService::cloudMusicHistoryDelete(uint8_t index) { return playerCoreCloudMusicHistoryDelete(index); }

CloudServiceState PlayerService::cloudServiceState() const {
    return static_cast<CloudServiceState>(playerCoreCloudServiceState());
}

void PlayerService::cloudMusicWakeStart() { playerCoreCloudMusicWakeStart(); }

bool PlayerService::cloudMusicSearchStart(const char* query) { return playerCoreCloudMusicSearchStart(query); }

CloudMusicRequestState PlayerService::cloudMusicSearchState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicSearchState());
}

uint8_t PlayerService::cloudMusicSearchResultCount() const { return playerCoreCloudMusicSearchResultCount(); }

bool PlayerService::cloudMusicSearchResult(uint8_t index, CloudTrackItem* item) const {
    return playerCoreCloudMusicSearchResult(index, item);
}

bool PlayerService::cloudMusicSearchHasMore() const { return playerCoreCloudMusicSearchHasMore(); }

const char* PlayerService::cloudMusicLastError() const { return playerCoreCloudMusicLastError(); }

void PlayerService::cloudMusicHotPlaylistsStart(const char* cat) { playerCoreCloudMusicHotPlaylistsStart(cat); }

CloudMusicRequestState PlayerService::cloudMusicHotPlaylistsState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicHotPlaylistsState());
}

uint8_t PlayerService::cloudMusicHotPlaylistCount() const { return playerCoreCloudMusicHotPlaylistCount(); }

bool PlayerService::cloudMusicHotPlaylist(uint8_t index, CloudPlaylistItem* item) const {
    return playerCoreCloudMusicHotPlaylist(index, item);
}

bool PlayerService::cloudMusicPlaylistDetailStart(const char* playlistId) {
    return playerCoreCloudMusicPlaylistDetailStart(playlistId);
}

CloudMusicRequestState PlayerService::cloudMusicPlaylistDetailState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicPlaylistDetailState());
}

CloudPlaylistItem PlayerService::cloudMusicPlaylistDetailInfo() const { return playerCoreCloudMusicPlaylistDetailInfo(); }

uint8_t PlayerService::cloudMusicPlaylistTrackCount() const { return playerCoreCloudMusicPlaylistTrackCount(); }

bool PlayerService::cloudMusicPlaylistTrack(uint8_t index, CloudTrackItem* item) const {
    return playerCoreCloudMusicPlaylistTrack(index, item);
}

void PlayerService::cloudMusicRankingsStart() { playerCoreCloudMusicRankingsStart(); }

CloudMusicRequestState PlayerService::cloudMusicRankingsState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicRankingsState());
}

uint8_t PlayerService::cloudMusicRankingCount() const { return playerCoreCloudMusicRankingCount(); }

bool PlayerService::cloudMusicRanking(uint8_t index, CloudRankingItem* item) const {
    return playerCoreCloudMusicRanking(index, item);
}

void PlayerService::cloudMusicNewSongsStart() { playerCoreCloudMusicNewSongsStart(); }

CloudMusicRequestState PlayerService::cloudMusicNewSongsState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicNewSongsState());
}

uint8_t PlayerService::cloudMusicNewSongCount() const { return playerCoreCloudMusicNewSongCount(); }

bool PlayerService::cloudMusicNewSong(uint8_t index, CloudTrackItem* item) const {
    return playerCoreCloudMusicNewSong(index, item);
}

void PlayerService::cloudThumbSyncStart() { playerCoreCloudThumbSyncStart(); }

bool PlayerService::cloudThumbSyncInProgress() const { return playerCoreCloudThumbSyncInProgress(); }

bool PlayerService::cloudThumbDecode(uint8_t kind, uint8_t index, uint8_t scaleFactor, uint16_t** outPixels,
                                     uint16_t* outWidth, uint16_t* outHeight) const {
    return playerCoreCloudThumbDecode(kind, index, scaleFactor, outPixels, outWidth, outHeight);
}

void PlayerService::cloudNowPlayingCoverStart(const char* fallbackUrl) { playerCoreCloudNowPlayingCoverStart(fallbackUrl); }

bool PlayerService::cloudNowPlayingCoverDecode(uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth,
                                               uint16_t* outHeight) const {
    return playerCoreCloudNowPlayingCoverDecode(scaleFactor, outPixels, outWidth, outHeight);
}

bool PlayerService::cloudMusicNowPlayingTrack(CloudTrackItem* item) const {
    return playerCoreCloudMusicNowPlayingTrack(item);
}

void PlayerService::cloudMusicLyricsStart(const char* trackId) { playerCoreCloudMusicLyricsStart(trackId); }

CloudLyricsState PlayerService::cloudMusicLyricsState() const {
    return static_cast<CloudLyricsState>(playerCoreCloudMusicLyricsState());
}

bool PlayerService::cloudMusicLyricsForTrack(const char* trackId) const {
    return playerCoreCloudMusicLyricsForTrack(trackId);
}

const char* PlayerService::cloudMusicCurrentLyricLine(uint32_t positionMs) const {
    return playerCoreCloudMusicCurrentLyricLine(positionMs);
}

bool PlayerService::cloudMusicPlayTrackStart(const char* trackId) {
    // Same reasoning as playRadioUrl()/playSdFile(): stop whatever's
    // playing now, at the moment playback is actually requested (not
    // just browsing/looking at a list). The resolve itself is async
    // (main.cpp's cloudMusicControllerTask), so unlike those two this
    // doesn't set m_snapshot.source itself -- tick() picks that up from
    // playerCoreReadSnapshot() once the resolve actually connects.
    if (m_snapshot.source == PlayerSource::Radio || m_snapshot.source == PlayerSource::Sd) stop();
    return playerCoreCloudMusicPlayTrackStart(trackId);
}

CloudMusicRequestState PlayerService::cloudMusicResolveState() const {
    return static_cast<CloudMusicRequestState>(playerCoreCloudMusicResolveState());
}

bool PlayerService::cloudMusicJustStarted() const { return playerCoreCloudMusicJustStarted(); }

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
