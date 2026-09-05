#include "common.h"
#include "ui/lvgl_runtime.h"
#include "ui/player_service.h"
#include "mwr_src/function.hpp"
#include "mwr_src/graphical.hpp"
#include "mwr_src/index.h"
#include "mwr_src/index.js.h"
#include "mwr_src/layout.hpp"
#include "gbk_table.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cstring>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <cctype>

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_MSC_ENABLED && !ARDUINO_USB_MODE
#include <USB.h>
#include <USBMSC.h>
#define MWR_USB_MSC_SUPPORTED 1
#else
#define MWR_USB_MSC_SUPPORTED 0
#endif

// USB 声卡（UAC2）模式。和 U 盘模式并列的第三个独立模式，两者互斥——
// 一个 USB 口、一套 TinyUSB 配置，同时只能是一种用途。
#if MWR_USB_DAC && SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_ENABLED && !ARDUINO_USB_MODE
#include "usb_dac.h"
// usb_persist_restart() / restart_type_t 在这里声明（<USB.h> 不转出来），
// 退出声卡模式时要用它把 USB PHY 干净地交还给 ROM 的 USB-Serial/JTAG。
#include "esp32-hal-tinyusb.h"
// 退出声卡模式时要把 USB PHY 的归属交还给 ROM 的 USB-Serial/JTAG，
// 见 usbDacRebootTask() 里的详细说明。
#include "soc/usb_pins.h"  // USBPHY_DP_NUM / USBPHY_DM_NUM
#include "library/library_store.h"  // 本地音乐库 2.0 索引持久化
#include "library/library_cleaner.h"  // 空间评估与淘汰（第一版只做 dry-run）
#include "discovery/jamendo_provider.h"  // Phase 3：候选发现（不下载）
#include "discovery/download_manager.h"  // Phase 4：流式下载 .part + rename
#include "discovery/daily_sync.h"        // Phase 5：每日发现同步
#define MWR_USB_DAC_SUPPORTED 1
#else
#define MWR_USB_DAC_SUPPORTED 0
#endif

// Unified memory snapshot for the LVGL-96KB-to-PSRAM A/B test (see
// src/lv_conf.h's LV_ATTRIBUTE_LARGE_RAM_ARRAY comment) -- ESP.getFreeHeap()
// alone can't distinguish internal vs PSRAM, and doesn't capture largest-
// free-block (fragmentation) or the low-water mark, all of which matter for
// judging whether this migration actually freed usable internal RAM rather
// than just moving the reported total around.
static void logMemoryState(const char* stage) {
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internalMinimum = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    printf("[MEM][%s] internal_free=%u internal_largest=%u internal_min=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u\n", stage,
           (unsigned)internalFree, (unsigned)internalLargest, (unsigned)internalMinimum, (unsigned)dmaFree, (unsigned)dmaLargest,
           (unsigned)psramFree, (unsigned)psramLargest);
}
// clang-format off
/*****************************************************************************************************************************************************
    MiniWebRadio -- Webradio receiver for ESP32-S3

    first release on 03/2017                                                                                                      */char Version[] ="\
    Version 4.2.0k - Jul 06, 2026                                                                                                               ";

/*  display (320x240px) with controller ILI9341 or
    display (480x320px) with controller ILI9486, ILI9488 or ST7796 (SPI) or
    display (800x480px) (RGB-HMI) with TP controller GT911 (I2C)
    display (1024x600px) (DSI) with TP controller GT911 (I2C)

    SD_MMC is mandatory
    IR remote is optional
    BT Transmitter is optional
    BH1750 (lightsensor) is optional

*****************************************************************************************************************************************************/

// THE SOFTWARE IS PROVIDED "AS IS" FOR PRIVATE USE ONLY, IT IS NOT FOR COMMERCIAL USE IN WHOLE OR PART OR CONCEPT. FOR PERSONAL USE IT IS SUPPLIED
// WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
// AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHOR OR COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE

// clang-format on

SET_LOOP_TASK_STACK_SIZE(14 * 1024);

// global variables

char _hl_item[18][40]{"",                    // none
                      "Internet Radio",      // "* интернет-радио *"  "ραδιόφωνο Internet"
                      "Audio Player",        // "** цифрово́й плеер **
                      "DLNA",                // Digital Living Network Alliance
                      "Clock",               // Clock "** часы́ **"  "** ρολόι **"
                      "Brightness",          // Brightness яркость λάμψη
                      "Alarm Clock (hh:mm)", // AlarmClock "будильник" "ξύπνημα"
                      "Off Timer (h:mm)",    // "Sleeptimer" "Χρονομετρητής" "Таймер сна"
                      "Stations List",
                      "Audio Files",
                      "DLNA List",
                      "Bluetooth",
                      "Equalizer",
                      "Settings",
                      "IR Settings",
                      "Alarm",
                      "WiFi Settings", //
                      ""};

constexpr uint16_t MAX_STATIONS = 1000;

dlnaHistory_s s_dlnaHistory[10];
timecounter_s s_timeCounter;
SD_content    s_SD_content;
Playlist      playlist;

IR_buttons     irb(&s_settings);
IR             ir(IR_PIN); // do not change the objectname, it must be "ir"
File           audioFile;
FtpServer      ftpSrv;
DLNA_Client    dlna;
KCX_BT_Emitter bt_emitter(BT_EMITTER_RX, BT_EMITTER_TX, BT_EMITTER_CONNECT, BT_EMITTER_MODE);
hp_BH1750      BH1750; // create the sensor
ES8311         es8311;

ps_ptr<char> s_time_s = "";
ps_ptr<char> s_myIP;
ps_ptr<char> s_icyDescription = "";
ps_ptr<char> s_streamTitle = "";
ps_ptr<char> s_cur_AudioFolder = "/audiofiles/";
ps_ptr<char> s_cur_AudioFileName = NULL;
ps_ptr<char> s_stationURL;
ps_ptr<char> s_playlistPath;
ps_ptr<char> s_stationName_air;
ps_ptr<char> s_homepage = "";
ps_ptr<char> s_TZName = "Europe/Berlin";
ps_ptr<char> s_TZString = "CET-1CEST,M3.5.0,M10.5.0/3";
ps_ptr<char> s_timeSpeechLang = "en";
ps_ptr<char> s_lyrics = "";

bool s_f_rtc = false; // true if time from ntp is received

// ⚠️ **不要用 rtc.hasValidTime() 来判断时间是否已同步 —— 它恒为 false。**
//
// RTIME::hasValidTime() 读的是**成员** timeinfo 的 tm_year，而本工程每秒调用的
// RTIME::gettime_s() 内部把 now / timeinfo 声明成了**局部变量**，遮蔽了同名成员：
//
//     const char* RTIME::gettime_s(){
//         time_t now;            // ← 局部，遮蔽成员
//         struct tm timeinfo;    // ← 局部，遮蔽成员
//
// 于是成员 timeinfo 永远停在构造时的全零，tm_year = 0 < (2016-1900)，
// hasValidTime() 恒返回 false。
//
// 2026-09-05 实测后果（s_f_rtc 恒 false）：
//   - dailySyncDue() 恒 false → **每日同步一次都不会跑**
//   - 播放事件时间戳恒 0 → lastPlayedAt 永远不写
//   - 扫描时间戳恒 0 → 所有 importedAt 都是 0
//   后两条会让 Cleaner 的淘汰评分（按"多久没听""何时导入"）完全退化。
//
// 直接看真实 epoch：早于 ~2023 就是 SNTP 还没同步。
// UI 时钟那条路径早就这么绕过了（见 buildPlayerSnapshot），
// 但当时只修了显示，没修 s_f_rtc。
static inline bool systemTimeSynced() { return time(nullptr) > 1700000000; }
bool s_f_100ms = false;
bool s_f_1sec = false;
bool s_f_10sec = false;
bool s_f_1min = false;
bool s_f_mute = false;
bool s_f_muteIsPressed = false;
bool s_f_recording = false;
bool s_f_sleeping = false;
bool s_f_isWebConnected = false;
bool s_f_WiFi_lost = false;
bool s_f_isFSConnected = false;
// Disambiguates PlayerSource::Radio vs ::CloudMusic when s_f_isWebConnected
// is true -- both play via the same connecttohost() stream connect (see
// playerCorePlayCloudUrl()), so playerCoreReadSnapshot() can't tell them
// apart from s_f_isWebConnected alone. Set true only by
// playerCorePlayCloudUrl(); every other connecttohost() call site
// (playerCorePlayRadioUrl, playerCorePlayStationNumber) resets it false.
bool s_cloudMusicPlaying = false;
bool s_f_eof = false;
bool s_f_reconnect = false;
bool s_f_eof_alarm = false;
bool s_f_alarm = false;
bool s_f_newIcyDescription = false;
bool s_f_newStreamTitle = false;
bool s_f_webFailed = false;
bool s_f_newBitRate = false;
bool s_f_newStationName = false;
bool s_f_newLyrics = false;
bool s_f_volBarVisible = false;
bool s_f_switchToClock = false;   // jump into CLOCK mode at the next opportunity
bool s_f_timeAnnouncement = true; // time announcement every full hour
bool s_f_playlistEnabled = false;
bool s_f_playlistNextFile = false;
bool s_f_logoUnknown = false;
bool s_f_pauseResume = false;
bool s_f_FFatFound = false;
bool s_f_clearLogo = false;
bool s_f_clearStationName = false;
bool s_f_dlnaBrowseServer = false;
bool s_f_dlnaWaitForResponse = false;
bool s_f_dlnaSeekServer = false;
bool s_f_dlnaMakePlaylistOTF = false; // notify callback that this browsing was to build a On-The_fly playlist
bool s_f_dlna_browseReady = false;
bool s_f_brightnessIsChangeable = false;
bool s_f_connectToLastStation = false;
bool s_f_msg_box = false;
bool s_f_esp_restart = false;
bool s_f_timeSpeech = false;
bool s_f_stationsChanged = false;
bool s_f_sd_card_found = false;
bool s_f_isWiFiConnected = false;
bool s_f_ok_from_ir = false;
// WiFi setup hotspot: broadcast when no saved network is reachable (see
// startWifiApFallback()), so a phone can join it and add real credentials
// via the WiFi setup page instead of the device being unreachable forever.
static volatile bool s_wifiApFallbackActive = false;
static const char* kWifiSetupApSsid = "MiniWebRadio-Setup";
static uint32_t s_eofCount = 0; // see PlayerSnapshot::eofCount

int8_t   s_state = UNDEFINED; // statemaschine
int8_t   s_subState = UNDEFINED;
int8_t   s_subState_radio = UNDEFINED;
int8_t   s_subState_player = UNDEFINED;
int8_t   s_subState_clock = UNDEFINED;
int8_t   s_ir_btn_select = UNDEFINED; // IR menue item
int8_t   s_currDLNAsrvNr = -1;
uint8_t  s_alarmdays = 0;
uint8_t  s_cur_Codec = 0;
uint8_t  s_numServers = 0; //
uint8_t  s_level = 0;
uint8_t  s_sleepMode = 1; // 0 display off, 1 show the clock
AudioOutputPolicy s_audioOutputPolicy = AudioOutputPolicy::Source;
uint8_t  s_staListPos = 0;
uint8_t  s_cthFailCounter = 0; // connecttohost fail
uint8_t  s_itemListPos = 0;    // DLNA items
uint8_t  s_fileListPos = 0;
int8_t   s_alarmSubMenue = -1;
int8_t   s_sleepTimerSubMenue = -1;
uint8_t  s_ambientValue = 50;
uint8_t  s_dlnaLevel = 0;
uint8_t  s_resetReason = (esp_reset_reason_t)ESP_RST_UNKNOWN;
int16_t  s_totalNumberReturned = -1;
int16_t  s_dlnaMaxItems = -1;
int16_t  s_dlnaMaXServers = -1;
int16_t  s_alarmtime[7] = {0};  // in minutes (23:59 = 23 *60 + 59) [0] Sun, [1] Mon
int16_t  s_cur_AudioFileNr = 0; // this is the position of the file within the (alpha ordered) folder starting with 0
uint8_t  s_brightness = UINT8_MAX;
uint8_t  s_bh1750Value = UINT8_MAX;
uint16_t s_staListNr = 0;
uint16_t s_fileListNr = 0;
uint16_t s_cur_station = 0; // current station(nr), will be set later
uint16_t s_sleeptime = 0;   // time in min until MiniWebRadio goes to sleep
uint16_t s_plsCurPos = 0;
uint16_t s_dlnaItemNr = 0;
uint16_t s_h_resolution = 320;
uint16_t s_v_resolution = 240;
uint32_t s_icyBitRate = 0;     // from http response header via event
uint32_t s_decoderBitRate = 0; // from decoder via getBitRate(false)
uint32_t s_playlistTime = 0;   // playlist start time millis() for timeout
uint32_t s_settingsHash = 0;
uint32_t s_audioFileSize = 0;
uint32_t s_media_downloadPort = 0;
uint32_t s_audioCurrentTime = 0;
uint32_t s_timestamp = 0;
uint32_t s_audioFileDuration = 0;
uint64_t s_totalRuntime = 0; // total runtime in seconds since start

// One-shot "stable playback" memory snapshots for the LVGL-96KB-to-PSRAM A/B
// test (see src/lv_conf.h): 0 = no snapshot pending, else the s_totalRuntime
// value at which to fire one (checked in loopLvglRuntime()'s s_f_1sec
// block). Set a few seconds out from playback actually starting so the
// snapshot reflects steady-state, not the transient connect/decode-startup
// allocations.
uint64_t s_memLogRadioAtSec = 0;
uint64_t s_memLogLocalAtSec = 0;

std::deque<ps_ptr<char>> s_PLS_content;
std::deque<ps_ptr<char>> s_logBuffer;

const char* codecname[10] = {"unknown", "WAV", "MP3", "AAC", "M4A", "FLAC", "OPUS", "VORBIS", "OGG"};

#ifdef TFT_MODE_SPI // ⏹⏹⏹⏹
TFT_SPI  tft(spiBus, TFT_CS);
TFT_SPI& getTFT() {
    return tft;
}
#elif defined TFT_MODE_RGB
TFT_RGB  tft;
TFT_RGB& getTFT() {
    return tft;
}
#elif defined TFT_MODE_DSI
TFT_DSI  tft;
TFT_DSI& getTFT() {
    return tft;
}
#else
    #error "wrong TFT_CONTROLLER"
#endif

#ifdef TP_MODE_XPT2046 // ⏹⏹⏹⏹
TP_XPT2046  tp(spiBus, TP_CS);
TP_XPT2046& getTP() {
    return tp;
}
#elif defined TP_MODE_GT911
TP_GT911  tp;
TP_GT911& getTP() {
    return tp;
}
#elif defined TP_MODE_FT6X63
FT6x36  tp;
FT6x36& getTP() {
    return tp;
}
#elif defined TP_MODE_CST816
CST816  tp;
CST816& getTP() {
    return tp;
}
#else
    #error "wrong TP_CONTROLLER"
#endif

stationManagement staMgnt(&s_cur_station);

SemaphoreHandle_t mutex_rtc;
SemaphoreHandle_t mutex_display;
static SemaphoreHandle_t s_prefMutex = nullptr;
static SemaphoreHandle_t s_wifiOpMutex = nullptr;

/*  ╔═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
    ║                                                     D E F A U L T S E T T I N G S                                                         ║
    ╚═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝   */

bool SD_MMC_exists(const char* path) {
    return SD_MMC.exists(path);
}

// clang-format off
/*🟢🟡🔴*/
boolean defaultsettings() {

    if (!SD_MMC.exists("/ir_buttons.json")) { // if not found ir_buttons.json create a default file
        File file = SD_MMC.open("/ir_buttons.json", "w", true);
        file.write((uint8_t*)ir_buttons_json, sizeof(ir_buttons_json));
        file.close();
    }
    irb.loadButtonsFromJSON("/ir_buttons.json");
    for (uint i = 0; i < s_settings.numOfIrButtons; i++) {
        MWR_LOG_DEBUG("0x{:04X},  {}", s_settings.irbuttons[i].val, s_settings.irbuttons[i].label);
        ir.set_irButtons(i, s_settings.irbuttons[i].val);
    }
    ir.set_irAddress(s_settings.irbuttons[42].val);
    MWR_LOG_DEBUG("0x{:04X},  {}", s_settings.irbuttons[42].val, s_settings.irbuttons[42].label);

    if (!SD_MMC.exists("/settings.json")) { // if not found create one
        updateSettings();
    }

    File         file2 = SD_MMC.open("/settings.json", "r", false);
    ps_ptr<char> jO;
    jO.calloc(2048);
    ps_ptr<char> tmp;
    tmp.calloc(1024);
    file2.readBytes(jO.get(), 2048);
    s_settingsHash = simpleHash(jO.get());
    file2.close();

    auto parseJson = [&](const char* s) { // lambda, inner function
        int16_t pos1 = 0, pos2 = 0, pos3 = 0;
        pos1 = jO.index_of(s, 0);
        if (pos1 < 0) {
            MWR_LOG_ERROR("index {} not found", s);
            return "0";
        }
        pos2 = jO.index_of(":", pos1) + 1;
        if (jO[pos2] == '\"')
            pos3 = jO.index_of("\"", pos2 + 1) + 1;
        else
            pos3 = jO.index_of(",", pos2);
        if (pos3 < 0) pos3 = find_first_of(jO.get(), "}\n", pos2);
        if (jO[pos2] == '\"') {
            pos2++;
            pos3--;
        } // remove \" embraced strings
        tmp = jO.substr(pos2, pos3 - pos2);
        tmp[pos3 - pos2] = '\0';
        return (const char*)tmp.c_get();
    };

    auto computeMinuteOfTheDay = [&](const char* s) {
        if (!s) return 0;
        int h = atoi(s);
        int m = atoi(s + 3);
        return h * 60 + m;
    };

    s_settings.lastconnectedhost.reset();
    s_settings.lastconnectedfile.reset();
    s_settings.lastconnectedfilepos = 0;

    s_volume.cur_volume = atoi(parseJson("\"volume\":"));
    s_volume.volumeSteps = atoi(parseJson("\"volumeSteps\":"));
    s_volume.ringVolume = atoi(parseJson("\"ringVolume\":"));
    s_volume.volumeAfterAlarm = atoi(parseJson("\"volumeAfterAlarm\":"));
    s_bt_emitter.volume = atoi(parseJson("\"BTvolume\":"));
    s_bt_emitter.enabled  = (strcmp(parseJson("\"BTpower\":"), "true") == 0) ? 1 : 0;
    s_bt_emitter.mode = ((strcmp(parseJson("\"BTmode\":"), "TX") == 0) ? "TX" : "RX");
    s_alarmtime[0] = computeMinuteOfTheDay(parseJson("\"alarmtime_sun\":"));
    s_alarmtime[1] = computeMinuteOfTheDay(parseJson("\"alarmtime_mon\":"));
    s_alarmtime[2] = computeMinuteOfTheDay(parseJson("\"alarmtime_tue\":"));
    s_alarmtime[3] = computeMinuteOfTheDay(parseJson("\"alarmtime_wed\":"));
    s_alarmtime[4] = computeMinuteOfTheDay(parseJson("\"alarmtime_thu\":"));
    s_alarmtime[5] = computeMinuteOfTheDay(parseJson("\"alarmtime_fri\":"));
    s_alarmtime[6] = computeMinuteOfTheDay(parseJson("\"alarmtime_sat\":"));
    s_alarmdays = atoi(parseJson("\"alarm_weekdays\":"));
    s_f_timeAnnouncement = (strcmp(parseJson("\"timeAnnouncing\":"), "true") == 0) ? 1 : 0;
    s_timeSpeechLang = parseJson("\"timeSpeechLang\":");
    s_f_mute = (strcmp(parseJson("\"mute\":"), "true") == 0) ? 1 : 0;
    s_brightness = max(5, atoi(parseJson("\"brightness\":")));
    s_sleeptime = atoi(parseJson("\"sleeptime\":"));
    s_cur_station = atoi(parseJson("\"station\":"));
    s_tone.LP = atoi(parseJson("\"toneLP\":"));
    s_tone.BP = atoi(parseJson("\"toneBP\":"));
    s_tone.HP = atoi(parseJson("\"toneHP\":"));
    s_tone.BAL = atoi(parseJson("\"balance\":"));
    {
        const int outputSampleRate = atoi(parseJson("\"outputSampleRate\":"));
        if (outputSampleRate == 44100) s_audioOutputPolicy = AudioOutputPolicy::Fixed44100;
        else if (outputSampleRate == 48000) s_audioOutputPolicy = AudioOutputPolicy::Fixed48000;
        else s_audioOutputPolicy = AudioOutputPolicy::Source;
    }
    s_TZName = parseJson("\"Timezone_Name\":");
    s_TZString = parseJson("\"Timezone_String\":");
    s_settings.lastconnectedhost.copy_from(parseJson("\"lastconnectedhost\":"));
    s_settings.lastconnectedfile.copy_from(parseJson("\"lastconnectedfile\":"));
    s_settings.lastconnectedfilepos = atoi(parseJson("\"lastconnectedfilepos\":"));
    s_sleepMode = atoi(parseJson("\"sleepMode\":"));
    s_state = atoi(parseJson("\"state\":"));

    // set some items ---------------------------------------------------------------------------------------------
    if (!s_settings.lastconnectedfile.starts_with("/")) { s_settings.lastconnectedfile.assign("/audiofiles/"); } // guard
    s_SD_content.setLastConnectedFile(s_settings.lastconnectedfile.get());
    s_cur_AudioFolder = s_SD_content.getLastConnectedFolder();
    s_cur_AudioFileName = s_SD_content.getLastConnectedFileName();
    s_cur_AudioFileNr = s_SD_content.getPosByFileName(s_cur_AudioFileName.c_get());
    if (s_cur_AudioFileNr == -1) s_cur_AudioFileNr = 0; // not found
    // ------------------------------------------------------------------------------------------------------------

    if (!SD_MMC.exists("/stations.json")) { // if not found create one
        File file1 = SD_MMC.open("/stations.json", "w", true);
        file1.write((uint8_t*)stations_json, sizeof(stations_json) - 1); // without termination
        file1.close();
    }
    staMgnt.updateStationsList();
    if (staMgnt.getSumStations() && strstr(staMgnt.getStationUrl(1), "0n-70s.radionetz.de")) {
        File file1 = SD_MMC.open("/stations.json", "w", true);
        file1.write((uint8_t*)stations_json, sizeof(stations_json) - 1);
        file1.close();
        staMgnt.updateStationsList();
        printfln(s_tag.sd_card, "legacy default stations.json replaced with LVGL radio starter list");
    }
    return true;
}
// clang-format on
/*🟢🟡🔴*/

void updateSettings() {
    if (!s_settings.lastconnectedhost.valid()) s_settings.lastconnectedhost.assign("");
    if (!s_settings.lastconnectedfile.valid()) s_settings.lastconnectedfile.assign("/audiofiles/");
    ps_ptr<char> jO;
    ; // JSON Object
    jO.assign("{\n");
    jO.appendf("  \"volume\":{}", s_volume.cur_volume);
    jO.appendf(",\n  \"volumeSteps\":{}", s_volume.volumeSteps);
    jO.appendf(",\n  \"ringVolume\":{}", s_volume.ringVolume);
    jO.appendf(",\n  \"volumeAfterAlarm\":{}", s_volume.volumeAfterAlarm);
    jO.appendf(",\n  \"BTvolume\":{}", s_bt_emitter.volume);
    jO.appendf(",\n  \"BTpower\":\"{}\"", s_bt_emitter.enabled);
    jO.appendf(",\n  \"BTmode\":\"{}\"", bt_emitter.getMode().c_get());
    jO.appendf(",\n  \"alarmtime_sun\":\"{:02}:{:02}\"", s_alarmtime[0] / 60, s_alarmtime[0] % 60);
    jO.appendf(",\n  \"alarmtime_mon\":\"{:02}:{:02}\"", s_alarmtime[1] / 60, s_alarmtime[1] % 60);
    jO.appendf(",\n  \"alarmtime_tue\":\"{:02}:{:02}\"", s_alarmtime[2] / 60, s_alarmtime[2] % 60);
    jO.appendf(",\n  \"alarmtime_wed\":\"{:02}:{:02}\"", s_alarmtime[3] / 60, s_alarmtime[3] % 60);
    jO.appendf(",\n  \"alarmtime_thu\":\"{:02}:{:02}\"", s_alarmtime[4] / 60, s_alarmtime[4] % 60);
    jO.appendf(",\n  \"alarmtime_fri\":\"{:02}:{:02}\"", s_alarmtime[5] / 60, s_alarmtime[5] % 60);
    jO.appendf(",\n  \"alarmtime_sat\":\"{:02}:{:02}\"", s_alarmtime[6] / 60, s_alarmtime[6] % 60);
    jO.appendf(",\n  \"alarm_weekdays\":{}", s_alarmdays);
    jO.appendf(",\n  \"timeAnnouncing\":\"{}\"", s_f_timeAnnouncement);
    jO.appendf(",\n  \"timeSpeechLang\":\"{}\"", s_timeSpeechLang.c_get());
    jO.appendf(",\n  \"mute\":\"{}\"", s_f_mute);
    jO.appendf(",\n  \"brightness\":{}", s_brightness);
    jO.appendf(",\n  \"sleeptime\":{}", s_sleeptime);
    jO.appendf(",\n  \"lastconnectedhost\":\"{}\"", s_settings.lastconnectedhost.c_get());
    jO.appendf(",\n  \"lastconnectedfile\":\"{}\"", s_settings.lastconnectedfile.c_get());
    jO.appendf(",\n  \"lastconnectedfilepos\":{}", s_settings.lastconnectedfilepos);
    jO.appendf(",\n  \"station\":{}", s_cur_station);
    jO.appendf(",\n  \"Timezone_Name\":\"{}\"", s_TZName.c_get());
    jO.appendf(",\n  \"Timezone_String\":\"{}\"", s_TZString.c_get());
    jO.appendf(",\n  \"toneLP\":{}", s_tone.LP);
    jO.appendf(",\n  \"toneBP\":{}", s_tone.BP);
    jO.appendf(",\n  \"toneHP\":{}", s_tone.HP);
    jO.appendf(",\n  \"balance\":{}", s_tone.BAL);
    const uint32_t outputSampleRate = s_audioOutputPolicy == AudioOutputPolicy::Fixed44100 ? 44100 :
                                      s_audioOutputPolicy == AudioOutputPolicy::Fixed48000 ? 48000 : 0;
    jO.appendf(",\n  \"outputSampleRate\":{}", outputSampleRate);
    jO.appendf(",\n  \"state\":{}", s_state);
    jO.appendf(",\n  \"sleepMode\":{}\n}", s_sleepMode);

    if (s_settingsHash != simpleHash(jO.get())) {
        File file = SD_MMC.open("/settings.json", "w", false);
        if (!file) {
            MWR_LOG_ERROR("file \"settings.json\" not found");
            return;
        }
        file.print(jO.get());
        s_settingsHash = simpleHash(jO.c_get());

        MWR_LOG_DEBUG("{}", jO.c_get());
    }
}
/*****************************************************************************************************************************************************
 *                                                      U R L d e c o d e                                                                            *
 *****************************************************************************************************************************************************/
// In m3u playlists, file names can be URL encoded.
// Since UTF-8 is always shorter than URI, the same memory is used for decoding
// e.g. Born%20On%20The%20B.mp3 --> Born On The B.mp3
// e.g. %D0%B8%D1%81%D0%BF%D1%8B%D1%82%D0%B0%D0%BD%D0%B8%D0%B5.mp3 --> испытание.mp3
void urldecode(char* str) {
    uint16_t p1 = 0, p2 = 0;
    char     a, b;
    while (str[p1]) {
        if ((str[p1] == '%') && ((a = str[p1 + 1]) && (b = str[p1 + 2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            str[p2++] = 16 * a + b;
            p1 += 3;
        } else if (str[p1] == '+') {
            str[p2++] = ' ';
            p1++;
        } else {
            str[p2++] = str[p1++];
        }
    }
    str[p2++] = '\0';
}

/*****************************************************************************************************************************************************
 *                                                               T I M E R                                                                           *
 *****************************************************************************************************************************************************/

void timer100ms() {
    static uint16_t ms100 = 0;
    s_f_100ms = true;
    ms100++;
    if (!(ms100 % 10)) {
        s_f_1sec = true;
        s_time_s = rtc.gettime_s();
        if (s_time_s.ends_with("59:53")) s_f_timeSpeech = true;
    }
    if (!(ms100 % 100)) s_f_10sec = true;
    if (!(ms100 % 600)) {
        s_f_1min = true;
        ms100 = 0;
    }
}

/*****************************************************************************************************************************************************
 *                                                               D I S P L A Y                                                                       *
 *****************************************************************************************************************************************************/

inline void clearLogo() {
    getTFT().copyFramebuffer(FB_BACKGROUND, FB_VISIBLE, layout.winLogo.x, layout.winLogo.y, layout.winLogo.w, layout.winLogo.h);
}
inline void clearStationName() {
    getTFT().copyFramebuffer(FB_BACKGROUND, FB_VISIBLE, layout.winName.x, layout.winName.y, layout.winName.w, layout.winName.h);
}
inline void clearStreamTitle() { // without VUmeter
    getTFT().copyFramebuffer(FB_BACKGROUND, FB_VISIBLE, layout.winSTitle.x, layout.winSTitle.y, layout.winSTitle.w, layout.winSTitle.h);
}
inline void clearWithOutHeaderFooter(int32_t bgColor) {
    if (bgColor == TFT_TRANSPARENT) {
        getTFT().copyFramebuffer(FB_BACKGROUND, FB_VISIBLE, layout.winWoHF.x, layout.winWoHF.y, layout.winWoHF.w, layout.winWoHF.h);
    } else {
        getTFT().fillRect(layout.winWoHF.x, layout.winWoHF.y, layout.winWoHF.w, layout.winWoHF.h, TFT_BLACK);
    }
}
inline void clearAll(int32_t bgColor) {
    if (bgColor == TFT_TRANSPARENT) {
        getTFT().copyFramebuffer(FB_BACKGROUND, FB_VISIBLE, 0, 0, displayConfig.dispWidth, displayConfig.dispHeight); // copy wallpaper
    } else {
        getTFT().fillRect(0, 0, displayConfig.dispWidth, displayConfig.dispHeight, TFT_BLACK);
    }
}

void showStationName() {
    if (s_f_sleeping) return;
    txt_RA_staName.setTextColor(TFT_CYAN);
    txt_RA_staName.setText(getStationName());
    txt_RA_staName.show();
}

void showStreamTitle(ps_ptr<char> streamtitle) {
    if (s_f_sleeping) return;

    streamtitle.trim();
    // replacestr(st, " | ", "\n"); // some stations use pipe as \n or
    // replacestr(st, "| ", "\n");
    // replacestr(st, "|", "\n");

    txt_RA_sTitle.setTextColor(TFT_CORNSILK);
    txt_RA_sTitle.setText(streamtitle.c_get());
    txt_RA_sTitle.show();
}

void showLogoAndStationName() {
    showStationName();
    pic_RA_logo.setPicturePath(getLogoPath());
    pic_RA_logo.show();
    dispFooter.updateStation(s_cur_station);
    dispFooter.updateFlag(getFlagPath(s_cur_station));
    webSrv_send_station_items();
    return;
}

ps_ptr<char> getStationName() {
    ps_ptr<char> SN_utf8;
    ps_ptr<char> path;
    if (s_cur_station) {
        SN_utf8 = staMgnt.getStationName(s_cur_station);
    } else {
        SN_utf8 = s_stationName_air;
    }
    SN_utf8.trim();
    return SN_utf8;
}

ps_ptr<char> getLogoPath() {
    ps_ptr<char> path;
    ps_ptr<char> staName = getStationName();
    path = "/logo/" + staName + ".jpg";
    if (!SD_MMC.exists(scaleImage(path).c_get())) path = "/common/unknown.png";
    return path;
}

const char* getFlagPath(uint16_t station) {
    if (station == 0) return "/flags/unknown.jpg";
    static char flagPath[40];
    flagPath[0] = '\0';
    strcpy(flagPath, "/flags/");
    strcat(flagPath, staMgnt.getStationCountry(station));
    for (int i = 0; i < strlen(flagPath); i++) flagPath[i] = tolower(flagPath[i]);
    strcat(flagPath, ".jpg");
    return flagPath;
}

void webSrv_send_station_items() {
    ps_ptr<char> staNr;
    staNr.assignf("{}", s_cur_station);
    webSrv.send("stationLogo=", getLogoPath());
    webSrv.send("stationNr=", staNr);
    webSrv.send("stationURL=", s_settings.lastconnectedhost.get());
}

void showFileLogo(int8_t state, int8_t subState) {
    String logo;
    if (state == DLNA) {
        logo = "/common/DLNA.jpg";
        pic_DL_logo.setPicturePath(logo.c_str());
        pic_DL_logo.setAlternativPicturePath("/common/unknown.png");
        pic_DL_logo.show();
        webSrv.send("stationLogo=", logo.c_str());
        return;
    }
    if (state == PLAYER) { // s_state PLAYER
        if (s_cur_Codec == 0)
            logo = "/common/AudioPlayer.png";
        else if (s_subState_player == 0)
            logo = "/common/AudioPlayer.png";
        else
            logo = "/common/" + (String)codecname[s_cur_Codec] + ".png";
        pic_PL_logo.setPicturePath(logo.c_str());
        pic_PL_logo.setAlternativPicturePath("/common/unknown.png");
        pic_PL_logo.show();
        return;
    }
    if (state == SETTINGS) {
        logo = "/common/Settings.png";
        pic_SE_logo.setPicturePath(logo.c_str());
        pic_SE_logo.setAlternativPicturePath("/common/unknown.png");
        pic_SE_logo.show();
        return;
    }
    if (state == RINGING) {
        logo = "/common/Alarm.png";
        pic_RI_logo.setPicturePath(logo.c_str());
        pic_RI_logo.setAlternativPicturePath("/common/unknown.png");
        pic_RI_logo.show();
        return;
    }
}

void showPlayerFileName(const char* fname) {
    if (!fname) return;
    txt_PL_fName.setTextColor(TFT_CYAN);
    txt_PL_fName.setText(fname);
    txt_PL_fName.show();
}

void show_DLNA_FileName(const char* fname) {
    if (!fname) return;
    txt_DL_fName.setTextColor(TFT_CYAN);
    txt_DL_fName.setText(fname);
    txt_DL_fName.show();
}

void showPlsFileNumber() {
    char buf[15];
    sprintf(buf, "%03u/%03u", s_plsCurPos, s_PLS_content.size());
    dispFooter.updateFileNr(buf);
}

void showAudioFileNumber() {
    char buf[15];
    sprintf(buf, "%03u/%03u", s_cur_AudioFileNr + 1, s_SD_content.getSize());
    dispFooter.updateFileNr(buf);
}

void display_sleeptime(int8_t ud) { // set sleeptimer
    if (ud == 1) {                  // up
        switch (s_sleeptime) {
            case 0 ... 14: s_sleeptime = (s_sleeptime / 5) * 5 + 5; break;
            case 15 ... 59: s_sleeptime = (s_sleeptime / 15) * 15 + 15; break;
            case 60 ... 359: s_sleeptime = (s_sleeptime / 60) * 60 + 60; break;
            default: s_sleeptime = 360; break; // max 6 hours
        }
    }
    if (ud == -1) { // down
        switch (s_sleeptime) {
            case 1 ... 15: s_sleeptime = ((s_sleeptime - 1) / 5) * 5; break;
            case 16 ... 60: s_sleeptime = ((s_sleeptime - 1) / 15) * 15; break;
            case 61 ... 360: s_sleeptime = ((s_sleeptime - 1) / 60) * 60; break;
            default: s_sleeptime = 0; break; // min
        }
    }
    otb_SL_stime.show(s_sleeptime);
}

boolean drawImage(ps_ptr<char> path, uint16_t posX, uint16_t posY, uint16_t maxWidth, uint16_t maxHeigth) {
    auto scImg = scaleImage(path);
    if (!SD_MMC.exists(scImg.c_get())) {
        if (scImg.index_of("/.", 0) > 0) return false; // empty filename
        printfln(s_tag.sd_card, ANSI_ESC_RED "file \"{}\" not found", scImg.c_get());
        return false;
    }
    if (scImg.ends_with("bmp")) { return getTFT().drawBmpFile(SD_MMC, scImg.c_get(), posX, posY, maxWidth, maxHeigth, 1.0); }
    if (scImg.ends_with("jpg")) { return getTFT().drawJpgFile(SD_MMC, scImg.c_get(), posX, posY, maxWidth, maxHeigth); }
    if (scImg.ends_with("gif")) { return getTFT().drawGifFile(SD_MMC, scImg.c_get(), posX, posY, 0); }
    if (scImg.ends_with("png")) { return getTFT().drawPngFile(SD_MMC, scImg.c_get(), posX, posY); }

    printfln(s_tag.action, ANSI_ESC_RED "the file \"{}\" contains neither a bmp, a gif, a png nor a jpg graphic", scImg);
    return false; // neither jpg nor bmp
}
/*****************************************************************************************************************************************************
 *                                                   H A N D L E  A U D I O F I L E                                                                  *
 *****************************************************************************************************************************************************/

boolean isAudio(File file) {
    if (endsWith(file.name(), ".mp3") || endsWith(file.name(), ".aac") || endsWith(file.name(), ".m4a") || endsWith(file.name(), ".wav") || endsWith(file.name(), ".flac") || endsWith(file.name(), ".opus") || endsWith(file.name(), ".ogg")) { return true; }
    return false;
}

boolean isAudio(const char* path) {
    if (endsWith(path, ".mp3") || endsWith(path, ".aac") || endsWith(path, ".m4a") || endsWith(path, ".wav") || endsWith(path, ".flac") || endsWith(path, ".opus") || endsWith(path, ".ogg")) { return true; }
    return false;
}

boolean isPlaylist(File file) {
    if (endsWith(file.name(), ".m3u")) { return true; }
    return false;
}

/*****************************************************************************************************************************************************
 *                                                                     P L A Y L I S T                                                               *
 *****************************************************************************************************************************************************/

void processPlaylist() {
    bool f_isURL, f_isFile;
start:
    f_isURL = false;
    f_isFile = false;
    if (playlist.get_size() == 0) { // guard
        MWR_LOG_ERROR("playlist is empty");
        s_f_playlistEnabled = false;
        return;
    }

    int idx = playlist.next_index();
    if (idx == -1) {
        printfln(s_tag.playlist, ANSI_ESC_YELLOW "end of playlist");
        webSrv.send("SD_playFile=", "end of playlist");
        s_f_playlistEnabled = false;
        changeState(PLAYER, 0);
        return;
    }

    if (idx == 0) { // first
        changeState(PLAYER, 1);
        txt_PL_fName.setText("");
        txt_PL_fName.show();
    }

    printfln(s_tag.playlist, ANSI_ESC_YELLOW "next playlist file");
    s_f_playlistEnabled = true;

    ps_ptr<char> path = playlist.get_file(); // path or url

    if (path.starts_with_icase("http;//") or path.starts_with_icase("https://")) {
        f_isURL = true; // is web file
    }

    if (path.starts_with("/") && SD_MMC.exists(path.c_get())) { f_isFile = true; }

    if (f_isFile == false && f_isURL == false) goto start;

    if (f_isURL) { connecttohost(path); }                  // is web file
    if (f_isFile) { connecttoFS("SD_MMC", path.c_get()); } // is file

    if (s_f_isFSConnected || s_f_isWebConnected) {
        printfln(s_tag.playlist, ANSI_ESC_YELLOW, path.c_get());
        webSrv.send("SD_playFile=", path);
        if (s_state == PLAYER) dispFooter.updateFileNr(playlist.get_coloured_index().c_get());
        txt_PL_fName.setText(playlist.get_items().c_get());
        txt_PL_fName.show();
    } else {
        printfln(s_tag.playlist, ANSI_ESC_YELLOW "can't connect to {}", path.c_get());
        goto start;
    }

    MWR_LOG_WARN("path {}, items {}", playlist.get_file().c_get(), playlist.get_items().c_get());

    return;
}

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// 📌📌📌  C O N N E C T   TO   W I F I   📌📌📌
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————

// Slot 0 is the compile-time default (_SSID/_PW), rewritten every boot and
// never user-editable (see setWiFiCredentials()'s state==0 case). Slots
// 1-7 are the up-to-7 user-added networks from the Settings > WiFi screen
// (8 total saved networks, per "最多保存8个"). wifiPrefKey() replaces what
// used to be four near-identical 6-case switch statements across this file
// -- extending those by hand for the 6->8 slot change would have meant
// copy-pasting 2 more cases into each of the 4 call sites.
constexpr uint8_t kWifiSlotCount = 8;
const char* wifiPrefKey(uint8_t i) {
    static const char* const keys[kWifiSlotCount] = {"wifiStr0", "wifiStr1", "wifiStr2", "wifiStr3", "wifiStr4", "wifiStr5", "wifiStr6", "wifiStr7"};
    return keys[i < kWifiSlotCount ? i : kWifiSlotCount - 1];
}

static bool lockPreferences(TickType_t timeout = pdMS_TO_TICKS(250)) {
    return !s_prefMutex || xSemaphoreTake(s_prefMutex, timeout) == pdTRUE;
}

static void unlockPreferences() {
    if (s_prefMutex) xSemaphoreGive(s_prefMutex);
}

// USB storage mode is entered by writing a temporary NVS flag and
// rebooting. Runtime USB.begin() is unsafe on this board (ESP_RST_USB --
// see DEV_LOG 2026-07-31), so TinyUSB is only ever started at boot:
// never in normal mode, and with media exposed immediately in storage
// mode. The flag is cleared (and the board rebooted) on host eject or on
// the unmount button, so normal boots never initialize TinyUSB and the
// USB-Serial/JTAG console stays alive.
static constexpr const char* kUsbMscModePrefKey = "usb_msc_mode";

static bool usbMscModeFlag() {
    bool value = false;
    if (lockPreferences()) {
        value = pref.getBool(kUsbMscModePrefKey, false);
        unlockPreferences();
    }
    return value;
}

static void usbMscSetModeFlag(bool on) {
    if (lockPreferences(pdMS_TO_TICKS(1000))) {
        pref.putBool(kUsbMscModePrefKey, on);
        unlockPreferences();
    }
}

// USB 声卡模式标志。和 MSC 那个是同一套机制、同样的理由：这块板运行时调
// USB.begin() 会触发 ESP_RST_USB 复位（见本文件上面那段注释），TinyUSB 只能
// 在开机时启动，所以切换模式必须写标志 + 重启，绕不过去。
//
// 两个标志互斥由 usbDacSetModeFlag()/usbMscSetModeFlag() 的调用方保证：
// UI 上是三选一的单选，选中任意一个就会把另一个清掉。
static constexpr const char* kUsbDacModePrefKey = "usb_dac_mode";

static bool usbDacModeFlag() {
    bool value = false;
    if (lockPreferences()) {
        value = pref.getBool(kUsbDacModePrefKey, false);
        unlockPreferences();
    }
    return value;
}

// 记住"最后一次播放过的音源"。
//
// 用途：重启后什么都还没播时，首页"正在播放"区域被点击应当进入**上次那个音源
// 的播放页**，而不是那个早已废弃的通用播放页（见 hifi_ui.cpp 的
// onHomeNowPlayingAction）。
//
// 为什么不复用 s_settings：那里虽然有 lastconnectedhost（电台）和
// lastconnectedfile（本地），但它们**只记内容、不记顺序**——两者可能同时非空，
// 无法判断最后一次是哪种；而且云音乐完全没有对应字段。所以单独存一个。
//
// 值就是 PlayerSource 枚举（0=None 1=Radio 2=Sd 3=Dlna 4=CloudMusic）。
static constexpr const char* kLastSourcePrefKey = "last_source";
static uint8_t s_lastSourceCached = 0xFF; // 0xFF = 还没从 NVS 读过

uint8_t playerCoreLastSource() {
    if (s_lastSourceCached == 0xFF) {
        if (!lockPreferences()) return 0;
        s_lastSourceCached = pref.getUChar(kLastSourcePrefKey, 0);
        unlockPreferences();
    }
    return s_lastSourceCached;
}

void playerCoreSetLastSource(uint8_t source) {
    // 只在真的变化时才写 —— 这个函数每 60ms 会被 tick() 调到一次，
    // 无脑写会毫无必要地磨损 flash。
    if (source == s_lastSourceCached) return;
    s_lastSourceCached = source;
    if (lockPreferences(pdMS_TO_TICKS(1000))) {
        pref.putUChar(kLastSourcePrefKey, source);
        unlockPreferences();
    }
}

static void usbDacSetModeFlag(bool on) {
    if (lockPreferences(pdMS_TO_TICKS(1000))) {
        pref.putBool(kUsbDacModePrefKey, on);
        if (on) pref.putBool(kUsbMscModePrefKey, false); // 互斥
        unlockPreferences();
    }
}

static ps_ptr<char> wifiPrefGet(uint8_t i) {
    ps_ptr<char> line;
    if (!lockPreferences()) {
        MWR_LOG_WARN("Preferences busy while reading {}", wifiPrefKey(i));
        line = "";
        return line;
    }
    line = pref.getString(wifiPrefKey(i)).c_str();
    unlockPreferences();
    return line;
}

static bool lockWifiOps(TickType_t timeout = pdMS_TO_TICKS(100)) {
    return !s_wifiOpMutex || xSemaphoreTake(s_wifiOpMutex, timeout) == pdTRUE;
}

static void unlockWifiOps() {
    if (s_wifiOpMutex) xSemaphoreGive(s_wifiOpMutex);
}

// _SSID/_PW get seeded into slot 0 by connectToWiFi() whenever it's empty.
// Only the literal, unconfigured PlatformIO defaults ("SSID"/"PASSWORD") are
// placeholders. If platformio_override.ini supplies real build-time
// credentials, slot 0 is a real saved network and must be shown in the WiFi
// list; otherwise the device can connect successfully while Settings > WiFi
// still appears empty.
bool playerCoreWifiIsPlaceholder(const char* line) {
    if (!line) return false;
    if (strcmp(_SSID, "SSID") != 0 || strcmp(_PW, "PASSWORD") != 0) return false;
    ps_ptr<char> placeholder(64);
    placeholder = _SSID;
    placeholder += "\t";
    placeholder += _PW;
    return strcmp(line, placeholder.c_get()) == 0;
}

bool playerCoreWifiLineIsSavedNetwork(const char* line) {
    if (!line || line[0] == '\0') return false;
    if (playerCoreWifiIsPlaceholder(line)) return false;
    const char* tab = strchr(line, '\t');
    return tab && tab != line;
}

bool connectToWiFi() {

    MWR_LOG_DEBUG("Connecting to WiFi...");
    ps_ptr<char> line(512);

    // create nvs entries if they do not exist
    if (lockPreferences(pdMS_TO_TICKS(1000))) {
        for (uint8_t i = 0; i < kWifiSlotCount; i++) {
            if (!pref.isKey(wifiPrefKey(i))) pref.putString(wifiPrefKey(i), ""); // SSID + \t + PW
        }

        // Seed slot 0 with the build-flag default credentials, but only the
        // first time (slot 0 still empty) -- this used to run unconditionally,
        // clobbering slot 0 on every boot even after the user saved a real
        // network there via the Settings UI.
        line = pref.getString(wifiPrefKey(0)).c_str();
        if (line.strlen() == 0) {
            const char* SSID = _SSID;
            const char* PW = _PW;
            line = SSID;
            line += "\t";
            line += PW;
            pref.putString(wifiPrefKey(0), line.c_get());
        }
        unlockPreferences();
    } else {
        MWR_LOG_WARN("Preferences busy while initializing WiFi slots");
    }
    printfln(s_tag.wifi_info, "free heap right before WiFi.mode(): " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET ", largest internal block: " ANSI_ESC_CYAN "{}",
             ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    WiFi.mode(WIFI_STA);
    // ESP-IDF defaults to the "01" world-safe regulatory domain, which only
    // scans/associates on channels 1-11 -- routers on channel 12/13 (common
    // outside the US, including China) are invisible to WiFi.scanNetworks()
    // even though a phone (using the phone OS's own region) connects to them
    // fine. CN permits 1-13 and matches this project's actual deployment.
    esp_wifi_set_country_code("CN", true);

    for (int i = 0; i < kWifiSlotCount; i++) {
        line.clear();
        line = wifiPrefGet(i).c_get();
        if (!playerCoreWifiLineIsSavedNetwork(line.c_get())) continue;
        int pos = line.index_of("\t", 0); // find first tab
        line[pos] = '\0';                 // terminate ssid
        char* ssid = line.get();          // ssid is the first part
        char* pw = line.get() + pos + 1;  // password is the second part
        MWR_LOG_DEBUG("ssid {}", ssid);
        MWR_LOG_DEBUG("pw {}", pw);
        wifiMulti.addAP(ssid, pw); // SSID and PW in code"
        size_t offset = 0;
        size_t pwlen = strlen(pw);
        size_t dot_len = strlen(emoji.blueCircle); // = 4
        size_t buf_size = pwlen * dot_len + 1;     // +1 für '\0'
        if (buf_size > 512) {
            MWR_LOG_ERROR("Password display buffer too large: {} bytes", buf_size);
            continue;
        }
        ps_ptr<char> pass;
        pass.alloc(buf_size);
        char* pass_dst = pass.get();
        if (!pass_dst) {
            MWR_LOG_ERROR("Password display buffer allocation failed");
            continue;
        }
        for (size_t j = 0; j < pwlen; j++) {
            if (offset + dot_len > buf_size - 1) {
                MWR_LOG_ERROR("Buffer overflow in password masking");
                break;
            }
            memcpy(pass_dst + offset, emoji.blueCircle, dot_len);
            offset += dot_len;
        }
        pass_dst[offset] = '\0'; // Zero-terminate the string
        printfln(s_tag.wifi_info, "add credentials: " ANSI_ESC_YELLOW "{} - {}" ANSI_ESC_RESET " [{}:{}]", ssid, pass_dst, __FILENAME__, __LINE__);
    }

    // These options can help when you need ANY kind of wifi connection to get a config file, report errors, etc.
    wifiMulti.setStrictMode(false); // Allow opportunistic connections while maintaining known APs in priority
    printfln(s_tag.wifi_info, ANSI_ESC_GREEN "Connecting WiFi...");

    if (lockWifiOps(pdMS_TO_TICKS(1000))) {
        wifiMulti.run();
        unlockWifiOps();
    } else {
        MWR_LOG_WARN("WiFi operation busy during initial connect");
    }
    int i = 0;
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(1000);
        i++;
        if (i > 10) break; // max 20s
    }
    if (WiFi.status() != WL_CONNECTED) {
        printfln(s_tag.wifi_info, ANSI_ESC_RED "WiFi credentials are not correct");
        return false;
    }
    printfln(s_tag.wifi_info, ANSI_ESC_GREEN "WiFi connected");
    vTaskDelay(1000);
    WiFi.setAutoReconnect(true);
    if (WIFI_TX_POWER >= 2 && WIFI_TX_POWER <= 21) WiFi.setTxPower((wifi_power_t)(WIFI_TX_POWER * 4));
    s_myIP = WiFi.localIP().toString().c_str();

    printfln(s_tag.wifi_info, "connected to " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET ", IP address is " ANSI_ESC_ORANGE "{}" ANSI_ESC_RESET ", Received Signal Strength " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " dB", WiFi.SSID().c_str(), s_myIP.c_get(), WiFi.RSSI());

    return true; // can't connect to any network
}
// ——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void setWiFiCredentials(ps_ptr<char> ssid, ps_ptr<char> password) {
    // Real SSIDs can be as short as 1 char (e.g. the "s3" seen scanning
    // nearby networks earlier) -- this used to require >=5, which silently
    // dropped any such network with no feedback to the caller (web page or
    // on-device UI alike just saw the request go nowhere).
    if (ssid.strlen() == 0) return;

    MWR_LOG_ERROR("ssid {}", ssid.c_get());

    ps_ptr<char> line = "";
    ps_ptr<char> credentials;
    int          i = 0, state = 0;

    if (!lockPreferences(pdMS_TO_TICKS(1000))) {
        printfln(s_tag.wifi_info, ANSI_ESC_RED "Preferences busy; could not save WiFi credentials for: {}", ssid.c_get());
        return;
    }

    for (i = 0; i < kWifiSlotCount; i++) {
        line = pref.getString(wifiPrefKey(i)).c_str();
        if (line.starts_with(ssid.c_get()) && line[ssid.strlen()] == '\t') { // ssid found
            if (i == 0) {
                MWR_LOG_ERROR("password can't changed, is hard coded");
                state = 0;
                goto exit;
            }
            if (password.strlen() == 0) {
                credentials = ""; // delete ssid and password
            } else {                // update password
                credentials = ssid;
                credentials += "\t";
                credentials += password;
            }
            pref.putString(wifiPrefKey(i), credentials.get());
            state = 1;
            goto exit;
        }
    }
    for (i = 1; i < kWifiSlotCount; i++) {
        line.clear();
        line = pref.getString(wifiPrefKey(i)).c_str();
        if (!playerCoreWifiLineIsSavedNetwork(line.c_get())) { // empty or stale-invalid slot
            credentials = ssid;
            credentials += "\t";
            credentials += password;
            pref.putString(wifiPrefKey(i), credentials.get());
            state = 2;
            goto exit;
        }
    }
    state = 3;

exit:
    unlockPreferences();
    if (state == 0) { printfln(s_tag.wifi_info, ANSI_ESC_RED "SSID: {} password can't changed, it is hard coded", ssid.c_get()); }
    if (state == 1) { printfln(s_tag.wifi_info, ANSI_ESC_GREEN "The passord \"{}\" for the SSID: {} has been changed", password.c_get(), ssid.c_get()); }
    if (state == 2) { printfln(s_tag.wifi_info, ANSI_ESC_GREEN "The SSID: {} has been added", ssid.c_get()); }
    if (state == 3) { printfln(s_tag.wifi_info, ANSI_ESC_RED "No more memory to save the credentials for: {}", ssid.c_get()); }
    return;
}

/*****************************************************************************************************************************************************
 *                                                                     A U D I O                                                                     *
 *****************************************************************************************************************************************************/
void connecttohost(ps_ptr<char> host) {
    // Guards against a real crash, not just a nicety: audio.connecttohost()
    // resolves the hostname via lwIP, which assumes the TCP/IP stack is
    // already up. If WiFi never actually connected (esp_wifi_init() failed,
    // or the background lvglNetworkTask never even started -- see
    // setupLvglRuntime()'s history), that stack was never brought up, and
    // the DNS lookup dereferences a null lwIP mailbox instead of failing
    // cleanly (LoadProhibited panic, reproduced by tapping next-station
    // while offline). Fail the same way a real connection failure would.
    if (!WiFi.isConnected()) {
        printfln(s_tag.wifi_info, ANSI_ESC_RED "Cannot connect to host, WiFi is not connected");
        s_f_isWebConnected = false;
        ++s_cthFailCounter;
        return;
    }
    int32_t      idx1, idx2;
    ps_ptr<char> url;
    ps_ptr<char> user;
    ps_ptr<char> pwd;

    dispFooter.updateBitRate(0);
    s_cur_Codec = 0;
    //    if(s_state == RADIO) clearStreamTitle();
    s_icyBitRate = 0;
    s_decoderBitRate = 0;
    s_f_webFailed = false;
    s_f_pauseResume = false;
    s_f_isFSConnected = false;

    idx1 = host.index_of("|", 0);
    if (idx1 == -1) { // no pipe found
        s_f_isWebConnected = audio.connecttohost(host.c_get());

        if (!s_f_isWebConnected) {
            s_cthFailCounter++;
        } else {
            (s_cthFailCounter = 0);
        }
    } else { // pipe found     e.g. http://xxx.com/ext|user|pw
        idx2 = host.index_of("|", idx1 + 1);
        // MWR_LOG_INFO("idx2 = {}", idx2);
        if (idx2 == -1) { // second pipe not found
            s_f_isWebConnected = audio.connecttohost(host.c_get());

            if (!s_f_isWebConnected) {
                s_cthFailCounter++;
            } else {
                (s_cthFailCounter = 0);
            }
        } else {                     // extract url, user, pwd
            url = host.substr(idx1); // extract url
            user = host.substr(idx1 + 1, idx2 - idx1 - 1);
            pwd = host.substr(idx2 + 1);
            printfln(s_tag.new_host, ANSI_ESC_YELLOW "\"{}\"" ANSI_ESC_RESET ", user: " ANSI_ESC_YELLOW "\"{}\"" ANSI_ESC_RESET ", pwd: " ANSI_ESC_YELLOW "\"{}\"", url.c_get(), user.c_get(), pwd.c_get());
            s_f_isWebConnected = audio.connecttohost(url.c_get(), user.c_get(), pwd.c_get());
        }
    }
    if (s_cthFailCounter >= 3) {
        audio.connecttospeech("The last hosts were not connected", "en");
        s_settings.lastconnectedhost.assign("");
    }
}
// Forward-declared (real definition is further down, alongside the rest of
// the USB-storage statics) so every SD-touching entry point can be guarded
// from one place instead of repeating the same check at each call site.
static bool usbStorageBlocksSdAppAccess();

static bool isUserLocalAudioPath(const char* filename) {
    if (!filename || !isAudio(filename)) return false;
    if (startsWith(filename, "/ring/")) return false;
    if (startsWith(filename, "/voice_time/")) return false;
    return true;
}

static void rememberCurrentLocalPlayback(bool persist) {
    if (!s_f_isFSConnected) return;
    if (s_settings.lastconnectedfile.valid()) {
        s_settings.lastconnectedfilepos = audio.getAudioCurrentTime();
        if (persist) updateSettings();
    }
}

void connecttoFS(const char* FS, const char* filename, uint32_t fileStartTime) {
    if (!filename) return;
    // The SD card is exposed as a raw USB block device while mounted/
    // restoring -- letting this function also open it via the filesystem
    // layer at the same time (e.g. an alarm or time announcement firing
    // mid-transfer) is exactly the kind of concurrent access that can
    // corrupt the FAT filesystem. Every local playback path (SD_playFile,
    // the alarm clock, time announcements, playerCorePlaySdFile) funnels
    // through this one function, so guarding here covers all of them.
    if (usbStorageBlocksSdAppAccess()) return;
    s_cloudMusicPlaying = false; // s_f_isFSConnected takes ternary precedence anyway, but keep this honest for anything reading the raw flag
    dispFooter.updateBitRate(0);
    s_icyBitRate = 0;
    s_decoderBitRate = 0;
    s_cur_Codec = 0;
    s_f_webFailed = false;
    s_f_pauseResume = false;
    s_f_isFSConnected = audio.connecttoFS(SD_MMC, filename, fileStartTime);
    s_f_isWebConnected = false;
    if (s_f_isFSConnected && isUserLocalAudioPath(filename)) {
        s_settings.lastconnectedfile.copy_from(filename);
        s_settings.lastconnectedfilepos = fileStartTime;
        s_SD_content.setLastConnectedFile(filename);
        s_cur_AudioFolder = s_SD_content.getLastConnectedFolder();
        s_cur_AudioFileName = s_SD_content.getLastConnectedFileName();
        s_cur_AudioFileNr = s_SD_content.getPosByFileName(s_cur_AudioFileName.c_get());
        if (s_cur_AudioFileNr == -1) s_cur_AudioFileNr = 0;
        updateSettings();
    }
    MWR_LOG_DEBUG("Filesize {}", audio.getFileSize());
    MWR_LOG_DEBUG("FilePos {}", audio.getAudioFilePosition());
}
void stopSong() {
    audio.stopSong();
    s_f_isFSConnected = false;
    s_f_isWebConnected = false;
    if (s_f_playlistEnabled) {
        s_PLS_content.clear();
        s_f_playlistEnabled = false;
        printfln(s_tag.playlist, ANSI_ESC_YELLOW "playlist stopped");
        webSrv.send("SD_playFile=", "playlist stopped");
    }
    s_f_pauseResume = false;
    s_f_playlistNextFile = false;
    s_playlistPath.reset();
}

// -----------------------------------------------------------------------------
// PlayerService core adapter
// -----------------------------------------------------------------------------
// These functions are the only MiniWebRadio globals the new LVGL application is
// allowed to touch.  Keep them here, beside the existing audio helpers, so the
// decoder task and its connection bookkeeping remain owned by upstream code.
bool playerCorePlayRadioUrl(const char* url) {
    if (!url || !url[0]) return false;
    s_cloudMusicPlaying = false;
    ps_ptr<char> host;
    host.assign(url);
    s_stationURL = host;
    s_stationName_air = host;
    s_cur_station = 0;
    connecttohost(host);
    return s_f_isWebConnected;
}

// Cloud Music phase 4: plays a resolved direct-CDN URL through the exact
// same connecttohost() path as internet radio -- no separate decoder (see
// docs spec's "不得为在线音乐再创建一套独立 I2S、DMA 或音频解码器").
// s_cloudMusicPlaying is what lets playerCoreReadSnapshot() report
// PlayerSource::CloudMusic instead of ::Radio afterwards. Unlike
// playerCorePlayRadioUrl(), s_stationName_air is deliberately left alone --
// the caller (PlayerService::playCloudUrl()) sets title/artist straight
// from the gateway's resolve response, no need to wait for ICY metadata
// text that a plain CDN file URL wouldn't send anyway.
bool playerCorePlayCloudUrl(const char* url) {
    if (!url || !url[0]) return false;
    s_cloudMusicPlaying = true;
    ps_ptr<char> host;
    host.assign(url);
    connecttohost(host);
    return s_f_isWebConnected;
}

static bool playerCorePlayStationNumber(uint16_t stationNumber) {
    if (stationNumber == 0 || stationNumber > staMgnt.getSumStations()) return false;
    const char* url = staMgnt.getStationUrl(stationNumber);
    if (!url || !url[0] || !strcmp(url, "unknown")) return false;
    const char* name = staMgnt.getStationName(stationNumber);
    s_cloudMusicPlaying = false;
    s_cur_station = stationNumber;
    s_stationURL = url;
    s_stationName_air = name && name[0] ? name : url;
    s_homepage = "";
    s_streamTitle = "";
    s_icyDescription = "";
    s_f_newStreamTitle = true;
    s_f_newIcyDescription = true;
    connecttohost(s_stationURL);
    printfln(s_tag.action, "LVGL switch to station " ANSI_ESC_CYAN "{}", stationNumber);
    if (s_f_isWebConnected) {
        s_memLogRadioAtSec = s_totalRuntime + 5; // see loopLvglRuntime()'s s_f_1sec block
        updateSettings();
    }
    return s_f_isWebConnected;
}

uint16_t playerCoreRadioStationCount() { return staMgnt.getSumStations(); }

bool playerCoreRadioStation(uint16_t index, RadioStationItem* item) {
    if (!item || index == 0 || index > staMgnt.getSumStations()) return false;
    item->index = index;
    item->favorite = staMgnt.getStationFav(index) == '*';
    strlcpy(item->country, staMgnt.getStationCountry(index), sizeof(item->country));
    strlcpy(item->name, staMgnt.getStationName(index), sizeof(item->name));
    strlcpy(item->url, staMgnt.getStationUrl(index), sizeof(item->url));
    return true;
}

uint16_t playerCoreCurrentRadioStationNumber() {
    const uint16_t count = staMgnt.getSumStations();
    if (!count) return 0;
    return (s_cur_station >= 1 && s_cur_station <= count) ? s_cur_station : 1;
}

const char* playerCoreLastLocalFilePath() {
    return s_settings.lastconnectedfile.valid() ? s_settings.lastconnectedfile.c_get() : "";
}

uint32_t playerCoreLastLocalFilePosition() { return s_settings.lastconnectedfilepos; }

bool playerCorePlayRadioStation(uint16_t index) { return playerCorePlayStationNumber(index); }

// Declared here (used by playerCorePlaySdFile() below) rather than only at
// its original spot further down near the rest of the USB-storage statics --
// that original position was after this function, which doesn't compile.
static volatile UsbStorageState s_usbStorageState = MWR_USB_MSC_SUPPORTED ? UsbStorageState::Idle : UsbStorageState::Unsupported;

// —— 播放事件记录（Phase 1）的状态与前置声明 ——
//
// 变量定义在这里、函数实现在下面靠近曲库那批静态变量的地方（实现要用到
// s_localTracks，那个还没声明）。playerCorePlaySdFile() 就在下面几行，
// 所以两者都得先声明。
static uint32_t s_playingLocalId = 0;    // 当前正在播放的本地曲目；0 = 没有
static bool     s_playCompleted = false; // 当前这首是否已播到结尾
static void libraryLogEvent(uint32_t localId, uint8_t type);
static void libraryFinishCurrentTrack();

bool playerCorePlaySdFile(const char* path, uint32_t positionSeconds) {
    if (s_usbStorageState != UsbStorageState::Idle) return false;
    if (!path || !path[0]) return false;
    // 换歌前先结算上一首：没播完就算跳过。
    libraryFinishCurrentTrack();
    connecttoFS("SD_MMC", path, positionSeconds);
    if (s_f_isFSConnected) {
        s_memLogLocalAtSec = s_totalRuntime + 5; // see loopLvglRuntime()'s s_f_1sec block
        s_playingLocalId = libraryHashPath(path);
        s_playCompleted = false;
        libraryLogEvent(s_playingLocalId, kEventPlayStarted);
    }
    return s_f_isFSConnected;
}

void playerCoreStop() {
    rememberCurrentLocalPlayback(true);
    libraryFinishCurrentTrack();
    stopSong();
}

// 找到"暂停再播放后频谱卡住约一分钟"这个 bug 的真正原因了：
// Audio::pauseResume() 的返回值语义是"true=现在是暂停状态，false=现在是
// 播放状态"（库里那条注释自己写的），但下面这行原来是 `if (accepted)
// s_f_pauseResume = !audio.isRunning();`——只有在"刚暂停"（accepted为
// true）时才会更新 s_f_pauseResume，"刚恢复播放"（accepted为false）时这
// 个判断整个跳过，s_f_pauseResume 就一直卡在 true（暂停）不会被改回
// false，即使 audio.isRunning() 已经立刻正确变回了 true。playerCore
// ReadSnapshot() 判断 transport 时 `else if (s_f_pauseResume)
// PlayerTransport::Paused` 排在 `audio.isRunning()` 判断前面，于是即使音
// 频已经在正常解码、频谱数据也在正常更新，UI 这边读到的 transport 一直
// 还是 Paused，频谱动画因为"没有在播放"就不画——这才是"卡住"的真相，不
// 是频谱计算本身卡住，是这个状态标志位没跟着同步。之所以"大概一分钟后自
// 己恢复"，是因为别处某个跟这个 bug 无关的路径最终把 s_f_pauseResume 重
// 置了。改成每次切换后都用 audio.isRunning() 无条件同步，不再依赖这个歧
// 义返回值。
static bool audioPauseResumeAndUpdateState() {
    const bool accepted = audio.pauseResume();
    s_f_pauseResume = !audio.isRunning();
    return accepted;
}

bool playerCoreTogglePause() {
    audioPauseResumeAndUpdateState();
    return s_f_pauseResume;
}

void playerCoreSetVolume(uint8_t volume) {
    s_f_mute = false;
    setVolume(min(volume, s_volume.volumeSteps));
    muteChanged(false);
}

void playerCoreSetMuted(bool muted) {
    s_f_mute = muted;
    muteChanged(muted);
}

AudioToneSettings playerCoreToneSettings() {
    AudioToneSettings settings;
    settings.low = static_cast<int8_t>(std::clamp(static_cast<int>(s_tone.LP), -12, 12));
    settings.mid = static_cast<int8_t>(std::clamp(static_cast<int>(s_tone.BP), -12, 12));
    settings.high = static_cast<int8_t>(std::clamp(static_cast<int>(s_tone.HP), -12, 12));
    settings.balance = static_cast<int8_t>(std::clamp(static_cast<int>(s_tone.BAL), -16, 16));
    return settings;
}

void playerCoreSetToneSettings(const AudioToneSettings& settings, bool persist) {
    s_tone.LP = std::clamp(static_cast<int>(settings.low), -12, 12);
    s_tone.BP = std::clamp(static_cast<int>(settings.mid), -12, 12);
    s_tone.HP = std::clamp(static_cast<int>(settings.high), -12, 12);
    s_tone.BAL = std::clamp(static_cast<int>(settings.balance), -16, 16);
    setI2STone();
    if (persist) updateSettings();
}

void playerCoreSaveSettings() { updateSettings(); }

AudioOutputPolicy playerCoreOutputPolicy() { return s_audioOutputPolicy; }

void playerCoreSetOutputPolicy(AudioOutputPolicy policy, bool persist) {
    s_audioOutputPolicy = policy;
    if (policy == AudioOutputPolicy::Fixed44100) audio.setOutputSampleRate(Audio::SR_44100);
    else if (policy == AudioOutputPolicy::Fixed48000) audio.setOutputSampleRate(Audio::SR_48000);
    else audio.setOutputSampleRate(Audio::SR_ORIGIN);
    if (persist) updateSettings();
}

// Legacy quick-EQ compatibility path; full audio settings keep balance too.
void playerCoreSetTone(int8_t low, int8_t mid, int8_t high) {
    AudioToneSettings settings = playerCoreToneSettings();
    settings.low = low;
    settings.mid = mid;
    settings.high = high;
    playerCoreSetToneSettings(settings, false);
}

void playerCoreNextStation() {
    if (!staMgnt.getSumStations()) return;
    uint16_t next = s_cur_station + 1;
    if (next > staMgnt.getSumStations()) next = 1;
    playerCorePlayStationNumber(next);
}

void playerCorePreviousStation() {
    if (!staMgnt.getSumStations()) return;
    uint16_t prev = s_cur_station > 1 ? s_cur_station - 1 : staMgnt.getSumStations();
    playerCorePlayStationNumber(prev);
}

// Local weather: no GPS on this board, so location comes from IP geolocation
// (ip-api.com, plain HTTP, no key) and the reading itself from Open-Meteo
// (HTTPS, no key). Fetched on a background task -- both are network calls
// that must never block the LVGL/audio loop -- and cached here behind a
// mutex since playerCoreReadSnapshot() (LVGL-tick thread) and weatherTask
// (its own thread) touch it from different cores.
void setRTC(ps_ptr<char> TZString); // defined below; weather fetch also derives+applies the real timezone

struct WeatherState {
    bool valid = false;
    int16_t tempC = 0;
    char desc[8] = "";
    int8_t iconCategory = -1; // 0 clear,1 partly cloudy,2 cloudy,3 fog,4 rain,5 snow,6 thunder
};
static WeatherState s_weather;
static SemaphoreHandle_t s_weatherMutex = nullptr;
static bool s_weatherTzApplied = false; // apply the IP-derived timezone once, not on every 30-min refetch

// Finds "<key>":<number> in a JSON blob and returns the number -- hand-
// counting prefix lengths (`pos + 18`, `pos + 16`, ...) was error-prone and
// silently produced wrong values (e.g. skipped the leading digit) instead of
// a parse failure, so this always measures the needle itself.
static bool jsonNumber(const String& body, const char* key, double* outValue, bool useLast = true) {
    const String needle = String("\"") + key + "\":";
    const int pos = useLast ? body.lastIndexOf(needle) : body.indexOf(needle);
    if (pos < 0) return false;
    *outValue = atof(body.c_str() + pos + needle.length());
    return true;
}

static const char* weatherCodeToLabel(int code) {
    // WMO weather codes, as returned by Open-Meteo's `weather_code` field.
    if (code == 0) return "晴";
    if (code <= 2) return "少云";
    if (code == 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 85 && code <= 86) return "阵雪";
    if (code >= 95) return "雷雨";
    return "--";
}

static int8_t weatherCodeToIcon(int code) {
    if (code == 0) return 0;
    if (code <= 3) return code == 1 ? 1 : 2; // 1-2 partly cloudy, 3 overcast
    if (code == 45 || code == 48) return 3;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 4;
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return 5;
    if (code >= 95) return 6;
    return -1;
}

static bool fetchWeatherOnce() {
    if (!WiFi.isConnected()) return false;

    WiFiClient geoClient;
    HTTPClient geoHttp;
    double lat = 0.0, lon = 0.0;
    bool haveLocation = false;
    if (geoHttp.begin(geoClient, "http://ip-api.com/json/?fields=lat,lon,status")) {
        geoHttp.setTimeout(6000);
        if (geoHttp.GET() == HTTP_CODE_OK) {
            const String body = geoHttp.getString();
            haveLocation = jsonNumber(body, "lat", &lat, false) && jsonNumber(body, "lon", &lon, false);
        }
        geoHttp.end();
    }
    if (!haveLocation) return false;

    char url[176];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&timezone=auto",
             lat, lon);

    WiFiClientSecure weatherClient;
    weatherClient.setInsecure(); // no cert pinning for a public no-key weather API
    HTTPClient weatherHttp;
    bool ok = false;
    if (weatherHttp.begin(weatherClient, url)) {
        weatherHttp.setTimeout(8000);
        if (weatherHttp.GET() == HTTP_CODE_OK) {
            const String body = weatherHttp.getString();
            // Open-Meteo's response has a "current_units" block ahead of
            // "current" with the SAME key names but string unit values
            // ("°C") -- useLast=true (lastIndexOf) lands on the real
            // "current" block instead of the units block.
            double temp = 0.0, code = 0.0, utcOffsetSeconds = 0.0;
            if (jsonNumber(body, "temperature_2m", &temp)) {
                const bool haveCode = jsonNumber(body, "weather_code", &code);
                const int codeInt = haveCode ? static_cast<int>(code) : -1;
                if (s_weatherMutex && xSemaphoreTake(s_weatherMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    s_weather.tempC = static_cast<int16_t>(lroundf(temp));
                    strlcpy(s_weather.desc, weatherCodeToLabel(codeInt), sizeof(s_weather.desc));
                    s_weather.iconCategory = weatherCodeToIcon(codeInt);
                    s_weather.valid = true;
                    xSemaphoreGive(s_weatherMutex);
                }
                ok = true;
            }
            // Same location lookup also fixes the clock: the board has no
            // way to know its own timezone otherwise, and defaulted to a
            // hardcoded Europe/Berlin rule (see s_TZString's initializer).
            // Fixed UTC offset, no DST rule -- good enough for most zones
            // and exactly right for ones (like Japan) that don't use DST.
            if (!s_weatherTzApplied && jsonNumber(body, "utc_offset_seconds", &utcOffsetSeconds, false)) {
                const long offsetHours = lroundf(static_cast<float>(utcOffsetSeconds) / 3600.0f);
                char tz[16];
                snprintf(tz, sizeof(tz), "UTC%+ld", -offsetHours);
                setRTC(tz);
                s_weatherTzApplied = true;
            }
        }
        weatherHttp.end();
    }
    return ok;
}

static void weatherTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(4000)); // let WiFi settle before the first fetch
    for (;;) {
        const bool ok = fetchWeatherOnce();
        printf("[WEATHER] fetch %s temp=%d desc=%s icon=%d\n", ok ? "ok" : "failed", s_weather.tempC, s_weather.desc,
               s_weather.iconCategory);
        vTaskDelay(pdMS_TO_TICKS(30UL * 60UL * 1000UL)); // refresh every 30 min
    }
}

// ---- Local music library ---------------------------------------------
// Recursive SD scan + a minimal, self-contained ID3v2.2/2.3/2.4 reader.
// Deliberately independent of the Audio class's own ID3 parsing, which
// only runs while a file is actually being decoded -- not useful for
// batch-scanning hundreds of files on disk without playing each one.
// 本地曲库上限。
//
// 2026-09-07：300 -> 2000（本地音乐库 2.0 Phase 1，见
// docs/LOCAL_LIBRARY_V2_AUDIT.md §2）。LocalTrackItem 约 322 字节、走 ps_calloc
// 分配在 PSRAM：
//     300 首 =  97 KB
//    2000 首 = 644 KB
//    5000 首 = 1.6 MB   <- PSRAM 空闲约 2.58MB，还要留给 LVGL 的 96KB 池和
//                          封面解码缓冲，5000 太紧，第一版不取
constexpr uint16_t kMaxLocalTracks = 2000;
static constexpr const char* kLocalMusicDir = "/Music";
// 本地音乐库 2.0：内存里的记录换成 TrackRecord。它刻意保持了与 LocalTrackItem
// 同名同序的前几个字段（path/title/artist/album/hasArt），所以下面 20 多处
// s_localTracks[i].path 之类的代码一行都不用改。
static TrackRecord* s_localTracks = nullptr;
static uint16_t s_localTrackCount = 0;
static volatile bool s_localLibraryScanning = false;
// 实际分配到的容量。分配失败会逐级减半，所以它可能小于 kMaxLocalTracks——
// 扫描的边界检查必须用它，不能用编译期常量。
static uint16_t s_localTrackCapacity = 0;

// 扫描期的对账上下文。scanMusicDir() 是递归的，把这些做成文件级静态比一路传参
// 干净。只在 localMusicScanTask() 里读写，不跨任务。
static uint8_t* s_scanSeenBits = nullptr;   // 大小见 libraryStoreSeenBytes()
// 开机时从 tracks.idx **读回**了多少条。
// 这是区分"索引真的持久化了"和"每次开机都重扫一遍"的唯一判据——两种情况下
// 最终的 tracks 数量是一样的，光看总数分不出来。
static uint16_t s_libLoadedAtBoot = 0;
// 扫描时算出来的空间评估结果，存下来供周期打印用。
// **不在周期打印里直接调 SD_MMC.usedBytes()** —— 那个在大容量 FAT32 上要遍历
// 分配表，几百毫秒起步，每 10 秒来一次会和音频抢 SD。
static CleanerPlan s_cleanerPlan{};

// —— Phase 3：候选发现 ——
//
// client_id 存 NVS，**不进仓库**（审计 R12）。没配置时 provider 的
// available() 为 false，整条发现链路直接跳过，不影响其它功能。
static constexpr const char* kJamendoClientIdPrefKey = "jamendo_id";

// 构建期默认值。真实的 client_id 放在 src/jamendo_secret.h —— **gitignored**，
// 和 platformio_override.ini 里的 WiFi 密码同样处理（审计 R12）。
// 代码里**绝不硬编码**任何凭据；文件不存在时留空，available() 为 false，
// 整条发现链路静默跳过。
//
// 用独立头文件而不是 platformio.ini 的 -D：改 [common].build_flags 会改变每个
// 源文件的编译命令，触发 45 分钟全量重编；这个头文件只被本文件包含。
#if defined(__has_include)
#if __has_include("jamendo_secret.h")
#include "jamendo_secret.h"
#endif
#endif
#ifndef JAMENDO_CLIENT_ID
#define JAMENDO_CLIENT_ID ""
#endif
static JamendoProvider s_jamendo;

// 解析器自检结果。**不联网、不需要 client_id** —— 喂一段真实格式的样本进去，
// 验证 JSON 扫描逻辑本身。这样 Phase 3 的核心（解析）能独立于网络和凭据被验证，
// 不用等申请 client_id，也不受网络波动影响。
static uint8_t s_jamendoSelfTestParsed = 0xFF;  // 0xFF = 还没跑

// 联网后的一次性真实拉取（Phase 3 验收）。只发现候选，**不下载任何文件**。
static volatile bool s_jamendoProbeDone = false;
static volatile uint8_t s_jamendoProbeCount = 0;
static char s_jamendoProbeErr[64] = "";

// Phase 4：单首下载测试的结果，供周期打印。
static volatile bool s_dlTestDone = false;
static char s_dlTestResult[96] = "";

// —— R6 验证：下载写 SD 与播放读 SD 的争用 ——
//
// 需要一个**客观指标**，"听着好像没卡"不算数。用 audio.inBufferFilled()：
// 解码输入缓冲的水位。下载期间如果它被抽干趋近 0，就是真的在饿着解码器。
//
// 采样挂在 loopLvglRuntime() 的 100ms 节拍里，而不是在下载任务里——
// 下载是阻塞调用，它自己没法给自己采样。
static volatile bool     s_r6Sampling = false;
static volatile uint32_t s_r6MinFilled = 0xFFFFFFFFu;
static volatile uint64_t s_r6SumFilled = 0;
static volatile uint32_t s_r6Samples = 0;
static volatile bool     s_r6Starved = false;   // 水位掉到过 0
// ⚠️ 只有 min 和 starv 是不够的：下载跑 180 秒，期间歌曲可能播完切下一首，
// 切歌瞬间缓冲本来就会归零。第一轮实测拿到 min=0/starv=1，但**分不出**那是
// 下载饿着了解码器还是正常切歌。
// 区分靠形态：切歌是一小段连续的 0；真争用是反复触底、低水位样本占比高。
static volatile uint32_t s_r6ZeroCount = 0;        // 水位为 0 的样本数
static volatile uint32_t s_r6LowCount = 0;         // 水位低于容量 25% 的样本数
static volatile uint32_t s_r6ConsecZero = 0;       // 当前连续 0 的长度
static volatile uint32_t s_r6MaxConsecZero = 0;    // 最长的一段连续 0
static volatile uint32_t s_r6Capacity = 0;         // filled+free，用于算百分比
// DMA 可用内部 RAM 的最低水位。这是"边播边下会不会因为内存不够而崩"的
// 直接证据，之前只有一个来路不清的"约 7KB"，且分不清是总量还是最大块。
// 两个都记：总量说明还剩多少，最大连续块说明还能不能满足一次较大的 DMA 分配。
static volatile uint32_t s_r6DmaFreeMin = 0xFFFFFFFFu;
static volatile uint32_t s_r6DmaBlockMin = 0xFFFFFFFFu;
static volatile bool     s_r6Done = false;
static char s_r6Result[320] = "";   // 字段多，短了会被 snprintf 截断

// 供 DownloadManager 做自适应节流。返回解码输入缓冲水位百分比。
// 没在播放就返回 kAudioFillUnknown —— 此时下载全速跑，没有理由让步。
static uint8_t audioFillPercentForDownload() {
    if (!audio.isRunning()) return kAudioFillUnknown;
    const uint32_t filled = audio.inBufferFilled();
    const uint32_t cap = filled + audio.inBufferFree();
    if (!cap) return kAudioFillUnknown;
    return static_cast<uint8_t>(filled * 100ull / cap);
}

// 是否有**网络供给**的音频在播（电台 / 云音乐）。
// s_f_isWebConnected 正是这个语义：本地文件播放走的是 s_f_isFSConnected。
static bool netAudioActiveForDownload() {
    return s_f_isWebConnected && audio.isRunning();
}

static void r6StatsReset() {
    s_r6MinFilled = 0xFFFFFFFFu;
    s_r6SumFilled = 0;
    s_r6Samples = 0;
    s_r6Starved = false;
    s_r6ZeroCount = 0;
    s_r6LowCount = 0;
    s_r6ConsecZero = 0;
    s_r6MaxConsecZero = 0;
    s_r6DmaFreeMin = 0xFFFFFFFFu;
    s_r6DmaBlockMin = 0xFFFFFFFFu;
    s_r6Sampling = true;
}

// 返回平均水位；min 通过出参给。样本为 0 时两者都返回 0。
struct R6Stats {
    uint32_t avg = 0, min = 0, samples = 0;
    uint32_t zeroCount = 0, lowCount = 0, maxConsecZero = 0;
    uint32_t dmaFreeMin = 0, dmaBlockMin = 0;
};

static R6Stats r6StatsSnapshot() {
    R6Stats r;
    r.samples = s_r6Samples;
    r.min = r.samples ? s_r6MinFilled : 0;
    r.avg = r.samples ? static_cast<uint32_t>(s_r6SumFilled / r.samples) : 0;
    r.zeroCount = s_r6ZeroCount;
    r.lowCount = s_r6LowCount;
    r.maxConsecZero = s_r6MaxConsecZero;
    r.dmaFreeMin = r.samples ? s_r6DmaFreeMin : 0;
    r.dmaBlockMin = r.samples ? s_r6DmaBlockMin : 0;
    return r;
}

// —— Phase 5：每日发现同步 ——
//
// 状态存 NVS。daily_sync 模块本身不碰 NVS —— pref 的加锁在本文件里，
// 让下层模块去拿这个锁会把分层搞乱，所以状态通过结构体进出。
static constexpr const char* kSyncDayPrefKey = "sync_day";
// ⚠️ key 从 "sync_off" 换成 "sync_off2"：2026-09-05 把配额维度从语言改成
// 风格，四个 offset 的**含义变了**。沿用旧 key 会把"英文档的 offset"当成
// "hiphop 档的 offset"接着用 —— 不会崩，但会静默地从一个毫无关系的位置开始翻。
static constexpr const char* kSyncOffPrefKey = "sync_off2";  // 四个 offset 打包成一个 u64

// 每轮下载几首。见 dailySyncTask 里的说明。
constexpr uint8_t kDailySyncTracksPerDay = 10;

// 调试开关：置 true 时忽略 lastRunDay，每次开机跑一轮。
//
// ⚠️ **生产状态必须是 false。** 它存在的理由是：每日那道闸会让每一轮真机
// 验证都白跑 —— 2026-09-05 就因为它，一轮 12 分钟的抓取里一行 SYNC 都没有。
// 2026-09-05 端到端验证通过后已改回 false。
constexpr bool kDailySyncForceOnBoot = false;

static DailySyncState s_syncState;
static DailySyncStats s_syncStats;
// 推迟后多久重试。15 分钟：够让用户听完几首歌换个事做，
// 又不至于频繁地去打扰正在听的电台。
constexpr uint32_t kSyncRetryCooldownSec = 900;
static uint32_t s_syncRetryAtSec = 0;

static volatile bool s_syncRunning = false;
static volatile bool s_syncDone = false;

static void dailySyncLoadState() {
    if (!lockPreferences()) return;
    s_syncState.lastRunDay = pref.getUInt(kSyncDayPrefKey, 0);
    const uint64_t packed = pref.getULong64(kSyncOffPrefKey, 0);
    s_syncState.offsetPop  = static_cast<uint16_t>(packed & 0xFFFF);
    s_syncState.offsetRock  = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    s_syncState.offsetHiphop  = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
    s_syncState.offsetAny = static_cast<uint16_t>((packed >> 48) & 0xFFFF);
    unlockPreferences();
}

static void dailySyncSaveState() {
    if (!lockPreferences()) return;
    pref.putUInt(kSyncDayPrefKey, s_syncState.lastRunDay);
    const uint64_t packed = static_cast<uint64_t>(s_syncState.offsetPop)
                          | (static_cast<uint64_t>(s_syncState.offsetRock) << 16)
                          | (static_cast<uint64_t>(s_syncState.offsetHiphop) << 32)
                          | (static_cast<uint64_t>(s_syncState.offsetAny) << 48);
    pref.putULong64(kSyncOffPrefKey, packed);
    unlockPreferences();
}

// 一轮同步要跑几十分钟（实测单首 150~210 秒 × 10 首）。
// 必须独立任务：放主循环会把 UI 卡死几十分钟。
static void dailySyncTask(void*) {
    s_syncRunning = true;
    const uint32_t nowEpoch = s_f_rtc ? static_cast<uint32_t>(time(nullptr)) : 0;

    DailySyncConfig cfg;
    // 实测（2026-09-05）：单首 160~220 秒，3 首共 548 秒。
    // 10 首约 30 分钟 —— 后台任务，可以接受。
    cfg.tracksPerDay = kDailySyncTracksPerDay;
    dailySyncRun(s_jamendo, s_localTracks, &s_localTrackCount, kMaxLocalTracks,
                 nowEpoch, 0, cfg, &s_syncState, &s_syncStats);

    dailySyncSaveState();

    // 索引存盘放在整轮结束后**一次性**做，不是每下一首存一次：
    // 2000 条 × 384B ≈ 750KB，每首存一次会把这次同步变成 10 次大写盘。
    if (s_syncStats.indexDirty) {
        if (libraryStoreSave(s_localTracks, s_localTrackCount)) {
            printf("[SYNC] index saved (%u tracks)\n", s_localTrackCount);
        } else {
            printf("[SYNC] index save FAILED\n");
        }
    }

    s_syncRunning = false;
    // "因为电台在播而推迟"不是跑完了 —— 安排冷却后重试，否则用户听一下午
    // 电台就等于当天不更新曲库。dailySyncRun 在这种情况下也不会记 lastRunDay。
    if (s_syncStats.deferredNetAudio) {
        s_syncRetryAtSec = s_totalRuntime + kSyncRetryCooldownSec;
        s_syncDone = false;
    } else {
        s_syncDone = true;
    }
    vTaskDelete(nullptr);
}

// 联网后拉一次真实候选，确认整条链路（HTTPS → JSON → RemoteTrack）通。
//
// 单开任务而不是在 loop 里做：HTTPS 握手会阻塞几百毫秒到几秒，放主循环会卡 UI。
// 沿用 weatherTask 的模式（core 0、优先级 1、10KB 栈——TLS 握手吃栈）。
static void jamendoProbeTask(void*) {
    // RemoteTrack 约 512B，5 条 2.5KB。放 PSRAM 而不是任务栈：栈要留给 TLS。
    RemoteTrack* tracks = static_cast<RemoteTrack*>(ps_calloc(5, sizeof(RemoteTrack)));
    if (tracks) {
        DiscoveryRequest req;
        req.tags = nullptr;              // 不限风格，只验链路通不通
        req.limit = 5;
        req.offset = 0;
        req.order = "popularity_month";
        // ⚠️ Jamendo 会**间歇性**返回空数组，同一个 URL 有时 5 条有时 0 条。
        // 已知至少两种形态（响应体分别是 189 和 105 字节，靠 [JAMENDO][RAW] 抓到）。
        // 这不是可以指望修掉的东西，只能扛：重试几次。
        // Phase 5 每日同步同理——一次拉空不能当成"今天没歌"。
        uint8_t n = 0;
        for (uint8_t attempt = 0; attempt < 3; ++attempt) {
            if (attempt) {
                printf("[JAMENDO] retry %u after empty result\n", attempt);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            n = s_jamendo.fetchCandidates(req, tracks, 5);
            if (n) break;
        }
        s_jamendoProbeCount = n;
        strlcpy(s_jamendoProbeErr, s_jamendo.lastError(), sizeof(s_jamendoProbeErr));
        for (uint8_t i = 0; i < n; ++i) {
            printf("[JAMENDO][PROBE] %u id=%s dur=%lus ~%luKB \"%s\" - \"%s\"\n",
                   i + 1, tracks[i].providerTrackId,
                   static_cast<unsigned long>(tracks[i].durationSec),
                   static_cast<unsigned long>(tracks[i].sizeHintBytes / 1024),
                   tracks[i].title, tracks[i].artist);
        }
        // —— Phase 4：单首下载测试（方案 §32 要求先手动验证单首闭环）——
        //
        // 只下第一条、只下一次、只在**没有播放**时下。
        // 这是受控实验不是自动同步：自动每日下载是 Phase 5 的事。
        //
        // ⚠️ "没在播放才下"是 R6 的第一道防线。Phase 1 已经因为在播放路径里写 SD
        // 把本地和电台都搞挂过一次（见 DEV_LOG §10），这次从一开始就避开。
        int8_t okIndex = -1;   // 哪一条下成功了，R6 阶段复用它

        // ⚠️ **Phase 4 自测已完成，默认关闭** —— 和 kRunR6Experiment 同一个理由，
        // 上一轮只关了 R6，漏了这个：
        //   1. 它每次开机下一首 3.4MB（约 100 秒），probe_done 要等它跑完才置位，
        //      **实测把每日同步的启动推迟了 2 分钟**；
        //   2. 它直接调 downloadToFile，同样绕过 daily_sync 的电台让路策略。
        // 需要重新验证单首下载闭环时再打开。
        constexpr bool kRunDownloadSelfTest = false;
        if (kRunDownloadSelfTest && n > 0 && !audio.isRunning()) {
            // ⚠️ **audiodownload_allowed=true 不保证真的下得下来。**
            // 实测 2026-09-04：同一批候选里 id=2034080 的下载地址恒定返回
            // HTTP 500（PC 直连 curl 同样 500，排除了板子和 TLS 的嫌疑），
            // 而同批的 1593988 / 1932670 都正常 206。API 层的许可标志和
            // 存储层的可用性是两回事。
            //
            // 所以这里往下试，不死磕第一条。Phase 5 的每日同步同理：
            // 单曲失败要跳过并记账，不能卡住整轮，也不能无限重试。
            constexpr uint8_t kMaxAttempts = 3;
            const uint8_t attempts = n < kMaxAttempts ? n : kMaxAttempts;
            for (uint8_t i = 0; i < attempts; ++i) {
                char finalPath[160];
                snprintf(finalPath, sizeof(finalPath), "/music/tracks/jamendo_%s.mp3",
                         tracks[i].providerTrackId);
                DownloadStats st{};
                const DownloadResult r = downloadToFile(tracks[i].audioUrl, finalPath, nullptr, &st);
                snprintf(s_dlTestResult, sizeof(s_dlTestResult),
                         "try%u/%u %s %luKB/%luKB %lums http=%d thr=%lux",
                         i + 1, attempts, downloadResultName(r),
                         static_cast<unsigned long>(st.bytesWritten / 1024),
                         static_cast<unsigned long>(st.expectedBytes / 1024),
                         static_cast<unsigned long>(st.elapsedMs), st.httpCode,
                         static_cast<unsigned long>(st.throttleEvents));
                printf("[DL][TEST] %s -> %s\n", s_dlTestResult, finalPath);
                if (r == DownloadResult::Ok) { okIndex = static_cast<int8_t>(i); break; }
            }
        } else if (!kRunDownloadSelfTest) {
            strlcpy(s_dlTestResult, "disabled (self-test done)", sizeof(s_dlTestResult));
        } else if (n > 0) {
            strlcpy(s_dlTestResult, "skipped (audio playing)", sizeof(s_dlTestResult));
        } else {
            // 没候选就没得下。写清楚原因，否则 [DL][STATE] 是空行，
            // “没结果”和“没运行”分不出来。
            strlcpy(s_dlTestResult, "no candidates", sizeof(s_dlTestResult));
        }
        s_dlTestDone = true;

        // —— R6 验证：播放中再下同一首 ——
        //
        // 为什么复用同一首而不是换一首：**只有同样大小、同一个服务器，
        // 和空闲时那次的对比才成立。** 换一首的话，时间差可能来自文件大小
        // 或 CDN 节点，说明不了争用。
        //
        // 这一段需要人配合：等用户在板子上开始播放本地音乐。等不到就跳过，
        // 不阻塞别的东西。
        //
        // ⚠️ 这一段**不依赖上面那次空闲下载**。第一版把它挂在 okIndex >= 0 上，
        // 结果用户按要求提前开始播放，空闲下载被 !audio.isRunning() 跳过，
        // 连带整个 R6 也跳过了 —— 前置条件互相矛盾。
        // R6 要回答的是"下载会不会饿着解码器"，只需要"播放中不下载"和
        // "播放中下载"两组对比，空闲下载与它无关。
        // ⚠️ **R6 实验已完成，默认关闭。**
        // 2026-09-05 实测发现它现在是纯负担：
        //   1. probe_done 要等它整个跑完才置位，**卡住每日同步 10 分钟**；
        //   2. 它直接调 downloadToFile，**绕过 daily_sync 里的电台让路策略** ——
        //      也就是说它会在用户听电台时下载，正是要避免的事。
        // 需要重新做争用测量时再打开。
        constexpr bool kRunR6Experiment = false;
        if (kRunR6Experiment && n > 0) {
            constexpr uint32_t kWaitPlaybackMs = 300000;   // 5 分钟
            const uint32_t waitUntil = millis() + kWaitPlaybackMs;
            while (!audio.isRunning() && millis() < waitUntil) vTaskDelay(pdMS_TO_TICKS(500));

            if (!audio.isRunning()) {
                strlcpy(s_r6Result, "skipped: no playback started within 5min", sizeof(s_r6Result));
            } else {
                // ⚠️ 先等 20 秒再开始测基线。
                // 上一轮电台实测踩到的坑：起播后缓冲还在往上填，15 秒基线量到
                // avg=16%，而后面 190 秒的下载窗口量到的是稳态 avg=46%，
                // 于是出现"下载期间水位反而更高"的假象 —— 那不是下载改善了
                // 缓冲，是基线取早了。两段必须都在稳态才有可比性。
                vTaskDelay(pdMS_TO_TICKS(20000));

                // 基线：只播放、不下载，15 秒
                r6StatsReset();
                vTaskDelay(pdMS_TO_TICKS(15000));
                const R6Stats base = r6StatsSnapshot();

                // 边播边下。优先用空闲那轮已验证能下的那条；没有的话就现挑，
                // 同样最多试 3 条（坏曲目是常态，见 download_manager.h）。
                r6StatsReset();
                DownloadStats st{};
                DownloadResult r = DownloadResult::HttpError;
                const uint8_t first = (okIndex >= 0) ? static_cast<uint8_t>(okIndex) : 0;
                for (uint8_t k = 0; k < n; ++k) {
                    const uint8_t i = (first + k) % n;
                    r = downloadToFile(tracks[i].audioUrl, "/music/tmp/r6_probe.bin", nullptr, &st);
                    if (r == DownloadResult::Ok) break;
                    if (k >= 2) break;   // 别把实验拖成无限重试
                }
                const R6Stats dl = r6StatsSnapshot();
                s_r6Sampling = false;
                SD_MMC.remove("/music/tmp/r6_probe.bin");   // 只是探针，不留

                // 播放中途被用户停掉的话，样本数会很少，结论不可信 —— 说出来，
                // 而不是让一个基于十几个样本的均值看起来像结论。
                // 基线 15 秒 @100ms ≈ 150 个样本；下载那段更长，只会更多。
                const bool trustworthy = (base.samples >= 100 && dl.samples >= 100);
                const uint32_t cap = s_r6Capacity ? s_r6Capacity : 1;
                snprintf(s_r6Result, sizeof(s_r6Result),
                         "%s cap=%luKB base(avg=%lu%% min=%lu%% zero=%lu low=%lu n=%lu) "
                         "dl(avg=%lu%% min=%lu%% zero=%lu low=%lu maxrun=%lu n=%lu) "
                         "dma_base=%luB/%luB dma_dl=%luB/%luB %lums thr=%lux/%lums%s",
                         downloadResultName(r),
                         static_cast<unsigned long>(cap / 1024),
                         static_cast<unsigned long>(base.avg * 100ull / cap),
                         static_cast<unsigned long>(base.min * 100ull / cap),
                         static_cast<unsigned long>(base.zeroCount),
                         static_cast<unsigned long>(base.lowCount),
                         static_cast<unsigned long>(base.samples),
                         static_cast<unsigned long>(dl.avg * 100ull / cap),
                         static_cast<unsigned long>(dl.min * 100ull / cap),
                         static_cast<unsigned long>(dl.zeroCount),
                         static_cast<unsigned long>(dl.lowCount),
                         static_cast<unsigned long>(dl.maxConsecZero),
                         static_cast<unsigned long>(dl.samples),
                         // 基线也要打 —— 只打下载期的话，分不出"系统本来就这么紧"
                         // 和"下载吃掉了一大块"，而这两者对应完全相反的处置。
                         static_cast<unsigned long>(base.dmaFreeMin),
                         static_cast<unsigned long>(base.dmaBlockMin),
                         static_cast<unsigned long>(dl.dmaFreeMin),
                         static_cast<unsigned long>(dl.dmaBlockMin),
                         static_cast<unsigned long>(st.elapsedMs),
                         static_cast<unsigned long>(st.throttleEvents),
                         static_cast<unsigned long>(st.throttleMs),
                         trustworthy ? "" : " [样本不足,结论不可信]");
                printf("[R6][TEST] %s\n", s_r6Result);
            }
        } else if (!kRunR6Experiment) {
            strlcpy(s_r6Result, "disabled (experiment done)", sizeof(s_r6Result));
        } else {
            strlcpy(s_r6Result, "skipped: no candidates from provider", sizeof(s_r6Result));
        }
        s_r6Done = true;

        free(tracks);
    } else {
        strlcpy(s_jamendoProbeErr, "ps_calloc failed", sizeof(s_jamendoProbeErr));
    }
    s_jamendoProbeDone = true;
    vTaskDelete(nullptr);
}

static void jamendoSelfTest() {
    // 一段按 Jamendo tracks API 真实格式构造的样本，覆盖三种情况：
    //   1. 正常曲目
    //   2. 标题里带转义（\" 和 \/），验证字符串扫描不会被引号截断
    //   3. audiodownload_allowed=false —— **必须被过滤掉**
    static const char kSample[] =
        "{\"headers\":{\"status\":\"success\",\"results_count\":3},\"results\":["
        "{\"id\":\"1880336\",\"name\":\"Sunrise\",\"duration\":204,"
        "\"artist_name\":\"Alpha\",\"album_name\":\"First\","
        "\"image\":\"https:\\/\\/usercontent.jamendo.com?id=1\","
        "\"audiodownload\":\"https:\\/\\/prod.jamendo.com\\/?trackid=1880336\","
        "\"audiodownload_allowed\":true},"
        "{\"id\":\"1880337\",\"name\":\"He said \\\"hi\\\" \\/ bye\",\"duration\":180,"
        "\"artist_name\":\"Beta\",\"album_name\":\"Second\","
        "\"image\":\"https:\\/\\/usercontent.jamendo.com?id=2\","
        "\"audiodownload\":\"https:\\/\\/prod.jamendo.com\\/?trackid=1880337\","
        "\"audiodownload_allowed\":true},"
        "{\"id\":\"1880338\",\"name\":\"NotAllowed\",\"duration\":300,"
        "\"artist_name\":\"Gamma\",\"album_name\":\"Third\","
        "\"image\":\"\",\"audiodownload\":\"https:\\/\\/x\","
        "\"audiodownload_allowed\":false}"
        "]}";

    RemoteTrack tracks[4]{};
    const uint8_t n = jamendoParseTracks(kSample, tracks, 4);
    s_jamendoSelfTestParsed = n;
    printf("[JAMENDO][SELFTEST] parsed=%u (expect 2, the third must be filtered)\n", n);
    for (uint8_t i = 0; i < n; ++i) {
        printf("[JAMENDO][SELFTEST]  id=%s dur=%lus title=\"%s\" artist=\"%s\" url=%.40s\n",
               tracks[i].providerTrackId, static_cast<unsigned long>(tracks[i].durationSec),
               tracks[i].title, tracks[i].artist, tracks[i].audioUrl);
    }
}

// —— 播放事件记录（Phase 1）——
//
// 放在 main.cpp 而不是审计 §8 建议的 src/library/play_history.cpp：这几个函数
// 本质上只是 libraryStoreAppendEvent() 的薄包装，而它们需要的状态
//（当前在放哪首、放到哪了）全都在 main.cpp 里。独立成模块反而要把这些状态
// 反向暴露出去，得不偿失。真正的存储逻辑仍然在 library_store 里。
// —— 事件队列 ——
//
// ⚠️ **播放路径绝不能碰 SD。**
//
// 第一版是在 libraryLogEvent() 里直接 SD_MMC.open(...,"a") 写盘，而它被
// playerCorePlaySdFile() 调用——就在 connecttoFS() 刚让音频库打开音乐文件、
// 正要开始流式读取的那一刻。在 1-bit SD_MMC 总线上这么干，实测**本地播放和
// 电台都点了没反应**（电台也中招是因为 playRadioUrl 会先 stop()，同样走到这里）。
// 而且 libraryStoreAppendEvent() 内部还有 ensureLibraryDir() 的两次
// SD_MMC.exists()，开销比看上去大得多。
//
// 现在改成：事件先进内存队列（纯赋值，微秒级），由 1 秒 tick 择机批量落盘，
// 且**优先在没有播放时才写**。内存状态仍然立即更新，UI 读到的数字不延迟。
constexpr uint8_t kLibraryEventQueueSize = 32;
static LibraryEvent s_libEventQueue[kLibraryEventQueueSize];
static volatile uint8_t s_libEventQueueCount = 0;
static uint32_t s_libEventDropped = 0;

// 记录一条事件：立刻更新内存，落盘交给队列。
static void libraryLogEvent(uint32_t localId, uint8_t type) {
    if (!localId) return;
    // RTC 没同步时传 0：libraryStoreApplyEvent() 会只累加计数、不写时间戳，
    // 避免把 1970 年写进 lastPlayedAt 让淘汰算法判反（见 library_store.cpp）。
    const uint32_t ts = s_f_rtc ? static_cast<uint32_t>(time(nullptr)) : 0;

    LibraryEvent e{};
    e.localId = localId;
    e.timestamp = ts;
    e.type = type;

    if (s_localTracks) libraryStoreApplyEvent(s_localTracks, s_localTrackCount, e);

    if (s_libEventQueueCount < kLibraryEventQueueSize) {
        s_libEventQueue[s_libEventQueueCount++] = e;
    } else {
        // 队列满：丢掉这条并计数。丢的是**落盘**，内存统计上面已经加过了，
        // 所以 UI 不受影响；只是这条在重启后会丢失。32 条的队列配上每秒一次的
        // 冲刷，正常使用几乎不可能满。
        ++s_libEventDropped;
    }
}

// 由 1 秒 tick 调用。把队列里的事件一次性追加到 events.log。
//
// 时机选择：**正在播放时默认不写**，避免和音频抢 SD 总线——除非队列快满了，
// 那时宁可短暂争用也不能丢事件。
static void libraryFlushEvents(bool audioBusy) {
    const uint8_t n = s_libEventQueueCount;
    if (!n) return;
    if (audioBusy && n < kLibraryEventQueueSize * 3 / 4) return;

    for (uint8_t i = 0; i < n; ++i) {
        libraryStoreAppendEvent(s_libEventQueue[i].localId, s_libEventQueue[i].type,
                                s_libEventQueue[i].timestamp);
    }
    s_libEventQueueCount = 0;
}

// 上一首还没播完就换歌 / 停止 —— 记为跳过。
static void libraryFinishCurrentTrack() {
    if (s_playingLocalId && !s_playCompleted) libraryLogEvent(s_playingLocalId, kEventSkipped);
    s_playingLocalId = 0;
    s_playCompleted = false;
}
static uint32_t s_scanNowEpoch = 0;
// s_usbStorageState itself is declared earlier now (see playerCorePlaySdFile()'s comment) -- the rest of the USB-storage statics stay here.
static volatile bool s_usbStorageBusy = false;
static volatile bool s_usbStorageHostEjected = false;
static bool s_usbMscModeActive = false;
static bool s_usbDacModeActive = false;
static volatile uint32_t s_usbStorageLastAccessMs = 0;
static volatile uint32_t s_usbMscReadCount = 0;
static volatile uint32_t s_usbMscWriteCount = 0;
static volatile uint32_t s_usbMscReadFailCount = 0;
static volatile uint32_t s_usbMscWriteFailCount = 0;
static volatile uint32_t s_usbMscLastLba = 0;
static volatile uint32_t s_usbMscMinLba = 0;
static volatile uint32_t s_usbMscMaxLba = 0;
static volatile uint32_t s_usbMscLastOffset = 0;
static volatile uint32_t s_usbMscLastSize = 0;
static volatile uint32_t s_usbMscMaxSize = 0;
static volatile int32_t s_usbMscLastResult = 0;
static uint32_t s_usbMscLbaBase = 0;
static uint32_t s_usbMscBlockCount = 0;
static uint32_t s_usbStoragePartitionStartLba = 0;
static UsbStorageFormatInfo s_usbStorageFormatInfo{};

#if MWR_USB_MSC_SUPPORTED
// ⚠️ USBMSC 的**构造函数**里就调了 tinyusb_enable_interface(USB_INTERFACE_MSC, …)
// （arduino-esp32 cores/esp32/USBMSC.cpp:204），也就是说只要这个对象存在，
// MSC 描述符就无条件被塞进配置描述符 —— 跟有没有调 begin() 毫无关系。
//
// 它原本是文件级全局对象，后果是**声卡模式下 MSC 接口也跟着枚举**：设备在主机
// 眼里成了 MSC + Audio 的复合设备（2026-09-05 用 macOS 的 ioreg 实测确认，
// 接口 0 是 TinyUSB MSC、接口 1 才是音频）。这既违背了"一个 USB 口同时只能是
// 一种用途"的设计，MSC 的 bulk 端点还要占 USB-OTG 的 DFIFO —— 而等时端点每帧
// 就要 196 字节，那块空间本来就吃紧，分不到时是**静默失败**：ep_out 保持 0、
// 端点从不 arm，可 tud_audio_set_itf_cb 照样被调用，于是屏幕上看到的就是
// "str=1 但 PACKETS 永远 0"，一个错误都不报。
//
// 改成函数内 static（C++11 magic static，线程安全的延迟构造）：第一次调用才
// 真正构造，声卡模式下这个函数永远不会被走到，MSC 接口自然也就不会注册。
static USBMSC& usbMsc() {
    static USBMSC instance;
    return instance;
}
static bool s_usbMscConfigured = false;
static bool s_usbStarted = false;
static uint16_t s_usbMscSectorSize = 0;
static constexpr bool kUsbMscReadOnlyProbe = false;
static constexpr bool kUsbMscExposePartitionOnly = false;
static constexpr bool kUsbMscBootPresentProbe = false;

static int32_t usbMscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize);
static int32_t usbMscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize);
static bool usbMscStartStop(uint8_t powerCondition, bool start, bool loadEject);

static uint32_t usbMscLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t usbMscLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

static bool usbMscMbrFitsDevice(const uint8_t* lba0, uint32_t sectorCount) {
    if (!lba0) return true;
    if (lba0[510] != 0x55 || lba0[511] != 0xAA) {
        printf("[USBMSC] no MBR boot signature, skip partition-bound check\n");
        return true;
    }

    bool sawPartition = false;
    bool fits = true;
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t* entry = lba0 + 446 + i * 16;
        const uint8_t type = entry[4];
        const uint32_t start = usbMscLe32(entry + 8);
        const uint32_t count = usbMscLe32(entry + 12);
        if (type == 0 || count == 0) continue;
        sawPartition = true;

        const uint64_t end = static_cast<uint64_t>(start) + static_cast<uint64_t>(count) - 1ULL;
        printf("[USBMSC] mbr part%u type=0x%02X start=%u sectors=%u end=%llu\n",
               static_cast<unsigned>(i + 1), type, static_cast<unsigned>(start), static_cast<unsigned>(count),
               static_cast<unsigned long long>(end));
        if (end >= sectorCount) {
            printf("[USBMSC] ERROR: partition end %llu exceeds exposed sectors %u\n",
                   static_cast<unsigned long long>(end), static_cast<unsigned>(sectorCount));
            fits = false;
        }
    }
    if (!sawPartition) printf("[USBMSC] MBR signature present, no usable partition entries\n");
    return fits;
}

static bool usbMscFindFirstPartition(const uint8_t* lba0, uint32_t sectorCount, uint32_t* outStart, uint32_t* outCount) {
    if (!lba0 || !outStart || !outCount || lba0[510] != 0x55 || lba0[511] != 0xAA) return false;
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t* entry = lba0 + 446 + i * 16;
        const uint8_t type = entry[4];
        const uint32_t start = usbMscLe32(entry + 8);
        const uint32_t count = usbMscLe32(entry + 12);
        if (type == 0 || count == 0 || start == 0) continue;
        const uint64_t end = static_cast<uint64_t>(start) + static_cast<uint64_t>(count) - 1ULL;
        if (end >= sectorCount) continue;
        *outStart = start;
        *outCount = count;
        return true;
    }
    return false;
}

static void usbStorageUpdateFormatInfo(uint32_t partitionStartLba, uint64_t totalBytes, uint64_t usedBytes) {
    UsbStorageFormatInfo info{};
    info.totalBytes = totalBytes;
    info.usedBytes = usedBytes <= totalBytes ? usedBytes : 0;
    info.partitionStartLba = partitionStartLba;

    uint8_t boot[512];
    if (SD_MMC.readRAW(boot, partitionStartLba) && boot[510] == 0x55 && boot[511] == 0xAA) {
        const uint16_t bytesPerSector = usbMscLe16(boot + 11);
        const uint8_t sectorsPerCluster = boot[13];
        const uint16_t rootEntryCount = usbMscLe16(boot + 17);
        const uint16_t fat16Sectors = usbMscLe16(boot + 22);
        const uint32_t fat32Sectors = usbMscLe32(boot + 36);
        const bool plausibleSectorSize = bytesPerSector == 512 || bytesPerSector == 1024 || bytesPerSector == 2048 || bytesPerSector == 4096;
        const bool plausibleCluster = sectorsPerCluster > 0 && (sectorsPerCluster & (sectorsPerCluster - 1)) == 0;
        info.valid = plausibleSectorSize && plausibleCluster;
        info.fat32 = info.valid && rootEntryCount == 0 && fat16Sectors == 0 && fat32Sectors > 0;
        info.bytesPerSector = bytesPerSector;
        info.sectorsPerCluster = sectorsPerCluster;
        info.allocationUnitBytes = static_cast<uint32_t>(bytesPerSector) * sectorsPerCluster;
        info.recommendedAllocation = info.fat32 && info.allocationUnitBytes == 32768;
        printf("[USBMSC] fs bpb valid=%u fat32=%u bytesPerSector=%u sectorsPerCluster=%u allocation=%u recommended=%u partStart=%u\n",
               static_cast<unsigned>(info.valid), static_cast<unsigned>(info.fat32), static_cast<unsigned>(info.bytesPerSector),
               static_cast<unsigned>(info.sectorsPerCluster), static_cast<unsigned>(info.allocationUnitBytes),
               static_cast<unsigned>(info.recommendedAllocation), static_cast<unsigned>(info.partitionStartLba));
    } else {
        printf("[USBMSC] fs bpb read failed at lba=%u\n", static_cast<unsigned>(partitionStartLba));
    }

    s_usbStorageFormatInfo = info;
}

// Geometry/format probe only -- no USB side effects. Called at every normal
// boot so the USB storage page can show FAT type / allocation unit /
// capacity without ever starting TinyUSB (which would take over the single
// USB-C port away from the serial console).
static bool usbStorageProbeGeometry() {
    const int sectorSize = SD_MMC.sectorSize();
    if (sectorSize <= 0) {
        MWR_LOG_ERROR("USB MSC init failed: invalid SD sector size");
        return false;
    }
    s_usbMscSectorSize = static_cast<uint16_t>(sectorSize);

    const uint64_t cardBytes = SD_MMC.cardSize();
    const uint64_t sectorCount64 = cardBytes / static_cast<uint32_t>(sectorSize);
    if (cardBytes == 0 || sectorCount64 == 0 || sectorCount64 > 0xFFFFFFFFULL) {
        MWR_LOG_ERROR("USB MSC init failed: invalid SD sector geometry");
        return false;
    }
    const uint32_t sectorCount = static_cast<uint32_t>(sectorCount64);

    uint32_t exposedBase = 0;
    uint32_t exposedCount = sectorCount;
    uint32_t firstPartitionStart = 0;
    {
        uint8_t lba0[512];
        bool lba0Ok = false;
        printf("[USBMSC] init cardBytes=%llu sectorSize=%d sectorCount=%u mode=%s\n",
               static_cast<unsigned long long>(cardBytes), sectorSize, static_cast<unsigned>(sectorCount),
               kUsbMscReadOnlyProbe ? "read-only" : "read-write");
        if (sectorSize <= (int)sizeof(lba0) && SD_MMC.readRAW(lba0, 0)) {
            lba0Ok = true;
            printf("[USBMSC] lba0 bootsig[510..511]= %02X %02X\n", lba0[510], lba0[511]);
        } else {
            printf("[USBMSC] readRAW(lba0) FAILED\n");
        }
        fflush(stdout);
        if (lba0Ok && !usbMscMbrFitsDevice(lba0, sectorCount)) {
            MWR_LOG_ERROR("USB MSC init failed: SD partition exceeds exposed device size");
            return false;
        }
        if (lba0Ok && kUsbMscExposePartitionOnly) {
            uint32_t partStart = 0;
            uint32_t partCount = 0;
            if (usbMscFindFirstPartition(lba0, sectorCount, &partStart, &partCount)) {
                exposedBase = partStart;
                exposedCount = partCount;
                firstPartitionStart = partStart;
            }
        } else if (lba0Ok) {
            uint32_t partStart = 0;
            uint32_t partCount = 0;
            if (usbMscFindFirstPartition(lba0, sectorCount, &partStart, &partCount)) firstPartitionStart = partStart;
        }
    }
    s_usbMscLbaBase = exposedBase;
    s_usbMscBlockCount = exposedCount;
    s_usbStoragePartitionStartLba = firstPartitionStart;
    usbStorageUpdateFormatInfo(s_usbStoragePartitionStartLba, SD_MMC.cardSize(), SD_MMC.usedBytes());
    printf("[USBMSC] expose base=%u blocks=%u partitionOnly=%u\n",
           static_cast<unsigned>(s_usbMscLbaBase), static_cast<unsigned>(s_usbMscBlockCount),
           static_cast<unsigned>(kUsbMscExposePartitionOnly));
    return true;
}

static bool usbStoragePrepareMsc(bool mediaPresent) {
    if (!usbStorageProbeGeometry()) return false;
    if (!s_usbMscConfigured) {
        usbMsc().vendorID("ESP32S3");
        usbMsc().productID("HiFi SD");
        usbMsc().productRevision("1.0");
        usbMsc().onRead(usbMscRead);
        usbMsc().onWrite(usbMscWrite);
        usbMsc().onStartStop(usbMscStartStop);
        usbMsc().isWritable(!kUsbMscReadOnlyProbe);
        usbMsc().mediaPresent(mediaPresent);
        if (!usbMsc().begin(static_cast<uint32_t>(s_usbMscBlockCount), s_usbMscSectorSize)) {
            MWR_LOG_ERROR("USB MSC begin failed");
            usbMsc().mediaPresent(false);
            return false;
        }
        s_usbMscConfigured = true;
    } else {
        usbMsc().isWritable(!kUsbMscReadOnlyProbe);
        usbMsc().mediaPresent(mediaPresent);
    }

    if (!s_usbStarted) {
        USB.manufacturerName("ESP32-S3 HiFi");
        USB.productName("HiFi SD Card");
        printf("[USBMSC] before boot USB.begin resetReason=%d\n", static_cast<int>(esp_reset_reason()));
        fflush(stdout);
        s_usbStarted = USB.begin();
        printf("[USBMSC] after boot USB.begin ok=%u resetReason=%d\n", static_cast<unsigned>(s_usbStarted), static_cast<int>(esp_reset_reason()));
        fflush(stdout);
        if (!s_usbStarted) {
            MWR_LOG_ERROR("USB begin failed");
            usbMsc().mediaPresent(false);
            return false;
        }
    }
    return true;
}
#endif

static bool usbStorageBlocksSdAppAccess() {
    return s_usbStorageState == UsbStorageState::Mounting ||
           s_usbStorageState == UsbStorageState::Mounted ||
           s_usbStorageState == UsbStorageState::Restoring;
}

static bool containsIcase(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return false;
    const size_t needleLen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, needleLen) == 0) return true;
    }
    return false;
}

// containsIcase() stops at the first NUL byte -- fine for genuine C-strings,
// but the APIC/PIC peek buffer starts with a 1-byte text-encoding field
// that's almost always 0x00, which made every scan-time cover check return
// false before it ever looked at the MIME string that follows. This variant
// is bounded by an explicit length instead of relying on NUL-termination.
static bool containsIcaseN(const uint8_t* haystack, size_t len, const char* needle) {
    const size_t needleLen = strlen(needle);
    if (!needleLen || needleLen > len) return false;
    for (size_t i = 0; i + needleLen <= len; ++i) {
        bool match = true;
        for (size_t j = 0; j < needleLen; ++j) {
            if (tolower(haystack[i + j]) != tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// SD_MMC/FAT filenames are commonly all-uppercase (e.g. "UTADA1.MP3") --
// endsWith() (common.h) is case-sensitive and silently skips those, so the
// scanner uses this instead.
static bool endsWithIcase(const char* name, const char* suffix) {
    if (!name || !suffix) return false;
    const size_t nameLen = strlen(name);
    const size_t suffixLen = strlen(suffix);
    if (suffixLen > nameLen) return false;
    return strncasecmp(name + (nameLen - suffixLen), suffix, suffixLen) == 0;
}

static void ensureLocalMusicDir() {
    if (usbStorageBlocksSdAppAccess()) return;
    File existing = SD_MMC.open(kLocalMusicDir);
    if (existing) {
        const bool isDir = existing.isDirectory();
        existing.close();
        if (isDir) return;
        MWR_LOG_ERROR("Local music path exists but is not a directory: {}", kLocalMusicDir);
        return;
    }
    if (SD_MMC.mkdir(kLocalMusicDir)) {
        printf("[MUSIC] created local music folder: %s\n", kLocalMusicDir);
    } else {
        MWR_LOG_ERROR("Failed to create local music folder: {}", kLocalMusicDir);
    }
}

// Reads a 4-byte synchsafe integer (each byte's high bit is always 0, 7
// significant bits) as used by ID3v2.4 header/frame sizes; v2.3 frame sizes
// are plain 32-bit big-endian instead (handled at each call site).
static uint32_t readSynchsafe32(const uint8_t* b) { return (uint32_t(b[0]) << 21) | (uint32_t(b[1]) << 14) | (uint32_t(b[2]) << 7) | uint32_t(b[3]); }
static uint32_t readBE32(const uint8_t* b) { return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]); }

// A lot of real-world J-pop MP3 tags (older/non-compliant taggers, common on
// files ripped from Japanese sources) write UTF-8 bytes but leave the ID3
// encoding byte at 0 (ISO-8859-1) instead of 3 -- decoding those as Latin-1
// treats each UTF-8 continuation byte as its own Latin-1 character and
// mangles every multi-byte codepoint into unrelated garbage (renders as
// tofu, since the resulting "UTF-8" from the double-encode doesn't map to
// real glyphs). This checks whether the raw bytes already form valid UTF-8
// with real multi-byte content, so that case can be detected and decoded
// correctly instead of trusting a mislabeled encoding byte.
static bool looksLikeValidUtf8(const uint8_t* data, size_t len) {
    size_t i = 0;
    bool sawMultibyte = false;
    while (i < len && data[i] != 0) {
        const uint8_t c = data[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        uint8_t extra;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false; // not a valid UTF-8 lead byte
        if (i + extra >= len) return false;
        for (uint8_t k = 1; k <= extra; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) return false; // not a valid continuation byte
        }
        sawMultibyte = true;
        i += extra + 1;
    }
    return sawMultibyte;
}

// Best-effort decode of an ID3 text frame's payload into UTF-8. Encoding
// byte: 0=ISO-8859-1 (Latin-1 -> UTF-8), 1=UTF-16 with BOM, 2=UTF-16BE no
// BOM (v2.4 only), 3=UTF-8 already.
static void decodeId3Text(const uint8_t* data, size_t len, char* out, size_t outSize) {
    if (!outSize) return;
    if (len == 0) { out[0] = '\0'; return; }
    uint8_t encoding = data[0];
    const uint8_t* text = data + 1;
    const size_t textLen = len - 1;
    size_t outPos = 0;

    if (encoding == 0 && looksLikeValidUtf8(text, textLen)) encoding = 3; // see looksLikeValidUtf8()

    if (encoding == 0 || encoding == 3) {
        // Character-aware (not byte-aware) truncation: the old version
        // capped the loop at "outPos + 2 < outSize" and copied one raw byte
        // per iteration regardless of whether that byte was the middle of a
        // multi-byte UTF-8 sequence -- running out of output space mid-title
        // could leave a lead byte with no (or only some of) its
        // continuation bytes, invalid UTF-8 that LVGL renders as a tofu box.
        // Now: figure out how many bytes the *whole* next character needs
        // (from the source, and from Latin-1's 2-byte UTF-8 expansion) and
        // stop before writing a partial one.
        for (size_t i = 0; i < textLen;) {
            const uint8_t c = text[i];
            if (c == 0) break;
            if (encoding == 3 && c >= 0x80) {
                size_t charLen = 1;
                if ((c & 0xE0) == 0xC0) charLen = 2;
                else if ((c & 0xF0) == 0xE0) charLen = 3;
                else if ((c & 0xF8) == 0xF0) charLen = 4;
                if (i + charLen > textLen) break;          // source truncated mid-character
                if (outPos + charLen + 1 > outSize) break;  // not enough room for the whole character + '\0'
                for (size_t k = 0; k < charLen; ++k) out[outPos++] = static_cast<char>(text[i + k]);
                i += charLen;
            } else if (c < 0x80) {
                if (outPos + 1 + 1 > outSize) break;
                out[outPos++] = static_cast<char>(c);
                ++i;
            } else { // Latin-1 high byte -> 2-byte UTF-8
                if (outPos + 2 + 1 > outSize) break;
                out[outPos++] = static_cast<char>(0xC0 | (c >> 6));
                out[outPos++] = static_cast<char>(0x80 | (c & 0x3F));
                ++i;
            }
        }
    } else {
        bool bigEndian = (encoding == 2);
        size_t i = 0;
        if (encoding == 1 && textLen >= 2) {
            if (text[0] == 0xFE && text[1] == 0xFF) { bigEndian = true; i = 2; }
            else if (text[0] == 0xFF && text[1] == 0xFE) { bigEndian = false; i = 2; }
        }
        for (; i + 1 < textLen && outPos + 3 < outSize; i += 2) {
            const uint16_t unit = bigEndian ? (uint16_t(text[i]) << 8 | text[i + 1]) : (uint16_t(text[i + 1]) << 8 | text[i]);
            if (unit == 0) break;
            if (unit < 0x80) {
                out[outPos++] = static_cast<char>(unit);
            } else if (unit < 0x800) {
                out[outPos++] = static_cast<char>(0xC0 | (unit >> 6));
                out[outPos++] = static_cast<char>(0x80 | (unit & 0x3F));
            } else {
                out[outPos++] = static_cast<char>(0xE0 | (unit >> 12));
                out[outPos++] = static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                out[outPos++] = static_cast<char>(0x80 | (unit & 0x3F));
            }
        }
    }
    out[outPos] = '\0';
}

// Scans an already-open file's ID3v2 tag for TIT2/TPE1/TALB (v2.2: TT2/
// TP1/TAL) and notes (via item->hasArt) whether an APIC/PIC frame with a
// JPEG payload is present -- the image bytes themselves are extracted
// lazily, only for whichever track is actually being displayed, by
// extractId3Picture() below (re-opens/re-parses; cheap, and avoids holding
// image data for a whole library scan).
static void parseId3Tags(File& file, TrackRecord* item) {
    uint8_t header[10];
    if (file.read(header, 10) != 10) return;
    if (memcmp(header, "ID3", 3) != 0) return;
    const uint8_t majorVersion = header[3];
    const uint32_t tagEnd = 10 + readSynchsafe32(&header[6]);
    const bool synchsafeFrameSize = majorVersion >= 4;
    const size_t idLen = majorVersion >= 3 ? 4 : 3;
    const size_t frameHeaderLen = majorVersion >= 3 ? 10 : 6;

    uint32_t pos = 10;
    uint8_t frameHeader[10];
    uint8_t textBuf[192];
    while (pos + frameHeaderLen <= tagEnd) {
        if (file.read(frameHeader, frameHeaderLen) != frameHeaderLen) break;
        char frameId[5] = {0, 0, 0, 0, 0};
        memcpy(frameId, frameHeader, idLen);
        if (frameId[0] == 0) break; // padding reached

        uint32_t frameSize;
        if (majorVersion >= 3) frameSize = synchsafeFrameSize ? readSynchsafe32(&frameHeader[4]) : readBE32(&frameHeader[4]);
        else frameSize = (uint32_t(frameHeader[3]) << 16) | (uint32_t(frameHeader[4]) << 8) | frameHeader[5];
        pos += frameHeaderLen;
        if (frameSize == 0 || pos + frameSize > tagEnd) break;

        const bool isTitle = strcmp(frameId, "TIT2") == 0 || strcmp(frameId, "TT2") == 0;
        const bool isArtist = strcmp(frameId, "TPE1") == 0 || strcmp(frameId, "TP1") == 0;
        const bool isAlbum = strcmp(frameId, "TALB") == 0 || strcmp(frameId, "TAL") == 0;
        const bool isPicture = strcmp(frameId, "APIC") == 0 || strcmp(frameId, "PIC") == 0;

        if (isPicture) {
            // Peek at the MIME/format field only, to avoid claiming
            // hasArt for a PNG cover (no PNG decoder in this build either).
            uint8_t peek[24] = {0};
            const uint32_t peekLen = std::min<uint32_t>(frameSize, sizeof(peek) - 1);
            file.read(peek, peekLen);
            if (containsIcaseN(peek, sizeof(peek), "jpeg") || containsIcaseN(peek, sizeof(peek), "jpg")) item->hasArt = true;
            file.seek(file.position() + (frameSize - peekLen));
        } else if ((isTitle || isArtist || isAlbum) && frameSize < sizeof(textBuf)) {
            if (file.read(textBuf, frameSize) != frameSize) break;
            char* dest = isTitle ? item->title : isArtist ? item->artist : item->album;
            const size_t destSize = isTitle ? sizeof(item->title) : isArtist ? sizeof(item->artist) : sizeof(item->album);
            decodeId3Text(textBuf, frameSize, dest, destSize);
        } else {
            file.seek(file.position() + frameSize);
        }
        pos += frameSize;
    }
}

// Re-opens `path` and walks its ID3 tag again looking for the first usable
// (JPEG) picture frame, this time extracting the exact image bytes into a
// fresh malloc'd buffer (caller frees). Only called for the track actually
// being shown, not during the library scan.
static bool extractId3Picture(const char* path, uint8_t** outData, size_t* outLen) {
    *outData = nullptr;
    *outLen = 0;
    File file = SD_MMC.open(path, "r");
    if (!file) return false;

    uint8_t header[10];
    bool ok = false;
    if (file.read(header, 10) == 10 && memcmp(header, "ID3", 3) == 0) {
        const uint8_t majorVersion = header[3];
        const uint32_t tagEnd = 10 + readSynchsafe32(&header[6]);
        const bool synchsafeFrameSize = majorVersion >= 4;
        const size_t idLen = majorVersion >= 3 ? 4 : 3;
        const size_t frameHeaderLen = majorVersion >= 3 ? 10 : 6;

        uint32_t pos = 10;
        uint8_t frameHeader[10];
        while (!ok && pos + frameHeaderLen <= tagEnd) {
            if (file.read(frameHeader, frameHeaderLen) != frameHeaderLen) break;
            char frameId[5] = {0, 0, 0, 0, 0};
            memcpy(frameId, frameHeader, idLen);
            if (frameId[0] == 0) break;

            uint32_t frameSize;
            if (majorVersion >= 3) frameSize = synchsafeFrameSize ? readSynchsafe32(&frameHeader[4]) : readBE32(&frameHeader[4]);
            else frameSize = (uint32_t(frameHeader[3]) << 16) | (uint32_t(frameHeader[4]) << 8) | frameHeader[5];
            pos += frameHeaderLen;
            if (frameSize == 0 || pos + frameSize > tagEnd) break;

            const bool isPicture = strcmp(frameId, "APIC") == 0 || strcmp(frameId, "PIC") == 0;
            const uint32_t frameStart = file.position();
            if (isPicture) {
                uint8_t encoding = 0;
                file.read(&encoding, 1);
                char mime[16] = {0};
                if (idLen == 4) { // APIC: null-terminated MIME string
                    size_t i = 0;
                    uint8_t c = 1;
                    while (i < sizeof(mime) - 1 && file.read(&c, 1) == 1 && c != 0) mime[i++] = static_cast<char>(c);
                } else { // PIC (v2.2): fixed 3-char format, not null-terminated
                    uint8_t fmt[3] = {0};
                    file.read(fmt, 3);
                    memcpy(mime, fmt, 3);
                }
                uint8_t pictureType = 0;
                file.read(&pictureType, 1);
                if (encoding == 1 || encoding == 2) { // UTF-16 description: 2-byte terminator
                    uint8_t pair[2] = {1, 1};
                    while (file.read(pair, 2) == 2 && (pair[0] != 0 || pair[1] != 0)) {}
                } else { // single-byte terminator
                    uint8_t c = 1;
                    while (file.read(&c, 1) == 1 && c != 0) {}
                }
                const uint32_t imageStart = file.position();
                const uint32_t imageLen = (frameStart + frameSize > imageStart) ? (frameStart + frameSize - imageStart) : 0;
                if ((containsIcase(mime, "jpeg") || containsIcase(mime, "jpg")) && imageLen > 0 && imageLen < 2UL * 1024 * 1024) {
                    uint8_t* buf = static_cast<uint8_t*>(ps_malloc(imageLen));
                    if (buf && file.read(buf, imageLen) == imageLen) {
                        *outData = buf;
                        *outLen = imageLen;
                        ok = true;
                    } else if (buf) {
                        free(buf);
                    }
                }
            }
            if (!ok) file.seek(frameStart + frameSize);
            pos += frameSize;
        }
    }
    file.close();
    return ok;
}

// 从文件名反推 provider 元数据。
//
// ⚠️ **为什么需要这个**：索引只在整轮同步结束时存盘一次（750KB，每首存一次
// 太重）。一旦中途重启/断电，已下好的文件还在 SD 上，索引里却没有记录 ——
// 下次开机扫描会把它们当成普通本地文件收进去：provider=local、没有
// providerTrackId、没有 Discovery 标记。后果有两个，都不轻：
//   1. 去重靠 provider+providerTrackId，认不出来 ⇒ **重复下载**
//      （2026-09-05 实测：中断后重跑，第一首又下了一遍 "Vlog Journey"）
//   2. 丢了 Discovery 标记 ⇒ Cleaner 当成用户自己的歌，**永不淘汰**
//
// 让文件名可还原元数据，索引就退化成缓存而不是唯一真相 —— 这比"更频繁地
// 存盘"稳健得多，也不用多写一次盘。
// 命名规则见 dailySyncBuildPath()：/music/tracks/<provider>_<id>.mp3
static void deriveProviderFromFilename(TrackRecord* item) {
    if (!item || !item->path[0]) return;

    // 只认 tracks 目录下的文件，避免用户自己拷进来的同名文件被误判。
    // 大小写不敏感 —— SD 上真实目录是 /Music（见 libraryHashPath 的说明）。
    bool inTracksDir = false;
    for (const char* p = item->path; *p; ++p) {
        if ((p[0] == '/') &&
            (p[1] == 't' || p[1] == 'T') && (p[2] == 'r' || p[2] == 'R') &&
            (p[3] == 'a' || p[3] == 'A') && (p[4] == 'c' || p[4] == 'C') &&
            (p[5] == 'k' || p[5] == 'K') && (p[6] == 's' || p[6] == 'S') &&
            p[7] == '/') {
            inTracksDir = true;
            break;
        }
    }
    if (!inTracksDir) return;

    const char* slash = strrchr(item->path, '/');
    const char* base = slash ? slash + 1 : item->path;

    static const char kPrefix[] = "jamendo_";
    const size_t plen = sizeof(kPrefix) - 1;
    for (size_t i = 0; i < plen; ++i) {
        char c = base[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kPrefix[i]) return;
    }

    // 前缀之后到 '.' 之前必须全是数字，否则不是我们下的文件
    const char* idStart = base + plen;
    const char* q = idStart;
    while (*q >= '0' && *q <= '9') ++q;
    if (q == idStart || *q != '.') return;

    const size_t idLen = static_cast<size_t>(q - idStart);
    if (idLen >= sizeof(item->providerTrackId)) return;
    memcpy(item->providerTrackId, idStart, idLen);
    item->providerTrackId[idLen] = '\0';
    item->provider = kProviderJamendo;
    item->flags |= kTrackFlagDiscovery;
}

static void scanMusicDir(const char* path, uint8_t depth) {
    if (usbStorageBlocksSdAppAccess()) return;
    if (!s_localTracks || s_localTrackCount >= s_localTrackCapacity || depth > 6) return;
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }
    for (;;) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (s_localTrackCount >= s_localTrackCapacity) {
            entry.close();
            break;
        }
        if (entry.isDirectory()) {
            ps_ptr<char> subPath = entry.path();
            entry.close();
            scanMusicDir(subPath.c_get(), depth + 1);
            continue;
        }
        if (endsWithIcase(entry.name(), ".mp3")) {
            TrackRecord item{};
            strlcpy(item.path, entry.path(), sizeof(item.path));
            item.fileSize = static_cast<uint32_t>(entry.size());
            parseId3Tags(entry, &item);
            if (!item.title[0]) { // no TIT2 -- fall back to filename minus extension
                strlcpy(item.title, entry.name(), sizeof(item.title));
                char* dot = strrchr(item.title, '.');
                if (dot) *dot = '\0';
            }
            printf("[MUSIC] #%u title=\"%s\" artist=\"%s\" album=\"%s\" hasArt=%d\n", s_localTrackCount, item.title, item.artist, item.album, item.hasArt);
            // 并入索引而不是直接追加：已存在的曲目要保留播放统计和 favorite，
            // 重扫一次 SD 不该把用户行为的产物清零
            //（见 library_store.h 里 libraryStoreUpsert 的说明）。
            // ⚠️ 诊断：新追加 vs 已存在。2026-09-05 实测发现下载入库的 3 首
            // 在下次扫描时被判成"不存在"（标 missing）同时又被当新曲追加，
            // 说明 daily_sync 写的路径和 entry.path() 对不上 —— 但对不上在哪
            // 靠猜没结果，得把两边的路径原样打出来。
            deriveProviderFromFilename(&item);
            const uint16_t beforeCount = s_localTrackCount;
            const int32_t upIdx = libraryStoreUpsert(s_localTracks, &s_localTrackCount,
                                                     s_localTrackCapacity, item,
                                                     s_scanSeenBits, s_scanNowEpoch);

            // ⚠️ 补救已经被误收成 local 的记录。
            // libraryStoreUpsert 命中已有记录时**刻意不覆盖 provider/flags**
            // （防止重扫清掉用户行为的产物），所以上面那个反推只对新记录生效。
            // 而中断过的同步恰恰会留下一批"文件在、记录是 local"的旧条目 ——
            // 不补的话它们永远不会被 Cleaner 淘汰。
            //
            // **只在现有记录压根没有 providerTrackId 时才填**，绝不覆盖已有值：
            // 有值说明它本来就是 provider 曲目，轮不到从文件名猜。
            if (upIdx >= 0 && item.provider != kProviderLocal &&
                s_localTracks[upIdx].providerTrackId[0] == '\0') {
                s_localTracks[upIdx].provider = item.provider;
                strlcpy(s_localTracks[upIdx].providerTrackId, item.providerTrackId,
                        sizeof(s_localTracks[upIdx].providerTrackId));
                s_localTracks[upIdx].flags |= kTrackFlagDiscovery;
                printf("[MUSIC][ADOPT] %s -> provider=%u id=%s\n",
                       item.path, item.provider, item.providerTrackId);
            }
            if (s_localTrackCount > beforeCount) {
                printf("[MUSIC][NEW] path=\"%s\" id=%lu\n", item.path,
                       static_cast<unsigned long>(libraryHashPath(item.path)));
            }
        }
        entry.close();
    }
    dir.close();
}

static void localMusicScanTask(void*) {
    ensureLocalMusicDir();
    if (s_localTracks) {
        free(s_localTracks);
        s_localTracks = nullptr;
    }
    s_localTrackCount = 0;

    const uint32_t psramBefore = ESP.getFreePsram();
    const uint32_t startMs = millis();

    // 分配失败时逐级退让，而不是直接把曲库变成空的。
    // 之前这里是 `if (s_localTracks) scanMusicDir(...)` —— ps_calloc 失败会
    // **静默**跳过整个扫描，用户看到的是"一首歌都没有"，日志里却什么都没有。
    // 扩容到 2000 首（644KB PSRAM）之后这个失败路径的概率明显上升，必须有声音。
    uint16_t capacity = kMaxLocalTracks;
    while (capacity >= 128) {
        s_localTracks = static_cast<TrackRecord*>(ps_calloc(capacity, sizeof(TrackRecord)));
        if (s_localTracks) break;
        printf("[MUSIC] ps_calloc(%u tracks, %u B) failed, halving\n",
               capacity, static_cast<unsigned>(capacity * sizeof(TrackRecord)));
        capacity /= 2;
    }
    if (!s_localTracks) {
        MWR_LOG_ERROR("local music index alloc failed -- library will be empty");
        s_localTrackCapacity = 0;
        s_localLibraryScanning = false;
        vTaskDelete(nullptr);
        return;
    }
    s_localTrackCapacity = capacity;

    // 先把已有索引读回来（顺带回放 events.log），扫描再往上 upsert。
    // 这样重扫一次 SD 不会丢掉播放次数、favorite 这些用户行为的产物。
    const uint32_t loadStart = millis();
    s_localTrackCount = libraryStoreLoad(s_localTracks, capacity);
    s_libLoadedAtBoot = s_localTrackCount;
    const uint32_t loadMs = millis() - loadStart;

    // seen 位图：记录本轮扫描碰过哪些下标，扫完把没碰过的标成 missing。
    // 2000 首 = 250 字节，栈上放得下（扫描任务栈 8192）。
    const uint16_t seenBytes = libraryStoreSeenBytes(capacity);
    uint8_t seenBits[libraryStoreSeenBytes(kMaxLocalTracks)]{};
    memset(seenBits, 0, seenBytes);
    s_scanSeenBits = seenBits;
    // RTC 没同步时给 0：libraryStoreApplyEvent/Upsert 会只更新计数不写时间戳，
    // 避免把 1970 年写进 lastPlayedAt 让淘汰算法判反（见 library_store.cpp）。
    s_scanNowEpoch = s_f_rtc ? static_cast<uint32_t>(time(nullptr)) : 0;

    const uint32_t allocMs = millis();
    scanMusicDir("/", 0);
    const uint32_t doneMs = millis();

    // 索引里有、这次没扫到的 → 标 missing（不删除，留住 favorite 与播放历史）。
    // 最典型的成因是用户在 U 盘模式下用电脑删了歌，见审计 §10。
    const uint16_t nowMissing = libraryStoreFinishScan(s_localTracks, s_localTrackCount, seenBits);
    // ⚠️ 无条件打印，不能只在 nowMissing != 0 时打 —— finishScan 只统计**本次
    // 新标记**的，已经标过的会跳过。第一版写成 if (nowMissing) 结果重启后
    // 一行都不打，白跑一轮。
    // 同时把所有 provider 曲目打出来：下载入库的和扫描新增的会各存一份，
    // 两份对照才看得出路径差在哪。
    {
        uint16_t shown = 0;
        for (uint16_t i = 0; i < s_localTrackCount && shown < 12; ++i) {
            const TrackRecord& r = s_localTracks[i];
            const bool isMissing = (r.flags & kTrackFlagMissing) != 0;
            if (!isMissing && r.provider != kProviderJamendo) continue;
            printf("[MUSIC][CHK] %s path=\"%s\" id=%lu prov=%u flags=0x%02X\n",
                   isMissing ? "MISSING" : "present",
                   r.path, static_cast<unsigned long>(r.localId),
                   r.provider, r.flags);
            ++shown;
        }
    }
    s_scanSeenBits = nullptr; // seenBits 是栈上的，函数返回后就失效了

    libraryStoreSave(s_localTracks, s_localTrackCount);

    // Phase 2：扫描完跑一次 dry-run。**不删除任何东西**，只把"如果空间不够会
    // 删哪些、能腾多少"打出来，供人工确认算法是否合理（方案 §30）。
    // incomingBytes 传 0 —— 现在还没有下载功能，只做纯容量评估。
    libraryCleanerDryRun(s_localTracks, s_localTrackCount, 0, s_scanNowEpoch,
                         s_playingLocalId, nullptr, 0);
    libraryCleanerAssess(s_localTracks, s_localTrackCount, 0, s_playingLocalId, &s_cleanerPlan);

    // Phase 3：解析器自检（不联网、不需要 client_id），以及读取已配置的 client_id。
    // 方案 §19：开机清掉上次断电/断网留下的 .part，避免半截文件被误当成歌曲。
    // 第一版直接删掉重下，不做 HTTP Range 续传。
    downloadCleanupStalePartFiles();

    jamendoSelfTest();
    // client_id 优先取 NVS（将来可以从 UI 配），没有就退回构建期的宏。
    //
    // ⚠️ 那个宏定义在 platformio_override.ini 里——**gitignored**，和 WiFi 密码
    // 同一个地方。凭据不进仓库（审计 R12）。没配置时 available() 为 false，
    // 整条发现链路静默跳过，不影响其它功能。
    {
        String id;
        if (lockPreferences()) {
            id = pref.getString(kJamendoClientIdPrefKey, "");
            unlockPreferences();
        }
        if (id.isEmpty()) id = JAMENDO_CLIENT_ID;
        s_jamendo.setClientId(id.c_str());
        // ⚠️ 必须在这里把同步状态读回来。不读的话 lastRunDay 恒为 0，
        // **每次重启都会被当成"今天还没同步过"**，反复下载。
        dailySyncLoadState();
    }
    if (nowMissing) printf("[LIB] %u track(s) newly marked missing\n", nowMissing);
    printf("[LIB][PERF] load=%lums\n", static_cast<unsigned long>(loadMs));

    // Phase 1 实测埋点：扩容之后必须知道分配成本、扫描耗时和单首均摊，才能推算
    // 真实曲库规模下的开机等待时间（见 docs/LOCAL_LIBRARY_V2_AUDIT.md §12）。
    // 本机 SD 上只有几十首，2000 首的耗时只能靠 per_track 外推。
    printf("[MUSIC] scan done, %u tracks (capacity %u)\n", s_localTrackCount, capacity);
    printf("[MUSIC][PERF] alloc=%lums scan=%lums per_track=%.1fms\n",
           static_cast<unsigned long>(allocMs - startMs),
           static_cast<unsigned long>(doneMs - allocMs),
           s_localTrackCount ? static_cast<double>(doneMs - allocMs) / s_localTrackCount : 0.0);
    printf("[MUSIC][PERF] psram before=%lu after=%lu used=%ld internal_free=%lu\n",
           static_cast<unsigned long>(psramBefore),
           static_cast<unsigned long>(ESP.getFreePsram()),
           static_cast<long>(psramBefore) - static_cast<long>(ESP.getFreePsram()),
           static_cast<unsigned long>(ESP.getFreeHeap()));

    s_localLibraryScanning = false;
    vTaskDelete(nullptr);
}

static bool startLocalMusicScan() {
    if (s_localLibraryScanning || usbStorageBlocksSdAppAccess()) return false;
    s_localLibraryScanning = true;
    const BaseType_t created = xTaskCreatePinnedToCore(localMusicScanTask, "musicScan", 8192, nullptr, 1, nullptr, 0);
    if (created != pdPASS) {
        s_localLibraryScanning = false;
        MWR_LOG_ERROR("Failed to create local music scan task");
        return false;
    }
    return true;
}

// —— 曲库查询与状态 API（Phase 1）——
//
// 只按下标暴露，不返回指针：s_localTracks 在重扫时会被 free/realloc，
// 把内部指针交出去迟早会用到已释放的内存。

// 用下标读一条完整记录（含 2.0 的统计字段）。UI 侧的 LocalTrackItem 版本
// 见 playerCoreLocalTrack()。
bool playerCoreLocalTrackRecord(uint16_t index, TrackRecord* out) {
    if (!s_localTracks || index >= s_localTrackCount || !out) return false;
    *out = s_localTracks[index];
    return true;
}

// 按类别统计条数。UI 做"今日新歌 / 收藏 / 全部"筛选时先拿总数再逐条取。
// kind: 0=全部（含 missing）1=收藏 2=最近播放过 3=自动发现下载 4=可播放（非 missing）
uint16_t playerCoreLibraryCount(uint8_t kind) {
    if (!s_localTracks) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < s_localTrackCount; ++i) {
        const TrackRecord& r = s_localTracks[i];
        switch (kind) {
            case 1: if (r.flags & kTrackFlagFavorite) ++n; break;
            case 2: if (r.playCount) ++n; break;
            case 3: if (r.flags & kTrackFlagDiscovery) ++n; break;
            case 4: if (!(r.flags & kTrackFlagMissing)) ++n; break;
            default: ++n; break;
        }
    }
    return n;
}

// 取某一类里的第 n 条，返回它在主数组中的下标；越界返回 -1。
int32_t playerCoreLibraryIndexOf(uint8_t kind, uint16_t nth) {
    if (!s_localTracks) return -1;
    uint16_t n = 0;
    for (uint16_t i = 0; i < s_localTrackCount; ++i) {
        const TrackRecord& r = s_localTracks[i];
        bool match = false;
        switch (kind) {
            case 1: match = r.flags & kTrackFlagFavorite; break;
            case 2: match = r.playCount != 0; break;
            case 3: match = r.flags & kTrackFlagDiscovery; break;
            case 4: match = !(r.flags & kTrackFlagMissing); break;
            default: match = true; break;
        }
        if (match && n++ == nth) return static_cast<int32_t>(i);
    }
    return -1;
}

bool playerCoreTrackFavorite(uint16_t index) {
    return s_localTracks && index < s_localTrackCount && (s_localTracks[index].flags & kTrackFlagFavorite);
}

bool playerCoreTrackKeep(uint16_t index) {
    return s_localTracks && index < s_localTrackCount && (s_localTracks[index].flags & kTrackFlagKeep);
}

// 设置收藏 / 保留。走事件日志而不是直接改内存 + 全量存盘：
// 800KB 的索引为了翻一个 bit 全量重写既慢又磨损 SD（见 library_store.h）。
void playerCoreSetTrackFavorite(uint16_t index, bool on) {
    if (!s_localTracks || index >= s_localTrackCount) return;
    if (playerCoreTrackFavorite(index) == on) return;
    libraryLogEvent(s_localTracks[index].localId, on ? kEventFavoriteOn : kEventFavoriteOff);
}

void playerCoreSetTrackKeep(uint16_t index, bool on) {
    if (!s_localTracks || index >= s_localTrackCount) return;
    if (playerCoreTrackKeep(index) == on) return;
    libraryLogEvent(s_localTracks[index].localId, on ? kEventKeepOn : kEventKeepOff);
}

bool playerCoreLocalLibraryScanning() { return s_localLibraryScanning; }
uint16_t playerCoreLocalLibraryCount() { return s_localTrackCount; }

bool playerCoreLocalTrack(uint16_t index, LocalTrackItem* item) {
    if (s_localLibraryScanning || usbStorageBlocksSdAppAccess()) return false;
    if (!s_localTracks || index >= s_localTrackCount || !item) return false;
    // TrackRecord 比 LocalTrackItem 多了一堆 2.0 的统计字段，不能整体赋值。
    // 对外 API 仍然给 LocalTrackItem，UI 侧一行不用改。
    const TrackRecord& r = s_localTracks[index];
    strlcpy(item->path, r.path, sizeof(item->path));
    strlcpy(item->title, r.title, sizeof(item->title));
    strlcpy(item->artist, r.artist, sizeof(item->artist));
    strlcpy(item->album, r.album, sizeof(item->album));
    item->hasArt = r.hasArt;
    return true;
}

bool playerCoreDecodeLocalTrackCover(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) {
    *outPixels = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    if (s_localLibraryScanning || usbStorageBlocksSdAppAccess()) return false;
    if (!s_localTracks || index >= s_localTrackCount) {
        printf("[COVER] idx=%u out of range (count=%u)\n", index, s_localTrackCount);
        return false;
    }
    if (!s_localTracks[index].hasArt) {
        printf("[COVER] idx=%u hasArt=false (scan-time MIME check found no jpeg/jpg)\n", index);
        return false;
    }
    uint8_t* jpegData = nullptr;
    size_t jpegLen = 0;
    const uint32_t coverExtractStart = millis();
    if (!extractId3Picture(s_localTracks[index].path, &jpegData, &jpegLen)) {
        printf("[COVER] idx=%u extractId3Picture failed (path=%s)\n", index, s_localTracks[index].path);
        return false;
    }
    // ⚠️ 这两步都是**阻塞的，而且跑在 UI 线程上**：extractId3Picture 要在 MP3
    // 文件里 seek 找图片，decodeJpgFromMemory 是纯 CPU 解码。
    // 2026-09-05 滚动测量里抓到过单次 lv_timer_handler 阻塞 904ms 的尖峰，
    // 形态是"刷屏很少却占满 CPU"，高度怀疑就是这里。分别计时才能定性。
    const uint32_t coverDecodeStart = millis();
    printf("[COVER] idx=%u extracted %u JPEG bytes (%lums), decoding...\n",
           index, static_cast<unsigned>(jpegLen),
           static_cast<unsigned long>(coverDecodeStart - coverExtractStart));
    const bool ok = getTFT().decodeJpgFromMemory(jpegData, jpegLen, scaleFactor, outPixels, outWidth, outHeight);
    printf("[COVER] idx=%u decode %lums (extract %lums, total %lums)\n", index,
           static_cast<unsigned long>(millis() - coverDecodeStart),
           static_cast<unsigned long>(coverDecodeStart - coverExtractStart),
           static_cast<unsigned long>(millis() - coverExtractStart));
    if (!ok) printf("[COVER] idx=%u decodeJpgFromMemory failed\n", index);
    free(jpegData);
    return ok;
}

#if MWR_USB_MSC_SUPPORTED
static bool usbStorageAllowsMscIo() {
    return s_usbStorageState == UsbStorageState::Mounting || s_usbStorageState == UsbStorageState::Mounted;
}

static bool usbMscRequestFits(uint32_t lba, uint32_t offset, uint32_t size, uint32_t sectorSize) {
    if (sectorSize == 0 || s_usbMscBlockCount == 0) return false;
    if (offset >= sectorSize) return false;
    if (size == 0) return lba < s_usbMscBlockCount;

    const uint64_t begin = static_cast<uint64_t>(lba) * sectorSize + offset;
    const uint64_t end = begin + size;
    const uint64_t limit = static_cast<uint64_t>(s_usbMscBlockCount) * sectorSize;
    return end <= limit;
}

static int32_t usbMscRecordResult(bool write, uint32_t lba, uint32_t offset, uint32_t size, int32_t result) {
    s_usbMscLastLba = lba;
    const uint32_t accessCount = size ? ((offset + size + 511) / 512) : 1;
    const uint32_t endLba = lba + accessCount - 1;
    const uint32_t totalCount = s_usbMscReadCount + s_usbMscWriteCount;
    if (totalCount == 0 || lba < s_usbMscMinLba) s_usbMscMinLba = lba;
    if (endLba > s_usbMscMaxLba) s_usbMscMaxLba = endLba;
    s_usbMscLastOffset = offset;
    s_usbMscLastSize = size;
    if (size > s_usbMscMaxSize) s_usbMscMaxSize = size;
    s_usbMscLastResult = result;
    if (write) {
        ++s_usbMscWriteCount;
        if (result < 0 || static_cast<uint32_t>(result) != size) ++s_usbMscWriteFailCount;
    } else {
        ++s_usbMscReadCount;
        if (result < 0 || static_cast<uint32_t>(result) != size) ++s_usbMscReadFailCount;
    }
    return result;
}

static int32_t usbMscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!usbStorageAllowsMscIo() || !buffer) return usbMscRecordResult(false, lba, offset, bufsize, -1);
    const uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return usbMscRecordResult(false, lba, offset, bufsize, -1);
    if (!usbMscRequestFits(lba, offset, bufsize, secSize)) {
        return usbMscRecordResult(false, lba, offset, bufsize, -1);
    }
    s_usbStorageLastAccessMs = millis();

    if (offset == 0 && (bufsize % secSize) == 0) {
        for (uint32_t done = 0; done < bufsize; done += secSize) {
            const uint32_t hostLba = lba + done / secSize;
            const uint32_t physicalLba = s_usbMscLbaBase + hostLba;
            if (!SD_MMC.readRAW(static_cast<uint8_t*>(buffer) + done, physicalLba)) {
                return usbMscRecordResult(false, hostLba, offset, bufsize, -1);
            }
        }
        return usbMscRecordResult(false, lba, offset, bufsize, static_cast<int32_t>(bufsize));
    }

    uint8_t sector[512];
    if (secSize > sizeof(sector)) return usbMscRecordResult(false, lba, offset, bufsize, -1);
    uint32_t done = 0;
    while (done < bufsize) {
        const uint32_t absolute = offset + done;
        const uint32_t hostLba = lba + absolute / secSize;
        const uint32_t physicalLba = s_usbMscLbaBase + hostLba;
        const uint32_t inSector = absolute % secSize;
        const uint32_t chunk = std::min<uint32_t>(bufsize - done, secSize - inSector);
        if (!SD_MMC.readRAW(sector, physicalLba)) return usbMscRecordResult(false, hostLba, offset, bufsize, -1);
        memcpy(static_cast<uint8_t*>(buffer) + done, sector + inSector, chunk);
        done += chunk;
    }
    return usbMscRecordResult(false, lba, offset, bufsize, static_cast<int32_t>(bufsize));
}

static int32_t usbMscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (kUsbMscReadOnlyProbe) {
        s_usbStorageLastAccessMs = millis();
        return usbMscRecordResult(true, lba, offset, bufsize, -1);
    }
    if (!usbStorageAllowsMscIo() || !buffer) return usbMscRecordResult(true, lba, offset, bufsize, -1);
    const uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return usbMscRecordResult(true, lba, offset, bufsize, -1);
    if (!usbMscRequestFits(lba, offset, bufsize, secSize)) {
        return usbMscRecordResult(true, lba, offset, bufsize, -1);
    }
    s_usbStorageLastAccessMs = millis();

    if (offset == 0 && (bufsize % secSize) == 0) {
        for (uint32_t done = 0; done < bufsize; done += secSize) {
            const uint32_t hostLba = lba + done / secSize;
            const uint32_t physicalLba = s_usbMscLbaBase + hostLba;
            if (!SD_MMC.writeRAW(buffer + done, physicalLba)) {
                return usbMscRecordResult(true, hostLba, offset, bufsize, -1);
            }
        }
        return usbMscRecordResult(true, lba, offset, bufsize, static_cast<int32_t>(bufsize));
    }

    uint8_t sector[512];
    if (secSize > sizeof(sector)) return usbMscRecordResult(true, lba, offset, bufsize, -1);
    uint32_t done = 0;
    while (done < bufsize) {
        const uint32_t absolute = offset + done;
        const uint32_t hostLba = lba + absolute / secSize;
        const uint32_t physicalLba = s_usbMscLbaBase + hostLba;
        const uint32_t inSector = absolute % secSize;
        const uint32_t chunk = std::min<uint32_t>(bufsize - done, secSize - inSector);
        if (!SD_MMC.readRAW(sector, physicalLba)) return usbMscRecordResult(true, hostLba, offset, bufsize, -1);
        memcpy(sector + inSector, buffer + done, chunk);
        if (!SD_MMC.writeRAW(sector, physicalLba)) return usbMscRecordResult(true, hostLba, offset, bufsize, -1);
        done += chunk;
    }
    return usbMscRecordResult(true, lba, offset, bufsize, static_cast<int32_t>(bufsize));
}

static bool usbMscStartStop(uint8_t powerCondition, bool start, bool loadEject) {
    printf("[USBMSC] start_stop power=%u start=%u eject=%u\n", powerCondition, start, loadEject);
    if (loadEject && !start) s_usbStorageHostEjected = true;
    return true;
}

static void usbStorageRebootToMscTask(void*) {
    // Mounting now means "write the NVS flag and reboot into USB storage
    // mode" -- TinyUSB is only ever started at boot (runtime USB.begin()
    // is unsafe: ESP_RST_USB, see DEV_LOG 2026-07-31). The short delay
    // lets the UI paint the "正在重启进入U盘模式" state before the reboot.
    s_usbStorageState = UsbStorageState::Mounting;
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP.restart();
    vTaskDelete(nullptr);
}

static void usbStorageRebootToNormalTask(void*) {
    // Leaving storage mode is the mirror image: clear the flag and reboot
    // back into the normal app, which remounts SD and rescans /Music.
    s_usbStorageState = UsbStorageState::Restoring;
    // Give any pending host writes a moment to settle before the reboot
    // (same quiesce window the old unmount task used).
    const uint32_t waitStart = millis();
    while (millis() - s_usbStorageLastAccessMs < 1000 && millis() - waitStart < 3000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    usbMscSetModeFlag(false);
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP.restart();
    vTaskDelete(nullptr);
}
#endif

UsbStorageState playerCoreUsbStorageState() { return s_usbStorageState; }

bool playerCoreUsbStorageStats(UsbStorageStats* out) {
    if (!out) return false;
    out->readCount = s_usbMscReadCount;
    out->writeCount = s_usbMscWriteCount;
    out->readFailCount = s_usbMscReadFailCount;
    out->writeFailCount = s_usbMscWriteFailCount;
    out->lastLba = s_usbMscLastLba;
    out->minLba = s_usbMscMinLba;
    out->maxLba = s_usbMscMaxLba;
    out->lastOffset = s_usbMscLastOffset;
    out->lastSize = s_usbMscLastSize;
    out->maxSize = s_usbMscMaxSize;
    out->lastResult = s_usbMscLastResult;
    return true;
}

bool playerCoreUsbStorageFormatInfo(UsbStorageFormatInfo* out) {
    if (!out) return false;
    *out = s_usbStorageFormatInfo;
    return s_usbStorageFormatInfo.valid || s_usbStorageFormatInfo.totalBytes > 0;
}

// USB 声卡模式的进入/退出。和 U 盘模式共用同一套机制、同样的理由：这块板
// 运行时调 USB.begin() 会触发 ESP_RST_USB 复位，TinyUSB 只能在开机时启动，
// 所以切换模式必须"写 NVS 标志 + 重启"，没有别的办法。
//
// 短延时是留给 UI 把状态画出来，和 usbStorageRebootToMscTask 一个道理。
static void usbDacRebootTask(void* arg) {
    // arg 非空 = 退出路径：延长到 1.5 秒，让用户读完屏幕上那句"请重新插拔 USB 线"。
    // 进入路径仍是 400ms（只是给 UI 一点时间画出状态）。
    vTaskDelay(arg ? pdMS_TO_TICKS(1500) : pdMS_TO_TICKS(400));
#if MWR_USB_DAC_SUPPORTED
    // 从声卡模式退出时，光 ESP.restart() 不够：必须先把 USB PHY 的归属交还给
    // ROM 的 USB-Serial/JTAG，否则重启后板子在 USB 上彻底消失。详见下面。
    if (s_usbDacModeActive) {
        // 制造一次 USB 总线断开，让主机把这台设备从列表里移除。
        //
        // 为什么必须显式做这件事（2026-09-07 实测确认）：
        // ESP.restart() 是**软件 CPU 复位**，它不复位 USB 外设、也不产生总线
        // 断开。板子确实重启并回到正常模式（屏幕能看到主界面），但主机从头到尾
        // 没看到断开，于是 MiniWebRadio DAC **一直留在设备列表里不消失**，
        // 成为一个没人应答的僵尸枚举。
        //
        // ⚠️ 注意历史记录里那句「退出后板子从 USB 上消失」是**错误描述**，
        // 它把排查方向带偏了两轮——真实现象是旧设备赖着不走，不是设备没了。
        //
        // 拉低 D+/D- 就是 USB 断开的物理表现。这段抄自框架的
        // usb_switch_to_cdc_jtag()（static，不能直接调用）。
        pinMode(USBPHY_DM_NUM, OUTPUT_OPEN_DRAIN);
        pinMode(USBPHY_DP_NUM, OUTPUT_OPEN_DRAIN);
        digitalWrite(USBPHY_DM_NUM, LOW);
        digitalWrite(USBPHY_DP_NUM, LOW);
        vTaskDelay(pdMS_TO_TICKS(20)); // 留够主机识别断开的时间

        // 必须再释放回高阻，不能带着"被驱动为低"的状态去重启。那个参考函数是给
        // **不重启的运行时切换**设计的（拉低后挂中断等 BUS_RESET 再自行恢复），
        // 只抄前半截就 restart，引脚会一直被压着 —— 实测表现是设备确实消失了，
        // 但重启后 USB 上什么都不出现。
        pinMode(USBPHY_DM_NUM, INPUT);
        pinMode(USBPHY_DP_NUM, INPUT);

        // —— 以下两条已实测无效，**不要再加回来** ——
        //
        // 1) 清 RTC_CNTL_USB_CONF_REG 的 PHY 归属位
        //    （SW_HW_USB_PHY_SEL / SW_USB_PHY_SEL / USB_PAD_ENABLE）
        //    实测：加与不加结果完全一样，既无害也无用。
        // 2) periph_module_reset/disable(PERIPH_USB_MODULE) 复位 OTG 外设
        //    实测：同样不改变结果。
        //
        // 也就是说：断开脉冲能让主机干净地移除设备，但重启后
        // USB-Serial/JTAG 仍需**一次物理插拔**才会被主机重新枚举。
        // 已知拔插能正常恢复（等于断电冷启动），所以硬件没有被弄坏。
        //
        // 退出键按下时屏幕会显示「已退出，请重新插拔 USB 线」，
        // 并把重启延时放长到 1.5s 让用户读完。
        //
        // 下次若要再攻这个问题，换个思路：正常模式**冷启动**时把
        // RTC_CNTL_USB_CONF_REG 的原始值读出来打印（正常模式串口是通的），
        // 退出时**原样恢复那个值**，而不是猜该清哪几位。
    }
#endif
    ESP.restart();
    vTaskDelete(nullptr);
}

bool playerCoreUsbDacEnter() {
#if MWR_USB_DAC_SUPPORTED
    stopSong(); // 先停播放，I2S 要交给声卡模式独占
    usbDacSetModeFlag(true); // 内部会同时清掉 U 盘模式标志（两者互斥）
    if (xTaskCreatePinnedToCore(usbDacRebootTask, "usbDacOn", 4096, nullptr, 2, nullptr, 0) != pdPASS) {
        usbDacSetModeFlag(false);
        MWR_LOG_ERROR("Failed to create USB DAC reboot task");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool playerCoreUsbDacExit() {
#if MWR_USB_DAC_SUPPORTED
    usbDacSetModeFlag(false);
    if (xTaskCreatePinnedToCore(usbDacRebootTask, "usbDacOff", 4096, reinterpret_cast<void*>(1), 2, nullptr, 0) != pdPASS) {
        MWR_LOG_ERROR("Failed to create USB DAC exit reboot task");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool playerCoreUsbDacActive() {
    return s_usbDacModeActive;
}

bool playerCoreUsbStorageMount() {
#if MWR_USB_MSC_SUPPORTED
    if ((s_usbStorageState != UsbStorageState::Idle && s_usbStorageState != UsbStorageState::Error) || s_usbStorageBusy) return false;
    stopSong(); // stop playback before handing the SD card to the host
    s_usbStorageBusy = true;
    usbMscSetModeFlag(true);
    const BaseType_t created = xTaskCreatePinnedToCore(usbStorageRebootToMscTask, "usbMscOn", 4096, nullptr, 2, nullptr, 0);
    if (created != pdPASS) {
        s_usbStorageBusy = false;
        usbMscSetModeFlag(false);
        s_usbStorageState = UsbStorageState::Error;
        MWR_LOG_ERROR("Failed to create USB MSC reboot task");
        return false;
    }
    return true;
#else
    s_usbStorageState = UsbStorageState::Unsupported;
    return false;
#endif
}

bool playerCoreUsbStorageUnmount() {
#if MWR_USB_MSC_SUPPORTED
    if (s_usbStorageState != UsbStorageState::Mounted || s_usbStorageBusy) return false;
    s_usbStorageBusy = true;
    const BaseType_t created = xTaskCreatePinnedToCore(usbStorageRebootToNormalTask, "usbMscOff", 4096, nullptr, 2, nullptr, 0);
    if (created != pdPASS) {
        s_usbStorageBusy = false;
        MWR_LOG_ERROR("Failed to create USB MSC reboot task");
        return false;
    }
    return true;
#else
    s_usbStorageState = UsbStorageState::Unsupported;
    return false;
#endif
}

static void processUsbStorage() {
#if MWR_USB_MSC_SUPPORTED
    if (s_usbStorageState == UsbStorageState::Mounted && s_usbStorageHostEjected && !s_usbStorageBusy) {
        playerCoreUsbStorageUnmount();
    }
#endif
    if (s_usbStorageState == UsbStorageState::Scanning && !s_localLibraryScanning) {
        s_usbStorageState = UsbStorageState::Idle;
    }
}

// ---- Synchronized lyrics (ID3v2 SYLT frame) ------------------------------
// Only millisecond-timestamped SYLT (time_stamp_format=2) is supported --
// the MPEG-frame-count format exists but is rare in practice and would need
// the actual frame rate to convert, which isn't readily available here.
// Parsed lazily for whichever track is currently playing (like cover art),
// not cached for the whole library.
struct LyricLine {
    uint32_t ms = 0;
    char text[160]{}; // was 80 -- real .lrc/lrclib lines routinely exceed that once CJK (3 bytes/char in UTF-8)
                       // or UTF-16-encoded SYLT text is accounted for, and got silently truncated
};
constexpr uint16_t kMaxLyricLines = 300;
static LyricLine* s_lyricLines = nullptr;
static uint16_t s_lyricCount = 0;
static uint16_t s_lyricTrackIndex = 0xFFFF; // which track s_lyricLines holds, avoids re-parsing every call

// Tracks the online-fetch lifecycle for whichever track s_lyricTrackIndex is
// currently on, so the UI can show something more useful than a blanket
// "没有歌词信息" while a lookup is in flight or after one has failed (see
// HifiUi::refreshLocalNowPlaying()). Idle: no lookup needed/started yet
// (local lyrics found, or track just changed). Pending: queued or running
// on lyricsFetchTask. Found/NotFound: lyricsFetchTask finished.
// Type itself lives in player_service.h (already #included above) since the
// UI side needs it too -- this file just uses it, doesn't redefine it.
static LyricFetchState s_lyricFetchState = LyricFetchState::Idle;

// Guards s_lyricLines/s_lyricCount/s_lyricTrackIndex, which playerCoreLoadLyrics()
// (LVGL-tick thread) and lyricsFetchTask() (background network fetch) can now
// both touch -- without this, a track change freeing s_lyricLines while the
// fetch task is mid-read/adopt would be a use-after-free.
static SemaphoreHandle_t s_lyricsMutex = nullptr;
static QueueHandle_t s_lyricsFetchQueue = nullptr; // length 1, holds a pending track index (xQueueOverwrite)
static volatile uint16_t s_lyricsOnlineReadyIndex = 0xFFFF; // consume-once signal for the UI (see playerCoreLyricsOnlineReady)

// GBK is a superset of GB2312, common for Chinese/Japanese .lrc files
// downloaded from lyric sites -- decodes a 2-byte GBK sequence via
// gbk_table (see gbk_table.h) into its Unicode codepoint, then encodes that
// as UTF-8. Ascii passes through unchanged.
static size_t gbkToUtf8(const uint8_t* data, size_t len, char* out, size_t outSize) {
    size_t outPos = 0;
    size_t i = 0;
    while (i < len && outPos + 3 < outSize) {
        const uint8_t c = data[i];
        if (c < 0x80) {
            out[outPos++] = static_cast<char>(c);
            ++i;
            continue;
        }
        if (i + 1 >= len || c < 0x81 || c > 0xFE) { ++i; continue; } // stray/invalid lead byte, skip
        const uint8_t trail = data[i + 1];
        if (trail < 0x40 || trail == 0x7F || trail > 0xFE) { ++i; continue; }
        const uint32_t trailIdx = trail < 0x7F ? (trail - 0x40) : (trail - 0x41);
        const uint32_t idx = (static_cast<uint32_t>(c) - 0x81) * 190 + trailIdx;
        const uint16_t cp = idx < 23940 ? gbk_table[idx] : 0xFFFD;
        i += 2;
        if (cp == 0xFFFD) continue;
        if (cp < 0x80) {
            out[outPos++] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            out[outPos++] = static_cast<char>(0xC0 | (cp >> 6));
            out[outPos++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out[outPos++] = static_cast<char>(0xE0 | (cp >> 12));
            out[outPos++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[outPos++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    out[outPos] = '\0';
    return outPos;
}

// Parses a "[mm:ss.xx]" timestamp starting right after '[' (p points at the
// first digit). Fractional-second digit count is interpreted as tenths(1)/
// hundredths(2)/milliseconds(3+), since real .lrc files in the wild mix
// digit counts even within one file. Returns the position right after ']',
// or nullptr if this bracket isn't a valid timestamp (a metadata tag like
// "[ti:...]" or malformed).
static const char* parseLrcTimestamp(const char* p, uint32_t* outMs) {
    if (!isdigit(static_cast<unsigned char>(*p))) return nullptr;
    uint32_t mm = 0;
    while (isdigit(static_cast<unsigned char>(*p))) { mm = mm * 10 + (*p - '0'); ++p; }
    if (*p != ':') return nullptr;
    ++p;
    if (!isdigit(static_cast<unsigned char>(*p))) return nullptr;
    uint32_t ss = 0;
    while (isdigit(static_cast<unsigned char>(*p))) { ss = ss * 10 + (*p - '0'); ++p; }
    uint32_t frac = 0, fracDigits = 0;
    if (*p == '.') {
        ++p;
        while (isdigit(static_cast<unsigned char>(*p))) {
            frac = frac * 10 + (*p - '0');
            ++fracDigits;
            ++p;
        }
    }
    if (*p != ']') return nullptr;
    ++p;
    uint32_t fracMs = 0;
    if (fracDigits == 1) fracMs = frac * 100;
    else if (fracDigits == 2) fracMs = frac * 10;
    else if (fracDigits >= 3) fracMs = frac;
    *outMs = mm * 60000 + ss * 1000 + fracMs;
    return p;
}

// External .lrc sidecar (same basename as the mp3, ".lrc" extension) --
// more common in practice than embedded ID3 SYLT, and what real lyric-site
// downloads use. Auto-detects GBK vs UTF-8 per file (looksLikeValidUtf8())
// since both show up in the wild and neither declares itself.
// Builds the .lrc sidecar path for a track (same basename, ".lrc" instead
// of the audio extension) -- shared by the on-disk loader and the online
// fetch's save-to-SD step, so both always agree on where a track's lyrics
// live.
static void deriveLrcPath(const char* mp3Path, char* outPath, size_t outSize) {
    strlcpy(outPath, mp3Path, outSize);
    char* dot = strrchr(outPath, '.');
    if (dot) *dot = '\0';
    strlcat(outPath, ".lrc", outSize);
}

// Parses raw LRC text (from either a file read or a downloaded lyrics
// string) into `lines` (caller-allocated, capacity maxLines), decoding
// GBK-or-UTF8 per line as auto-detected by the caller. Returns the count
// written; caller still needs to sort by timestamp (multi-timestamp lines
// and authoring quirks can leave raw output out of order).
// Copies up to outSize-1 bytes of a UTF-8 string, but if the byte-count cap
// would land in the middle of a multi-byte sequence, backs off to drop that
// whole (incomplete) character instead of cutting it in half. A plain
// memcpy+truncate (what this replaced) could leave a lead byte with no -- or
// only some of -- its continuation bytes, which isn't valid UTF-8 and is
// exactly what LVGL renders as a tofu box. This is what was actually behind
// "部分字方框乱码" in long lyric lines, not a missing font glyph.
static void utf8SafeCopy(const char* src, size_t srcLen, char* out, size_t outSize) {
    size_t n = std::min(srcLen, outSize - 1);
    // Walk back over continuation bytes (10xxxxxx) to find where the last
    // (possibly truncated) character starts.
    size_t leadPos = n;
    while (leadPos > 0 && (static_cast<uint8_t>(src[leadPos - 1]) & 0xC0) == 0x80) --leadPos;
    if (leadPos > 0) {
        const uint8_t lead = static_cast<uint8_t>(src[leadPos - 1]);
        size_t seqLen = 1;
        if ((lead & 0xE0) == 0xC0) seqLen = 2;
        else if ((lead & 0xF0) == 0xE0) seqLen = 3;
        else if ((lead & 0xF8) == 0xF0) seqLen = 4;
        // That lead byte's full sequence spans [leadPos-1, leadPos-1+seqLen).
        // If it doesn't fit within n, the cut landed inside it -- drop the
        // whole character rather than keep a partial one.
        if ((leadPos - 1) + seqLen > n) n = leadPos - 1;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

static uint16_t parseLrcBuffer(const uint8_t* raw, size_t readLen, bool isUtf8, LyricLine* lines, uint16_t maxLines) {
    uint16_t count = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i <= readLen && count < maxLines; ++i) {
        if (i == readLen || raw[i] == '\n') {
            size_t lineEnd = i;
            while (lineEnd > lineStart && (raw[lineEnd - 1] == '\r' || raw[lineEnd - 1] == ' ')) --lineEnd;
            const char* p = reinterpret_cast<const char*>(raw + lineStart);
            const char* lineEndPtr = reinterpret_cast<const char*>(raw + lineEnd);

            uint32_t timestamps[8];
            uint8_t tsCount = 0;
            while (p < lineEndPtr && *p == '[' && tsCount < 8) {
                uint32_t ms;
                const char* next = parseLrcTimestamp(p + 1, &ms);
                if (!next) {
                    const char* close = static_cast<const char*>(memchr(p, ']', lineEndPtr - p));
                    if (!close) { p = lineEndPtr; break; } // malformed tag, bail this line
                    p = close + 1;
                    continue; // metadata tag ([ti:...] etc) -- keep scanning, don't count as a timestamp
                }
                timestamps[tsCount++] = ms;
                p = next;
            }
            if (tsCount > 0 && p < lineEndPtr) {
                // Sized to match LyricLine::text (160 bytes) -- was 80 and,
                // separately from the UTF-8 boundary bug above, silently
                // truncating any line longer than that regardless.
                char text[160];
                if (isUtf8) {
                    utf8SafeCopy(p, lineEndPtr - p, text, sizeof(text));
                } else {
                    gbkToUtf8(reinterpret_cast<const uint8_t*>(p), lineEndPtr - p, text, sizeof(text));
                }
                for (uint8_t t = 0; t < tsCount && count < maxLines; ++t) {
                    lines[count].ms = timestamps[t];
                    strlcpy(lines[count].text, text, sizeof(lines[count].text));
                    ++count;
                }
            }
            lineStart = i + 1;
        }
    }
    return count;
}

static bool loadLrcSidecar(const char* mp3Path) {
    char lrcPath[176];
    deriveLrcPath(mp3Path, lrcPath, sizeof(lrcPath));
    if (!SD_MMC.exists(lrcPath)) return false;

    File file = SD_MMC.open(lrcPath, "r");
    if (!file) return false;
    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > 256UL * 1024) { // sanity cap, real lyric files are a few KB
        file.close();
        return false;
    }
    uint8_t* raw = static_cast<uint8_t*>(ps_malloc(fileSize + 1));
    if (!raw) {
        file.close();
        return false;
    }
    const size_t readLen = file.read(raw, fileSize);
    file.close();
    raw[readLen] = 0;

    const bool isUtf8 = looksLikeValidUtf8(raw, readLen);
    LyricLine* lines = static_cast<LyricLine*>(ps_calloc(kMaxLyricLines, sizeof(LyricLine)));
    if (!lines) {
        free(raw);
        return false;
    }
    const uint16_t count = parseLrcBuffer(raw, readLen, isUtf8, lines, kMaxLyricLines);
    free(raw);

    if (count == 0) {
        free(lines);
        return false;
    }
    std::sort(lines, lines + count, [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    s_lyricLines = lines;
    s_lyricCount = count;
    return true;
}

// ---- Online lyrics fetch (lrclib.net) ------------------------------------
// Runs only when a track has neither a local .lrc sidecar nor embedded
// SYLT lyrics (see playerCoreLoadLyrics) and WiFi is up. lrclib.net is a
// free, keyless, open lyrics database purpose-built for synced LRC lookup.

static void urlEncodeAppend(String& url, const char* s) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        const unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') url += static_cast<char>(c);
        else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 0xF];
        }
    }
}

// Extracts and unescapes a top-level JSON string field "key":"value" from
// body. Handles the escapes a typical JSON serializer emits (\", \\, \/,
// \n, \r, \t, \uXXXX -- BMP only, re-encoded to UTF-8); raw non-ASCII bytes
// pass through unescaped since lyric text is served as literal UTF-8, not
// \u-escaped. Returns false if the key is absent or its value isn't a
// string (e.g. JSON null for an instrumental track's syncedLyrics).
static bool jsonStringField(const String& body, const char* key, String& out) {
    const String needle = String("\"") + key + "\":";
    int pos = body.indexOf(needle);
    if (pos < 0) return false;
    pos += needle.length();
    while (pos < static_cast<int>(body.length()) && isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= static_cast<int>(body.length()) || body[pos] != '"') return false;
    ++pos;
    out = "";
    while (pos < static_cast<int>(body.length()) && body[pos] != '"') {
        const char c = body[pos];
        if (c == '\\' && pos + 1 < static_cast<int>(body.length())) {
            const char e = body[pos + 1];
            if (e == 'n') { out += '\n'; pos += 2; continue; }
            if (e == 'r') { out += '\r'; pos += 2; continue; }
            if (e == 't') { out += '\t'; pos += 2; continue; }
            if (e == '"') { out += '"'; pos += 2; continue; }
            if (e == '\\') { out += '\\'; pos += 2; continue; }
            if (e == '/') { out += '/'; pos += 2; continue; }
            if (e == 'u' && pos + 5 < static_cast<int>(body.length())) {
                char hex[5] = {body[pos + 2], body[pos + 3], body[pos + 4], body[pos + 5], 0};
                const uint32_t cp = strtoul(hex, nullptr, 16);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate of a non-BMP pair (emoji etc.): expect
                    // a following \uDC00-\uDFFF low surrogate and combine
                    // them into one code point encoded as 4-byte UTF-8.
                    // Without this, each surrogate half got encoded as a
                    // 3-byte sequence -> invalid UTF-8 -> garbled text.
                    if (pos + 11 < static_cast<int>(body.length()) && body[pos + 6] == '\\' && body[pos + 7] == 'u') {
                        char lowHex[5] = {body[pos + 8], body[pos + 9], body[pos + 10], body[pos + 11], 0};
                        const uint32_t low = strtoul(lowHex, nullptr, 16);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            const uint32_t combined = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            out += static_cast<char>(0xF0 | (combined >> 18));
                            out += static_cast<char>(0x80 | ((combined >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((combined >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (combined & 0x3F));
                            pos += 12;
                            continue;
                        }
                    }
                    // Lone/invalid surrogate: drop it rather than emit bad UTF-8.
                    pos += 6;
                    continue;
                }
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                pos += 6;
                continue;
            }
        }
        out += c;
        ++pos;
    }
    return true;
}

// Issues one lrclib.net request and, on success, parses+saves+adopts the
// lyrics -- shared by both the exact /api/get lookup (single JSON object)
// and the fuzzy /api/search lookup (JSON array): jsonStringField() just
// finds the first top-level "syncedLyrics" field in the body either way,
// which for /search is whichever result the API ranked first/most
// relevant. Extracted from what used to be fetchLyricsOnline()'s entire
// body, now that it tries several URLs per track (see fetchLyricsOnline()).
static bool tryLyricsRequest(const String& url, const TrackRecord& item, uint16_t index) {
    printf("[LYRICS] GET %s\n", url.c_str());
    WiFiClientSecure client;
    client.setInsecure(); // no cert pinning, same tradeoff as the weather fetch
    HTTPClient http;
    if (!http.begin(client, url)) return false;
    http.setTimeout(8000);
    http.addHeader("User-Agent", "MiniWebRadio-HiFi/1.0 (github.com, ESP32-S3 device)");
    const int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        const String body = http.getString();
        printf("[LYRICS] HTTP %d, body[0..200]=%s\n", code, body.substring(0, 200).c_str());
        String synced;
        if (jsonStringField(body, "syncedLyrics", synced) && synced.length() > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(synced.c_str());
            const size_t rawLen = synced.length();
            const bool isUtf8 = looksLikeValidUtf8(raw, rawLen); // API text is UTF-8; kept for consistency with the file path
            LyricLine* lines = static_cast<LyricLine*>(ps_calloc(kMaxLyricLines, sizeof(LyricLine)));
            if (lines) {
                uint16_t count = parseLrcBuffer(raw, rawLen, isUtf8, lines, kMaxLyricLines);
                if (count > 0) {
                    std::sort(lines, lines + count, [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
                    char lrcPath[176];
                    deriveLrcPath(item.path, lrcPath, sizeof(lrcPath));
                    File out = SD_MMC.open(lrcPath, "w", true);
                    if (out) {
                        out.print(synced);
                        out.close();
                    }
                    if (s_lyricsMutex) xSemaphoreTake(s_lyricsMutex, portMAX_DELAY);
                    // Only adopt into the live cache if it's still tracking
                    // this same index -- if the user skipped to another
                    // track during the slow network round trip, a later
                    // playerCoreLoadLyrics() call already moved
                    // s_lyricTrackIndex on, and adopting here would clobber
                    // whatever that call loaded.
                    if (s_lyricTrackIndex == index) {
                        if (s_lyricLines) free(s_lyricLines);
                        s_lyricLines = lines;
                        s_lyricCount = count;
                        lines = nullptr; // ownership transferred
                    }
                    if (s_lyricsMutex) xSemaphoreGive(s_lyricsMutex);
                    ok = true;
                }
                if (lines) free(lines); // not adopted, or count==0
            }
        }
    } else {
        printf("[LYRICS] HTTP GET failed, code=%d\n", code);
    }
    http.end();
    return ok;
}

// ---- Radio station logos (radio-browser.info) -----------------------------
// One JPEG cache file per station on SD (/logo/radio_<index>.jpg) -- fetched
// on demand, never at boot. radio-browser's own favicon field is frequently
// PNG/ICO, which the on-device decoder (tftLib's JPEG-only decodeJpgFromMemory,
// same one local cover art uses) can't handle; those simply fail to decode
// later and the UI falls back to the generic RADIO glyph, same as "no logo
// found" -- there's no format sniffing/conversion here, just graceful failure.
static volatile bool s_radioIconSyncInProgress = false;

static void radioIconPath(uint16_t index, char* out, size_t outSize) { snprintf(out, outSize, "/logo/radio_%u.jpg", index); }

// Downloads url's response body straight into an SD file, capped at maxBytes
// so one absurdly large "favicon" can't fill the card. Returns false (and
// leaves no partial file behind) on any error.
static bool httpDownloadToFile(const String& url, const char* sdPath, size_t maxBytes) {
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    HTTPClient http;
    const bool isHttps = url.startsWith("https://");
    if (isHttps) secureClient.setInsecure();
    const bool began = isHttps ? http.begin(secureClient, url) : http.begin(plainClient, url);
    if (!began) {
        printf("[ICON] download begin() failed url=%s\n", url.c_str());
        return false;
    }
    http.setTimeout(8000);
    // Placeholder/theme-photo services (loremflickr.com in particular) hand
    // out the actual image via a 301/302 to their CDN -- HTTPClient doesn't
    // follow redirects by default (HTTPC_DISABLE_FOLLOW_REDIRECTS), which
    // read as plain download failures until this was traced via [ICON] logs.
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent", "MiniWebRadio-HiFi/1.0 (github.com, ESP32-S3 device)");
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        printf("[ICON] download GET failed url=%s httpCode=%d free=%u\n", url.c_str(), code, (unsigned)ESP.getFreeHeap());
        http.end();
        return false;
    }
    WiFiClient* stream = http.getStreamPtr();
    File out = SD_MMC.open(sdPath, "w", true);
    if (!out) {
        printf("[ICON] download SD open failed path=%s\n", sdPath);
        http.end();
        return false;
    }
    uint8_t buf[512];
    size_t total = 0;
    bool ok = true;
    uint32_t lastDataMs = millis();
    while (http.connected() && total < maxBytes) {
        const size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastDataMs > 8000) break; // stream stalled
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t want = std::min(avail, sizeof(buf));
        const size_t got = stream->readBytes(buf, std::min(want, maxBytes - total));
        if (!got) break;
        out.write(buf, got);
        total += got;
        lastDataMs = millis();
    }
    out.close();
    http.end();
    if (total == 0) {
        printf("[ICON] download got 0 bytes url=%s httpCode=%d\n", url.c_str(), code);
        SD_MMC.remove(sdPath);
        ok = false;
    } else {
        printf("[ICON] download ok url=%s bytes=%u%s\n", url.c_str(), (unsigned)total, total >= maxBytes ? " (capped)" : "");
    }
    return ok;
}

// Retries a download twice (one retry after a short backoff) -- covers the
// occasional transient TLS/network hiccup seen in testing (a station's real
// favicon failing once, then succeeding a moment later on an identical
// request). Doesn't help a genuinely-empty/dead URL (station.country field
// staying 0 bytes every time), only actual flakiness.
static bool httpDownloadToFileRetrying(const String& url, const char* sdPath, size_t maxBytes) {
    if (httpDownloadToFile(url, sdPath, maxBytes)) return true;
    vTaskDelay(pdMS_TO_TICKS(300));
    return httpDownloadToFile(url, sdPath, maxBytes);
}

// Picks a single ASCII keyword to drive the Level-1 theme-photo fallback
// (see fetchOneStationIcon): radio-browser's own "tags" field first (its
// genre tags, e.g. "jazz,lounge", are exactly the kind of word a stock-photo
// keyword search wants), then the first ASCII word in the station name
// itself (works for "Jazz Sakura" -> "Jazz"; a Chinese/Japanese-only name
// has no ASCII run and falls through), and finally a generic "radio" so
// this always returns something.
static String pickThemeKeyword(const String& tags, const char* name) {
    // A single ASCII word only -- loremflickr's path segment is a bare
    // keyword (or comma-separated keywords), and a raw space in there (e.g.
    // radio-browser's own multi-word tags like "classic jazz") makes an
    // invalid URL and comes back 400. Taking just the first word of
    // whichever source we use sidesteps encoding it at all.
    const int comma = tags.indexOf(',');
    const String firstTag = comma >= 0 ? tags.substring(0, comma) : tags;
    String word;
    for (size_t i = 0; i < firstTag.length(); ++i) {
        const char c = firstTag[i];
        if (isalpha(static_cast<uint8_t>(c))) {
            word += c;
        } else if (word.length() >= 2) {
            break;
        } else {
            word = "";
        }
    }
    if (word.length() >= 2) return word;

    word = "";
    for (const char* p = name; *p; ++p) {
        if (isalpha(static_cast<uint8_t>(*p))) {
            word += *p;
        } else if (word.length() >= 3) {
            break;
        } else {
            word = ""; // false start (too short) -- keep scanning for a real word
        }
    }
    if (word.length() >= 2) return word;
    return "radio";
}

// One station: radio-browser's byname search, first result's "favicon"
// field, then download that image straight to this station's cache path.
// Falls back through two levels if the real favicon is missing or won't
// download: a themed stock photo keyed by genre/name (Level 1), then --
// entirely offline, no further network attempt -- the UI itself draws a
// colored monogram tile (Level 2, see HifiUi's station-icon fallback
// rendering). This function only ever handles Level 0/1; returning false
// here just means "nothing to cache", which the UI already treats as "draw
// the fallback".
static bool fetchOneStationIcon(uint16_t index) {
    RadioStationItem item{};
    if (!playerCoreRadioStation(index, &item) || !item.name[0]) return false;

    String encodedName;
    for (const char* p = item.name; *p; ++p) {
        const uint8_t c = static_cast<uint8_t>(*p);
        if (isalnum(c)) encodedName += static_cast<char>(c);
        else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", c);
            encodedName += esc;
        }
    }
    const String searchUrl = "https://de1.api.radio-browser.info/json/stations/byname/" + encodedName + "?limit=1";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, searchUrl)) {
        printf("[ICON] #%u \"%s\" search begin() failed\n", index, item.name);
        return false;
    }
    http.setTimeout(8000);
    http.addHeader("User-Agent", "MiniWebRadio-HiFi/1.0 (github.com, ESP32-S3 device)");
    const int code = http.GET();
    String favicon, tags;
    if (code == HTTP_CODE_OK) {
        const String body = http.getString();
        jsonStringField(body, "favicon", favicon);
        jsonStringField(body, "tags", tags);
    }
    http.end();

    char sdPath[48];
    radioIconPath(index, sdPath, sizeof(sdPath));

    if (favicon.length() > 0) {
        printf("[ICON] #%u \"%s\" favicon=%s\n", index, item.name, favicon.c_str());
        if (httpDownloadToFileRetrying(favicon, sdPath, 96 * 1024)) return true;
        printf("[ICON] #%u real favicon failed (incl. retry), trying theme photo\n", index);
    } else {
        printf("[ICON] #%u \"%s\" no favicon (searchHttpCode=%d)\n", index, item.name, code);
    }

    // Level 1: no real logo -- try a themed stock photo instead of leaving
    // this station with nothing. loremflickr.com serves a real (Flickr,
    // public-domain-ish) photo for a given keyword, always as a JPEG, which
    // is exactly what the on-device decoder needs -- no format-sniffing
    // required, unlike radio-browser's favicon field.
    const String keyword = pickThemeKeyword(tags, item.name);
    const String themeUrl = "https://loremflickr.com/300/300/" + keyword;
    printf("[ICON] #%u theme fallback keyword=\"%s\" url=%s\n", index, keyword.c_str(), themeUrl.c_str());
    if (httpDownloadToFileRetrying(themeUrl, sdPath, 96 * 1024)) return true;

    printf("[ICON] #%u theme fallback also failed -- UI will draw an offline monogram\n", index);
    return false;
}

static void radioIconSyncTask(void*) {
    if (!SD_MMC.exists("/logo")) SD_MMC.mkdir("/logo");
    const uint16_t count = staMgnt.getSumStations();
    for (uint16_t i = 1; i <= count; ++i) {
        char sdPath[48];
        radioIconPath(i, sdPath, sizeof(sdPath));
        if (SD_MMC.exists(sdPath)) continue; // already cached (or a prior attempt already left a marker -- see below)
        if (!WiFi.isConnected()) break;       // no point continuing a sync that can't reach the network
        if (!fetchOneStationIcon(i)) {
            // Leave a zero-byte marker so a station with no findable/valid
            // logo isn't re-queried (and re-fails) every single time this
            // list is opened -- SD_MMC.exists() above treats it the same
            // as a real cached file, and the decode step below already
            // handles "exists but 0 bytes" as "no icon" gracefully.
            File marker = SD_MMC.open(sdPath, "w", true);
            if (marker) marker.close();
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // don't hammer the API/network back to back
    }
    s_radioIconSyncInProgress = false;
    vTaskDelete(nullptr);
}

void playerCoreRadioIconSyncStart() {
    if (s_radioIconSyncInProgress) return;
    s_radioIconSyncInProgress = true;
    if (xTaskCreatePinnedToCore(radioIconSyncTask, "radioIcon", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        s_radioIconSyncInProgress = false;
        MWR_LOG_ERROR("Failed to create radio icon sync task");
    }
}

bool playerCoreRadioIconSyncInProgress() { return s_radioIconSyncInProgress; }

bool playerCoreDecodeRadioIcon(uint16_t index, uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth, uint16_t* outHeight) {
    *outPixels = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    char sdPath[48];
    radioIconPath(index, sdPath, sizeof(sdPath));
    File in = SD_MMC.open(sdPath, "r");
    if (!in) return false;
    const size_t len = in.size();
    if (len == 0) { // zero-byte marker -- "no icon available", not an error
        in.close();
        return false;
    }
    uint8_t* data = static_cast<uint8_t*>(ps_malloc(len));
    if (!data) {
        in.close();
        return false;
    }
    const size_t readLen = in.read(data, len);
    in.close();
    if (readLen != len) {
        free(data);
        return false;
    }
    const bool ok = getTFT().decodeJpgFromMemory(data, len, scaleFactor, outPixels, outWidth, outHeight);
    free(data);
    return ok;
}

// Strips a trailing " (...)" parenthetical -- e.g. a Chinese translation
// bolted onto a Japanese title by whoever tagged the file, "俺の彼女 (我的
// 女友)" -- to get a cleaner query. lrclib.net's database has the bare
// original title; an exact-match /api/get on the annotated version never
// hits since the annotation isn't part of the real track name. Returns
// false (leaving out untouched) if there's no parenthetical to strip.
static bool stripParentheticalSuffix(const char* title, char* out, size_t outSize) {
    const char* paren = strchr(title, '(');
    if (!paren || paren == title) return false;
    size_t len = paren - title;
    while (len > 0 && title[len - 1] == ' ') --len; // trim the space before '('
    if (len == 0 || len >= outSize) return false;
    memcpy(out, title, len);
    out[len] = '\0';
    return true;
}

// Fetches synced lyrics for s_localTracks[index] from lrclib.net, saves them
// as a .lrc sidecar on SD (so future plays don't need the network again),
// and adopts them into the live s_lyricLines cache if the user hasn't since
// moved on to a different track. Runs on lyricsFetchTask, never the UI
// thread -- this does a real multi-second HTTP round trip (up to 4, see
// below, in the worst case where nothing matches at all).
//
// Tries the original ID3 title first, then (if it has a "(...)" suffix) a
// cleaned-up short title -- each against both lrclib.net's exact-match
// /api/get and its fuzzy /api/search, in that order. This was added after
// real-world testing showed most "not found" results were locally-tagged
// titles that simply don't match lrclib's database character-for-character
// (a translation annotation, or -- for tracks with no real ID3 title at
// all, just the bare filename -- nothing usably close to the real title),
// not a bug in the request itself.
static bool fetchLyricsOnline(uint16_t index) {
    if (!s_localTracks || index >= s_localTrackCount) return false;
    if (!WiFi.isConnected()) return false;
    const TrackRecord& item = s_localTracks[index];
    if (!item.title[0]) return false;

    char shortTitle[64];
    const bool hasShortTitle = stripParentheticalSuffix(item.title, shortTitle, sizeof(shortTitle));
    const char* titles[2] = {item.title, hasShortTitle ? shortTitle : nullptr};

    for (uint8_t t = 0; t < 2; ++t) {
        const char* title = titles[t];
        if (!title) continue;

        String getUrl = "https://lrclib.net/api/get?track_name=";
        urlEncodeAppend(getUrl, title);
        if (item.artist[0]) {
            getUrl += "&artist_name=";
            urlEncodeAppend(getUrl, item.artist);
        }
        if (item.album[0]) {
            getUrl += "&album_name=";
            urlEncodeAppend(getUrl, item.album);
        }
        if (tryLyricsRequest(getUrl, item, index)) return true;

        String query = title;
        if (item.artist[0]) {
            query += " ";
            query += item.artist;
        }
        String searchUrl = "https://lrclib.net/api/search?q=";
        urlEncodeAppend(searchUrl, query.c_str());
        if (tryLyricsRequest(searchUrl, item, index)) return true;
    }
    return false;
}

static void lyricsFetchTask(void*) {
    uint16_t index;
    for (;;) {
        if (xQueueReceive(s_lyricsFetchQueue, &index, portMAX_DELAY) == pdTRUE) {
            const bool ok = fetchLyricsOnline(index);
            printf("[LYRICS] online fetch idx=%u %s\n", index, ok ? "found" : "not found");
            // Only updates the state the UI shows if the user is still on this
            // same track -- a later playerCoreLoadLyrics() for a different
            // track already reset s_lyricFetchState for that new track, and
            // this would otherwise stomp it right back to Found/NotFound.
            if (s_lyricTrackIndex == index) s_lyricFetchState = ok ? LyricFetchState::Found : LyricFetchState::NotFound;
            s_lyricsOnlineReadyIndex = index; // consumed once by playerCoreLyricsOnlineReady()
        }
    }
}

// Consume-once signal: true (and clears the pending flag) exactly once per
// completed background fetch, whether or not lyrics were actually found --
// the UI uses this to know when to re-check playerCoreLoadLyrics()'s cache
// for a track it's still displaying, rather than polling the network state
// directly.
bool playerCoreLyricsOnlineReady(uint16_t* outIndex) {
    if (s_lyricsOnlineReadyIndex == 0xFFFF) return false;
    *outIndex = s_lyricsOnlineReadyIndex;
    s_lyricsOnlineReadyIndex = 0xFFFF;
    return true;
}

bool playerCoreLoadLyrics(uint16_t index) {
    if (!s_localTracks || index >= s_localTrackCount) return false;
    if (s_lyricsMutex) xSemaphoreTake(s_lyricsMutex, portMAX_DELAY);
    if (s_lyricTrackIndex == index) {
        const bool cached = s_lyricCount > 0; // already parsed (or already known to have none)
        if (s_lyricsMutex) xSemaphoreGive(s_lyricsMutex);
        return cached;
    }
    if (s_lyricLines) {
        free(s_lyricLines);
        s_lyricLines = nullptr;
    }
    s_lyricCount = 0;
    s_lyricTrackIndex = index;
    s_lyricFetchState = LyricFetchState::Idle;
    if (s_lyricsMutex) xSemaphoreGive(s_lyricsMutex);

    if (loadLrcSidecar(s_localTracks[index].path)) {
        printf("[LYRICS] idx=%u lines=%u (source=.lrc)\n", index, s_lyricCount);
        s_lyricFetchState = LyricFetchState::Found;
        return true;
    }

    File file = SD_MMC.open(s_localTracks[index].path, "r");
    if (!file) return false;

    uint8_t header[10];
    bool found = false;
    if (file.read(header, 10) == 10 && memcmp(header, "ID3", 3) == 0) {
        const uint8_t majorVersion = header[3];
        const uint32_t tagEnd = 10 + readSynchsafe32(&header[6]);
        const bool synchsafeFrameSize = majorVersion >= 4;
        const size_t idLen = majorVersion >= 3 ? 4 : 3;
        const size_t frameHeaderLen = majorVersion >= 3 ? 10 : 6;

        uint32_t pos = 10;
        uint8_t frameHeader[10];
        while (!found && pos + frameHeaderLen <= tagEnd) {
            if (file.read(frameHeader, frameHeaderLen) != frameHeaderLen) break;
            char frameId[5] = {0, 0, 0, 0, 0};
            memcpy(frameId, frameHeader, idLen);
            if (frameId[0] == 0) break;

            uint32_t frameSize;
            if (majorVersion >= 3) frameSize = synchsafeFrameSize ? readSynchsafe32(&frameHeader[4]) : readBE32(&frameHeader[4]);
            else frameSize = (uint32_t(frameHeader[3]) << 16) | (uint32_t(frameHeader[4]) << 8) | frameHeader[5];
            pos += frameHeaderLen;
            if (frameSize == 0 || pos + frameSize > tagEnd) break;

            const uint32_t frameStart = file.position();
            if (strcmp(frameId, "SYLT") == 0) {
                uint8_t encoding = 0, lang[3], tsFormat = 0, contentType = 0;
                file.read(&encoding, 1);
                file.read(lang, 3);
                file.read(&tsFormat, 1);
                file.read(&contentType, 1);
                // skip content descriptor (encoding-dependent null terminator)
                if (encoding == 1 || encoding == 2) {
                    uint8_t pair[2] = {1, 1};
                    while (file.read(pair, 2) == 2 && (pair[0] != 0 || pair[1] != 0)) {}
                } else {
                    uint8_t c = 1;
                    while (file.read(&c, 1) == 1 && c != 0) {}
                }
                if (tsFormat == 2) { // milliseconds; MPEG-frame-count format not supported
                    LyricLine* lines = static_cast<LyricLine*>(ps_calloc(kMaxLyricLines, sizeof(LyricLine)));
                    if (lines) {
                        uint16_t count = 0;
                        while (count < kMaxLyricLines && file.position() < frameStart + frameSize) {
                            // Sized for LyricLine::text's 160-byte output cap: UTF-16 encodings
                            // use 2 bytes/char, so 320 raw bytes covers every line that could
                            // possibly still fit after decodeId3Text's own truncation.
                            uint8_t textBuf[321];
                            size_t tlen = 0;
                            if (encoding == 1 || encoding == 2) {
                                uint8_t pair[2];
                                while (tlen + 2 < sizeof(textBuf) && file.read(pair, 2) == 2) {
                                    if (pair[0] == 0 && pair[1] == 0) break;
                                    textBuf[tlen++] = pair[0];
                                    textBuf[tlen++] = pair[1];
                                }
                            } else {
                                uint8_t c;
                                while (tlen < sizeof(textBuf) - 1 && file.read(&c, 1) == 1 && c != 0) textBuf[tlen++] = c;
                            }
                            uint8_t tsBytes[4];
                            if (file.read(tsBytes, 4) != 4) break;
                            // decodeId3Text expects [encoding][text...], reuse it for the
                            // per-line text same as any other ID3 text field.
                            uint8_t frameForDecode[322];
                            frameForDecode[0] = encoding;
                            memcpy(frameForDecode + 1, textBuf, tlen);
                            decodeId3Text(frameForDecode, tlen + 1, lines[count].text, sizeof(lines[count].text));
                            lines[count].ms = readBE32(tsBytes);
                            ++count;
                        }
                        if (count > 0) {
                            s_lyricLines = lines;
                            s_lyricCount = count;
                            found = true;
                        } else {
                            free(lines);
                        }
                    }
                } else {
                    // Real-world SYLT frames often use MPEG-frame-count
                    // timestamps (tsFormat=1) instead of milliseconds --
                    // this diagnostic exists because a track can show
                    // "audiofile contains synchronized lyrics" in the audio
                    // library's own log while still yielding 0 parsed lines
                    // here, which otherwise looks identical to "no lyrics".
                    printf("[LYRICS] SYLT found but tsFormat=%u unsupported (need 2=ms)\n", tsFormat);
                }
            }
            file.seek(frameStart + frameSize);
            pos += frameSize;
        }
    }
    file.close();
    printf("[LYRICS] idx=%u lines=%u\n", index, s_lyricCount);
    if (found) s_lyricFetchState = LyricFetchState::Found;

    // No lyrics found locally (neither .lrc sidecar nor embedded SYLT) --
    // ask lyricsFetchTask to try lrclib.net in the background. Enqueued,
    // not fetched inline, since this function runs on the LVGL/UI thread
    // and a multi-second HTTP round trip here would freeze the UI.
    if (!found) {
        if (WiFi.isConnected() && s_lyricsFetchQueue) {
            s_lyricFetchState = LyricFetchState::Pending;
            xQueueOverwrite(s_lyricsFetchQueue, &index);
        } else {
            s_lyricFetchState = LyricFetchState::NotFound; // no network to even try -- see playerCoreRetryLyricsFetch()
        }
    }
    return found;
}

// Re-queues an online lookup for the track currently on screen (see
// HifiUi::onLyricRetryAction) -- e.g. the first attempt had no WiFi yet, or
// lrclib.net simply doesn't have this track and the user wants to check
// again after correcting local tags. No-op if playback has since moved to
// a different track, or there's still nothing to fetch with.
void playerCoreRetryLyricsFetch(uint16_t index) {
    if (index != s_lyricTrackIndex || !WiFi.isConnected() || !s_lyricsFetchQueue) return;
    s_lyricFetchState = LyricFetchState::Pending;
    xQueueOverwrite(s_lyricsFetchQueue, &index);
}

// Idle/Pending/Found/NotFound as a plain uint8_t for the UI (player_service.h
// declares its own identical enum rather than including this file's).
// Returns Idle for any index other than s_lyricTrackIndex -- the state only
// really describes "whichever track is currently loaded".
uint8_t playerCoreLyricsFetchState(uint16_t index) {
    if (index != s_lyricTrackIndex) return static_cast<uint8_t>(LyricFetchState::Idle);
    return static_cast<uint8_t>(s_lyricFetchState);
}

// Returns the text of whichever lyric line is current at positionMs, or an
// empty string before the first line / with no lyrics loaded. Lines are
// assumed chronologically ordered in the file (standard for SYLT). Copies
// the matched text into a static buffer while holding s_lyricsMutex, rather
// than returning a pointer straight into s_lyricLines, since the background
// lyricsFetchTask can free/replace that array between calls.
const char* playerCoreCurrentLyricLine(uint32_t positionMs) {
    static char lineBuf[80];
    lineBuf[0] = '\0';
    if (s_lyricsMutex) xSemaphoreTake(s_lyricsMutex, portMAX_DELAY);
    if (s_lyricLines && s_lyricCount) {
        int32_t idx = -1;
        for (uint16_t i = 0; i < s_lyricCount; ++i) {
            if (s_lyricLines[i].ms <= positionMs) idx = i;
            else break;
        }
        if (idx >= 0) strlcpy(lineBuf, s_lyricLines[idx].text, sizeof(lineBuf));
    }
    if (s_lyricsMutex) xSemaphoreGive(s_lyricsMutex);
    return lineBuf;
}

// ---- Cloud Music (在线音乐 / 网易云) -- Render Music Gateway -------------
// Phase 2 scope only: config storage + gateway health/wake check, per the
// project's own phased NCM spec. No search, resolve, or playback wiring yet
// (that's phases 3-4) -- see services/ncm-gateway/README.md for the backend
// this talks to, and PlayerSource::CloudMusic's own comment (added in a
// later phase) for how this eventually plugs into the existing
// Radio/Sd mutual-exclusion in PlayerService.

static constexpr const char* kCloudBaseUrlPrefKey = "cm_base_url";
static constexpr const char* kCloudDeviceKeyPrefKey = "cm_dev_key";

static volatile CloudServiceState s_cloudServiceState = CloudServiceState::Unknown;
static SemaphoreHandle_t s_cloudMusicMutex = nullptr; // guards s_cloudConfig across the LVGL thread and cloudMusicWakeTask
static CloudMusicConfig s_cloudConfig;
static volatile bool s_cloudWakeTaskRunning = false;
static volatile uint32_t s_cloudLastReadyMs = 0; // millis() when s_cloudServiceState last became Ready
// Gateway config history (up to 5, newest first). Every saved/used config
// is upserted here; only playerCoreCloudMusicHistoryDelete() removes one.
// Stored in NVS as cm_h<i>u/k/t so it survives app-only reflashes.
static constexpr uint8_t kCloudHistoryMax = 5;
static CloudMusicHistoryEntry s_cloudHistory[kCloudHistoryMax];
static uint8_t s_cloudHistoryCount = 0;

static bool cloudMusicLock(TickType_t timeout = pdMS_TO_TICKS(250)) {
    return !s_cloudMusicMutex || xSemaphoreTake(s_cloudMusicMutex, timeout) == pdTRUE;
}
static void cloudMusicUnlock() {
    if (s_cloudMusicMutex) xSemaphoreGive(s_cloudMusicMutex);
}

// Loads the saved gateway URL/key from NVS into s_cloudConfig. Called once
// at boot (see setup()) -- afterwards s_cloudConfig is the source of truth,
// updated in lockstep with NVS by playerCoreSetCloudMusicConfig().
static void cloudMusicLoadConfig() {
    if (!lockPreferences()) return;
    const String baseUrl = pref.getString(kCloudBaseUrlPrefKey, "");
    const String deviceKey = pref.getString(kCloudDeviceKeyPrefKey, "");
    unlockPreferences();
    if (cloudMusicLock()) {
        strlcpy(s_cloudConfig.baseUrl, baseUrl.c_str(), sizeof(s_cloudConfig.baseUrl));
        strlcpy(s_cloudConfig.deviceKey, deviceKey.c_str(), sizeof(s_cloudConfig.deviceKey));
        s_cloudConfig.configured = s_cloudConfig.baseUrl[0] != '\0' && s_cloudConfig.deviceKey[0] != '\0';
        cloudMusicUnlock();
    }
}

CloudMusicConfig playerCoreCloudMusicConfig() {
    CloudMusicConfig out;
    if (cloudMusicLock()) {
        out = s_cloudConfig;
        cloudMusicUnlock();
    }
    // Render only serves https (plain http is a 301 stub) and our HTTPS
    // clients cannot follow an http->https redirect, so hand every consumer
    // a normalized https URL -- this also covers configs saved before this
    // normalization existed.
    if (strncmp(out.baseUrl, "http://", 7) == 0) {
        String tmp(out.baseUrl);
        tmp.remove(0, 7); // drop "http://"
        tmp = "https://" + tmp;
        strlcpy(out.baseUrl, tmp.c_str(), sizeof(out.baseUrl));
    }
    return out;
}

static void cloudMusicHistoryKey(uint8_t index, char suffix, char* out, size_t outSize) {
    snprintf(out, outSize, "cm_h%u%c", static_cast<unsigned>(index), suffix);
}

static void cloudMusicLoadHistory() {
    if (!lockPreferences()) return;
    uint8_t count = 0;
    for (uint8_t i = 0; i < kCloudHistoryMax; ++i) {
        char kUrl[16], kKey[16], kTs[16];
        cloudMusicHistoryKey(i, 'u', kUrl, sizeof(kUrl));
        cloudMusicHistoryKey(i, 'k', kKey, sizeof(kKey));
        cloudMusicHistoryKey(i, 't', kTs, sizeof(kTs));
        const String url = pref.getString(kUrl, "");
        const String key = pref.getString(kKey, "");
        if (url.isEmpty() || key.isEmpty()) continue;
        CloudMusicHistoryEntry& e = s_cloudHistory[count];
        strlcpy(e.baseUrl, url.c_str(), sizeof(e.baseUrl));
        strlcpy(e.deviceKey, key.c_str(), sizeof(e.deviceKey));
        e.lastUsedEpoch = pref.getUInt(kTs, 0);
        ++count;
    }
    unlockPreferences();
    s_cloudHistoryCount = count;
}

static void cloudMusicSaveHistory() {
    if (!lockPreferences(pdMS_TO_TICKS(1000))) return;
    for (uint8_t i = 0; i < kCloudHistoryMax; ++i) {
        char kUrl[16], kKey[16], kTs[16];
        cloudMusicHistoryKey(i, 'u', kUrl, sizeof(kUrl));
        cloudMusicHistoryKey(i, 'k', kKey, sizeof(kKey));
        cloudMusicHistoryKey(i, 't', kTs, sizeof(kTs));
        if (i < s_cloudHistoryCount) {
            pref.putString(kUrl, s_cloudHistory[i].baseUrl);
            pref.putString(kKey, s_cloudHistory[i].deviceKey);
            pref.putUInt(kTs, s_cloudHistory[i].lastUsedEpoch);
        } else {
            pref.remove(kUrl);
            pref.remove(kKey);
            pref.remove(kTs);
        }
    }
    unlockPreferences();
}

// Insert/refresh a gateway config in the history: same baseUrl refreshes the
// key + timestamp and moves to the front; a new URL is prepended; keep at
// most kCloudHistoryMax entries.
static void cloudMusicHistoryUpsert(const char* baseUrl, const char* deviceKey) {
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    uint8_t existing = kCloudHistoryMax;
    for (uint8_t i = 0; i < s_cloudHistoryCount; ++i) {
        if (strcmp(s_cloudHistory[i].baseUrl, baseUrl) == 0) { existing = i; break; }
    }
    if (existing == kCloudHistoryMax) {
        // Shift slots down to make room at the front; when full, the oldest
        // (last) slot is dropped. Never write past kCloudHistoryMax-1.
        const uint8_t shiftFrom = (s_cloudHistoryCount < kCloudHistoryMax) ? s_cloudHistoryCount : kCloudHistoryMax - 1;
        for (uint8_t i = shiftFrom; i > 0; --i) s_cloudHistory[i] = s_cloudHistory[i - 1];
        if (s_cloudHistoryCount < kCloudHistoryMax) ++s_cloudHistoryCount;
    } else {
        const CloudMusicHistoryEntry tmp = s_cloudHistory[existing];
        for (uint8_t i = existing; i > 0; --i) s_cloudHistory[i] = s_cloudHistory[i - 1];
        s_cloudHistory[0] = tmp;
    }
    strlcpy(s_cloudHistory[0].baseUrl, baseUrl, sizeof(s_cloudHistory[0].baseUrl));
    strlcpy(s_cloudHistory[0].deviceKey, deviceKey, sizeof(s_cloudHistory[0].deviceKey));
    s_cloudHistory[0].lastUsedEpoch = now;
    cloudMusicSaveHistory();
}

uint8_t playerCoreCloudMusicHistoryCount() { return s_cloudHistoryCount; }

bool playerCoreCloudMusicHistoryEntry(uint8_t index, CloudMusicHistoryEntry* out) {
    if (!out || index >= s_cloudHistoryCount) return false;
    *out = s_cloudHistory[index];
    return true;
}

bool playerCoreCloudMusicHistoryDelete(uint8_t index) {
    if (index >= s_cloudHistoryCount) return false;
    for (uint8_t i = index; i + 1 < s_cloudHistoryCount; ++i) s_cloudHistory[i] = s_cloudHistory[i + 1];
    --s_cloudHistoryCount;
    cloudMusicSaveHistory();
    return true;
}

// Trims whitespace and a trailing slash, requires an http(s) scheme and
// both fields non-empty -- rejecting an obviously-broken config here (rather
// than saving it and letting the first health check fail) means the
// settings UI can tell "you typed something wrong" apart from "the gateway
// is unreachable".
bool playerCoreSetCloudMusicConfig(const char* baseUrl, const char* deviceKey) {
    if (!baseUrl || !deviceKey) return false;
    String trimmedUrl(baseUrl);
    trimmedUrl.trim();
    while (trimmedUrl.endsWith("/")) trimmedUrl.remove(trimmedUrl.length() - 1);
    String trimmedKey(deviceKey);
    trimmedKey.trim();
    if (trimmedUrl.isEmpty() || trimmedKey.isEmpty()) return false;
    // Normalize to https://: Render only serves https (plain http is a 301
    // stub), and our HTTP clients (WiFiClientSecure) can't ride an
    // http->https redirect. Also covers typing the bare hostname.
    if (trimmedUrl.startsWith("http://")) {
        trimmedUrl.replace("http://", "https://");
    } else if (!trimmedUrl.startsWith("https://")) {
        trimmedUrl = "https://" + trimmedUrl;
    }
    if (trimmedUrl.length() >= sizeof(CloudMusicConfig::baseUrl)) return false;
    if (trimmedKey.length() >= sizeof(CloudMusicConfig::deviceKey)) return false;

    if (lockPreferences(pdMS_TO_TICKS(1000))) {
        pref.putString(kCloudBaseUrlPrefKey, trimmedUrl);
        pref.putString(kCloudDeviceKeyPrefKey, trimmedKey);
        unlockPreferences();
    }
    if (cloudMusicLock(pdMS_TO_TICKS(1000))) {
        strlcpy(s_cloudConfig.baseUrl, trimmedUrl.c_str(), sizeof(s_cloudConfig.baseUrl));
        strlcpy(s_cloudConfig.deviceKey, trimmedKey.c_str(), sizeof(s_cloudConfig.deviceKey));
        s_cloudConfig.configured = true;
        cloudMusicUnlock();
    }
    s_cloudServiceState = CloudServiceState::Unknown; // old Ready/Offline no longer describes this config
    cloudMusicHistoryUpsert(trimmedUrl.c_str(), trimmedKey.c_str()); // remember this gateway for the future
    return true;
}

uint8_t playerCoreCloudServiceState() { return static_cast<uint8_t>(s_cloudServiceState); }

// One health-check attempt: GET {baseUrl}/esp/v1/health with the device key
// header, 3s timeout (spec 8.2: "2~3秒超时"). Checks for a real
// {"ok":true,...} body, not just HTTP 200 -- a captive portal or a
// still-cold-starting Render placeholder response could return 200 with the
// wrong body. No JSON library in this project (see jsonNumber/
// jsonStringField's own comments) and "ok" is a bare JSON boolean, not a
// number or string, so this checks for the literal the gateway always emits
// rather than adding a one-off jsonBool() helper.
static bool cloudMusicHealthCheckOnce(const CloudMusicConfig& cfg) {
    if (!WiFi.isConnected() || !cfg.configured) return false;
    WiFiClientSecure client;
    client.setInsecure(); // matches this project's existing HTTPS convention (weather/radio-icon fetches)
    HTTPClient http;
    const String url = String(cfg.baseUrl) + "/esp/v1/health";
    if (!http.begin(client, url)) return false;
    http.setTimeout(10000); // Render cold start can take a while
    http.addHeader("X-Device-Key", cfg.deviceKey);
    const int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        const String body = http.getString();
        ok = body.indexOf("\"ok\":true") >= 0 || body.indexOf("\"ok\": true") >= 0;
    }
    http.end();
    return ok;
}

// Second wake stage: the gateway being up doesn't mean the upstream
// api-enhanced is -- Render's free tier sleeps BOTH services, and a browse
// against a still-cold upstream returns HTTP 502 in <1s (Render's
// cold-start proxy answer), so the device's retry window can't wait it out.
// The gateway's /esp/v1/wake blocks until the upstream genuinely answers
// (up to ~90s), so this call is what actually turns "已连接" into "browse
// will work". HTTP timeout must exceed the gateway's own wake timeout.
static bool cloudMusicWakeUpstreamOnce(const CloudMusicConfig& cfg) {
    if (!WiFi.isConnected() || !cfg.configured) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    const String url = String(cfg.baseUrl) + "/esp/v1/wake";
    if (!http.begin(client, url)) return false;
    http.setTimeout(95000); // longer than the gateway's 90s upstream-wake budget
    http.addHeader("X-Device-Key", cfg.deviceKey);
    const int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        const String body = http.getString();
        ok = body.indexOf("\"ok\":true") >= 0 || body.indexOf("\"ok\": true") >= 0;
    }
    http.end();
    return ok;
}

// Cold-start wake sequence (spec 8.2): stage 1 is the gateway's own health
// (retry every 7s, ~70s budget); stage 2 warms the cold-sleeping upstream
// api-enhanced via /esp/v1/wake (up to ~190s total) so the first browse
// doesn't 502. Runs on its own one-shot task (mirrors
// radioIconSyncTask/wifiScanTask's pattern) so the LVGL thread never blocks
// on network I/O -- the UI polls playerCoreCloudServiceState() the same way
// it already polls UsbStorageState for USB MSC mount progress.
static void cloudMusicWakeTask(void*) {
    const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
    if (!cfg.configured) {
        s_cloudServiceState = CloudServiceState::Offline;
        s_cloudWakeTaskRunning = false;
        vTaskDelete(nullptr);
        return;
    }
    s_cloudServiceState = CloudServiceState::Waking;
    // Two-stage wake: up to ~70s for the gateway's own health, then up to
    // ~180s total for the upstream api-enhanced to come back from Render's
    // cold sleep (the /esp/v1/wake call itself blocks on the gateway).
    const uint32_t deadlineMs = millis() + 190000;
    bool ready = false;
    bool upstreamReady = false;
    for (;;) {
        ready = cloudMusicHealthCheckOnce(cfg);
        printf("[CLOUDMUSIC] health check %s\n", ready ? "ok" : "failed");
        if (ready || millis() >= deadlineMs) break;
        vTaskDelay(pdMS_TO_TICKS(7000));
    }
    if (ready) {
        // Gateway is up -- now warm the sleeping upstream so the first
        // browse/resolve doesn't 502. Retry until it answers or the
        // deadline passes (the gateway itself retries upstream internally
        // for up to 90s per call, so a few attempts cover Render's worst
        // cold-start).
        while (!upstreamReady && millis() < deadlineMs) {
            upstreamReady = cloudMusicWakeUpstreamOnce(cfg);
            printf("[CLOUDMUSIC] upstream warm %s\n", upstreamReady ? "ok" : "failed");
            if (!upstreamReady) vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    s_cloudServiceState = (ready && upstreamReady) ? CloudServiceState::Ready : CloudServiceState::Offline;
    if (ready && upstreamReady) s_cloudLastReadyMs = millis();
    s_cloudWakeTaskRunning = false;
    vTaskDelete(nullptr);
}

// Entry point: called when the user opens 在线音乐 settings/home (spec 8.2:
// "进入'在线音乐' -> 请求 /health"). No-op while a wake is already in
// flight; safe to call repeatedly (e.g. re-entering the page).
//
// 如果服务最近已经确认过 Ready，这里也直接跳过：buildCloudMusicHome() 每次
// 进页面都会无条件调这个函数（不只是第一次进），没有这个判断的话，每次进
// 页面都会重新做一次 WiFiClientSecure/mbedTLS 的健康检查握手——这个任务没
// 绑核，会跟 LVGL 渲染任务抢 CPU，正好对应"进在线音乐时UI有点卡"这个真实
// 反馈。5 分钟这个窗口比 Render 免费版约 15 分钟的休眠阈值短很多，所以不会
// 重新引入两段式唤醒机制本来要解决的"双重冷启动 502"问题——真正冷启动的
//情况还是会走完整的唤醒流程。
static constexpr uint32_t kCloudReadySkipWindowMs = 5UL * 60UL * 1000UL;

void playerCoreCloudMusicWakeStart() {
    if (s_cloudWakeTaskRunning) return;
    if (s_cloudServiceState == CloudServiceState::Ready && (millis() - s_cloudLastReadyMs) < kCloudReadySkipWindowMs) return;
    const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
    if (!cfg.configured) {
        s_cloudServiceState = CloudServiceState::Offline;
        return;
    }
    s_cloudWakeTaskRunning = true;
    // 8K was the value that worked before (health checks succeeded), and
    // every kilobyte of task stack is internal heap that mbedTLS needs for
    // its handshake buffers -- the "唤醒不了" bug was heap exhaustion, so
    // stack stays at the proven size rather than being bumped speculatively.
    if (xTaskCreatePinnedToCore(cloudMusicWakeTask, "cloudWake", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        s_cloudWakeTaskRunning = false;
        s_cloudServiceState = CloudServiceState::Offline;
        MWR_LOG_ERROR("Failed to create cloud music wake task");
    }
}

// Periodic keep-alive: Render's free tier spins BOTH services (the gateway
// and the upstream api-enhanced) down after ~15 min of inactivity, which is
// what made browsing 502 after idle. Every 9 min the board health-checks
// the gateway AND warms the upstream via /esp/v1/wake, so both stay awake
// while the device is powered on -- the first 在线音乐 visit after a long
// idle no longer waits through a double cold start. The wake call is fast
// (<1s) when services are already warm; the retries here only bite during
// a genuine cold start.
static void cloudKeepaliveTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(9 * 60 * 1000));
        const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
        if (!cfg.configured || !WiFi.isConnected()) continue;
        for (uint8_t attempt = 0; attempt < 3; ++attempt) {
            if (!cloudMusicHealthCheckOnce(cfg)) {
                vTaskDelay(pdMS_TO_TICKS(20000));
                continue;
            }
            if (cloudMusicWakeUpstreamOnce(cfg)) break;
            vTaskDelay(pdMS_TO_TICKS(20000));
        }
    }
}

// ---- Cloud Music phase 3: search / hot playlists / playlist detail ------
// Browse-only, per the project's phased NCM spec -- no playback wiring
// (that's phase 4). One background controller task processes one command
// at a time (mirrors lyricsFetchTask's queue+task shape); each *Start()
// bumps s_cloudRequestGeneration and stamps its command with the new value,
// and a finishing command only writes its result into the shared arrays if
// that generation is still current -- otherwise a newer request has since
// superseded it and the stale result is silently dropped (same idea as
// lyricsFetchTask checking s_lyricTrackIndex before applying a result, one
// generation counter shared across all three lookup kinds since only one of
// them is ever visible on screen at a time in this UI).

static constexpr uint8_t kCloudSearchMaxResults = 10;
static constexpr uint8_t kCloudHotPlaylistMax = 8;
static constexpr uint8_t kCloudPlaylistTrackMax = 20;
// Sized to fit the 320x170 screen's scrollable list comfortably while
// keeping the static arrays' internal-RAM footprint as small as possible --
// TLS handshakes need ~40KB of contiguous heap, and every kilobyte of
// static array counts on this device.
static constexpr uint8_t kCloudRankingMax = 8;
static constexpr uint8_t kCloudNewSongMax = 12;

enum class CloudCommandType : uint8_t { Search, LoadHotPlaylists, LoadPlaylist, LoadRankings, LoadNewSongs, ResolveAndPlay };
struct CloudCommand {
    CloudCommandType type;
    uint32_t generation;
    char param[128]; // search query, or playlist id
};

static QueueHandle_t s_cloudCommandQueue = nullptr;
static SemaphoreHandle_t s_cloudResultMutex = nullptr;
static volatile uint32_t s_cloudRequestGeneration = 0;
static char s_cloudLastError[96] = "";

static CloudMusicRequestState s_cloudSearchState = CloudMusicRequestState::Idle;
// PSRAM-backed result arrays (allocated in setup()): internal RAM is the
// constraint for TLS handshakes (~40KB contiguous heap), so the bulk
// cloud-music lists live in PSRAM -- same reasoning as the audio library's
// big input buffer. All access goes through the result mutex, unchanged.
static CloudTrackItem* s_cloudSearchResults = nullptr;
static uint8_t s_cloudSearchResultCount = 0;
static bool s_cloudSearchHasMore = false;

static CloudMusicRequestState s_cloudHotState = CloudMusicRequestState::Idle;
static CloudPlaylistItem* s_cloudHotPlaylists = nullptr;
static uint8_t s_cloudHotPlaylistCount = 0;

static CloudMusicRequestState s_cloudPlaylistState = CloudMusicRequestState::Idle;
static CloudPlaylistItem s_cloudPlaylistInfo;
static CloudTrackItem* s_cloudPlaylistTracks = nullptr;
static uint8_t s_cloudPlaylistTrackCount = 0;

// Phase 5: ranking charts + new-song arrivals (see buildCloudRankings()/
// buildCloudNewSongs() in the UI). Separate arrays/states, same
// single-request-at-a-time discipline as the phase-3 lookups.
static CloudMusicRequestState s_cloudRankingState = CloudMusicRequestState::Idle;
static CloudRankingItem* s_cloudRankings = nullptr;
static uint8_t s_cloudRankingCount = 0;

static CloudMusicRequestState s_cloudNewSongState = CloudMusicRequestState::Idle;
static CloudTrackItem* s_cloudNewSongs = nullptr;
static uint8_t s_cloudNewSongCount = 0;

// Phase 4: resolve + play. s_cloudResolveState is polled by the UI (a
// track row's tap feedback); the actual connecttohost() happens on
// cloudMusicControllerTask itself once resolve succeeds (background tasks
// already do this elsewhere in this codebase -- e.g. usbStorageMountTask
// calls stopSong() -- so this isn't a new pattern), not routed back through
// the LVGL thread first.
static CloudMusicRequestState s_cloudResolveState = CloudMusicRequestState::Idle;
// Title/artist/album/cover for whatever cloud track is currently playing --
// set once resolve succeeds, read by PlayerService::tick() to fill
// PlayerSnapshot's title/detail/durationSeconds fields for CloudMusic
// (there's no ICY metadata on a plain CDN file URL to wait for, unlike
// internet radio).
static CloudTrackItem s_cloudNowPlaying;
// One-shot "playback just started" latch consumed by the LVGL refresh loop:
// lets the UI jump to the Now Playing page the moment a resolved track
// actually starts, instead of leaving the user staring at the list with no
// visible feedback (which reads as "播放不了" even when audio is playing).
static volatile bool s_cloudPlaybackJustStarted = false;

static bool cloudResultLock(TickType_t timeout = pdMS_TO_TICKS(250)) {
    return !s_cloudResultMutex || xSemaphoreTake(s_cloudResultMutex, timeout) == pdTRUE;
}
static void cloudResultUnlock() {
    if (s_cloudResultMutex) xSemaphoreGive(s_cloudResultMutex);
}

// Finds "key":[ ... ] in body and returns up to maxItems top-level {...}
// object slices from inside it (raw JSON text per item, for
// jsonStringField()/jsonNumber() to run on directly). Same hand-scan
// approach as jsonStringField (no JSON library in this project), extended
// to walk balanced brace depth -- respecting quoted strings and \" escapes
// so a brace or comma inside a title/name string doesn't desync it.
static uint8_t jsonArrayItems(const String& body, const char* key, String* outItems, uint8_t maxItems) {
    const String needle = String("\"") + key + "\":[";
    const int start = body.indexOf(needle);
    if (start < 0) return 0;
    int pos = start + static_cast<int>(needle.length());
    const int len = static_cast<int>(body.length());
    uint8_t count = 0;
    while (count < maxItems && pos < len) {
        while (pos < len && (isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ',')) ++pos;
        if (pos >= len || body[pos] == ']') break;
        if (body[pos] != '{') break; // malformed / not an object array -- stop rather than misparse
        const int objStart = pos;
        int depth = 0;
        bool inString = false;
        for (; pos < len; ++pos) {
            const char c = body[pos];
            if (inString) {
                if (c == '\\') { ++pos; continue; } // loop's own ++pos advances past the escaped char too
                if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) { ++pos; break; }
            }
        }
        outItems[count++] = body.substring(objStart, pos);
    }
    return count;
}

// Strips characters the embedded CJK subset font (lv_font_cjk_13, ASCII +
// ~19k CJK/kana/symbols) cannot render -- most importantly non-BMP emoji
// (4-byte UTF-8) which would otherwise show as tofu boxes mixed into
// otherwise-fine Chinese text ("文字乱码"), plus stray control chars.
// Operates in place on a NUL-terminated UTF-8 buffer.
static void cloudTextSanitize(char* s) {
    char* w = s;
    const char* r = s;
    while (*r) {
        const uint8_t c = static_cast<uint8_t>(*r);
        if (c < 0x80) {
            if (c >= 0x20 && c != 0x7F) *w++ = *r;
            ++r;
        } else if (c < 0xE0) {
            *w++ = *r++;
        } else if (c < 0xF0) {
            *w++ = *r++;
            if (*r) *w++ = *r++;
            if (*r) *w++ = *r++;
        } else {
            r += 4; // non-BMP (emoji / rare chars) -- drop the whole sequence
        }
    }
    *w = '\0';
}

static void parseCloudTrackItem(const String& itemJson, CloudTrackItem* out) {
    *out = CloudTrackItem{};
    String s;
    if (jsonStringField(itemJson, "id", s)) strlcpy(out->id, s.c_str(), sizeof(out->id));
    if (jsonStringField(itemJson, "title", s)) strlcpy(out->title, s.c_str(), sizeof(out->title));
    if (jsonStringField(itemJson, "artist", s)) strlcpy(out->artist, s.c_str(), sizeof(out->artist));
    if (jsonStringField(itemJson, "album", s)) strlcpy(out->album, s.c_str(), sizeof(out->album));
    if (jsonStringField(itemJson, "cover_url", s)) strlcpy(out->coverUrl, s.c_str(), sizeof(out->coverUrl));
    double num = 0;
    if (jsonNumber(itemJson, "duration_ms", &num)) out->durationMs = static_cast<uint32_t>(num);
    out->playableHint = itemJson.indexOf("\"playable_hint\":false") < 0;
    out->vip = itemJson.indexOf("\"vip\":true") >= 0;
    out->paid = itemJson.indexOf("\"paid\":true") >= 0;
    cloudTextSanitize(out->title);
    cloudTextSanitize(out->artist);
    cloudTextSanitize(out->album);
}

static void parseCloudPlaylistItem(const String& itemJson, CloudPlaylistItem* out) {
    *out = CloudPlaylistItem{};
    String s;
    if (jsonStringField(itemJson, "id", s)) strlcpy(out->id, s.c_str(), sizeof(out->id));
    if (jsonStringField(itemJson, "name", s)) strlcpy(out->name, s.c_str(), sizeof(out->name));
    if (jsonStringField(itemJson, "creator", s)) strlcpy(out->creator, s.c_str(), sizeof(out->creator));
    if (jsonStringField(itemJson, "cover_url", s)) strlcpy(out->coverUrl, s.c_str(), sizeof(out->coverUrl));
    double num = 0;
    if (jsonNumber(itemJson, "track_count", &num)) out->trackCount = static_cast<uint16_t>(num);
    cloudTextSanitize(out->name);
    cloudTextSanitize(out->creator);
}

static void parseCloudRankingItem(const String& itemJson, CloudRankingItem* out) {
    *out = CloudRankingItem{};
    String s;
    if (jsonStringField(itemJson, "id", s)) strlcpy(out->id, s.c_str(), sizeof(out->id));
    if (jsonStringField(itemJson, "name", s)) strlcpy(out->name, s.c_str(), sizeof(out->name));
    if (jsonStringField(itemJson, "cover_url", s)) strlcpy(out->coverUrl, s.c_str(), sizeof(out->coverUrl));
    if (jsonStringField(itemJson, "update_freq", s)) strlcpy(out->updateFreq, s.c_str(), sizeof(out->updateFreq));
    cloudTextSanitize(out->name);
}

// One GET against the configured gateway, with the device key header and
// this project's existing HTTPS posture (setInsecure(), see
// cloudMusicHealthCheckOnce's comment). On a gateway error response
// ({"ok":false,"error":{...}}), fills s_cloudLastError from the "message"
// field so callers don't need to re-parse the error envelope themselves.
static bool cloudMusicHttpGet(const String& path, String& outBody) {
    const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
    if (!WiFi.isConnected()) {
        strlcpy(s_cloudLastError, "WiFi 未连接", sizeof(s_cloudLastError));
        return false;
    }
    if (!cfg.configured) {
        strlcpy(s_cloudLastError, "未配置在线音乐网关", sizeof(s_cloudLastError));
        return false;
    }
    WiFiClientSecure client;
    client.setInsecure(); // matches this project's HTTPS convention (weather/health)
    HTTPClient http;
    const String url = String(cfg.baseUrl) + path;
    // Explicit TLS client: configs are normalized to https:// (see
    // playerCoreCloudMusicConfig), so a secure client is always correct.
    // setFollowRedirects stays as harmless safety for https->https
    // redirects.
    if (!http.begin(client, url)) {
        strlcpy(s_cloudLastError, "无法连接网关", sizeof(s_cloudLastError));
        return false;
    }
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.addHeader("X-Device-Key", cfg.deviceKey);
    const int code = http.GET();
    if (code <= 0) {
        http.end();
        strlcpy(s_cloudLastError, "网络请求失败", sizeof(s_cloudLastError));
        return false;
    }
    outBody = http.getString();
    http.end();
    const bool ok = outBody.indexOf("\"ok\":true") >= 0 || outBody.indexOf("\"ok\": true") >= 0;
    if (!ok) {
        String message;
        if (!jsonStringField(outBody, "message", message) || message.isEmpty()) message = "请求失败";
        strlcpy(s_cloudLastError, message.c_str(), sizeof(s_cloudLastError));
        return false;
    }
    return true;
}

// Render's free tier sleeps the upstream api-enhanced service after idle
// and takes up to ~50s to wake it; during that window Render answers with
// HTTP 502. Retry transient failures with a 7s pause (same spirit as
// cloudMusicWakeTask) so the first browse/resolve request after a sleep
// succeeds instead of erroring out instantly. Stops early if a newer
// command superseded us while we were waiting.
static constexpr uint8_t kCloudBrowseRetryMax = 8;
static constexpr uint32_t kCloudBrowseRetryDelayMs = 7000;

static bool cloudMusicHttpGetWithRetry(const String& path, String& outBody, const uint32_t& generation) {
    for (uint8_t attempt = 0; attempt < kCloudBrowseRetryMax; ++attempt) {
        if (generation != s_cloudRequestGeneration) return false; // user moved on
        if (cloudMusicHttpGet(path, outBody)) return true;
        if (attempt + 1 < kCloudBrowseRetryMax) vTaskDelay(pdMS_TO_TICKS(kCloudBrowseRetryDelayMs));
    }
    return false;
}

// ---- Cloud music connectivity diagnostic ---------------------------------
// Served as /cloud_diag_start + /cloud_diag_json on the device web server:
// replicates the exact wake/browse failure path (DNS resolve -> plain TCP
// to :443 -> full TLS health check) on a generously-stacked background
// task and reports which stage fails, so a "唤醒不了/连接不上" report can be
// attributed to DNS vs firewall vs TLS/heap instead of guessing. The
// result is stored in a static buffer that /cloud_diag_json returns once
// s_cloudDiagReady flips.
static char s_cloudDiagResult[512] = "";
static volatile bool s_cloudDiagReady = false;
static volatile bool s_cloudDiagRunning = false;

// Writes a partial result immediately so /cloud_diag_json shows progress
// even if a stage hangs. Heap is reported first -- TLS memory pressure was
// the actual root cause of "唤醒不了" (free internal heap had dropped to
// ~13KB; mbedTLS needs ~40KB on top of the task stack).
static void cloudDiagReport(const char* stage, const char* extraJson) {
    String out = "{\"ready\":false,\"stage\":\"";
    out += stage;
    out += "\",\"heap\":";
    out += String(ESP.getFreeHeap());
    if (extraJson && extraJson[0]) out += extraJson;
    out += "}";
    strlcpy(s_cloudDiagResult, out.c_str(), sizeof(s_cloudDiagResult));
}

static void cloudDiagTask(void*) {
    String out = "{";
    const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
    const bool wifiOk = WiFi.status() == WL_CONNECTED;
    cloudDiagReport("start", wifiOk ? ",\"wifi\":true" : ",\"wifi\":false");
    out += "\"wifi\":";
    out += wifiOk ? "true" : "false";
    out += ",\"ssid\":\"";
    out += wifiOk ? WiFi.SSID().c_str() : "";
    out += "\",\"rssi\":";
    out += String(wifiOk ? static_cast<int>(WiFi.RSSI()) : 0);
    out += ",\"heap\":";
    out += String(ESP.getFreeHeap());
    out += ",\"psram\":";
    out += String(ESP.getFreePsram());

    String host = cfg.baseUrl;
    if (host.startsWith("https://")) host = host.substring(8);
    else if (host.startsWith("http://")) host = host.substring(7);
    const int slash = host.indexOf('/');
    if (slash >= 0) host = host.substring(0, slash);
    cloudDiagReport("dns", "");

    IPAddress resolved;
    const bool dnsOk = WiFi.hostByName(host.c_str(), resolved);
    out += ",\"dns_ok\":";
    out += dnsOk ? "true" : "false";
    if (dnsOk) {
        out += ",\"resolved\":\"";
        out += resolved.toString();
        out += "\"";
    }
    cloudDiagReport("tcp", "");

    WiFiClient tcp;
    const bool tcpOk = dnsOk && tcp.connect(resolved, 443);
    out += ",\"tcp443\":";
    out += tcpOk ? "true" : "false";
    if (tcp) tcp.stop();
    cloudDiagReport("health", "");

    String body;
    const bool healthOk = cloudMusicHttpGet("/esp/v1/health", body);
    out += ",\"health_ok\":";
    out += healthOk ? "true" : "false";
    out += ",\"error\":\"";
    // Gateway error messages are plain Chinese/ASCII text -- no quotes or
    // backslashes in practice; escape defensively anyway.
    const char* err = s_cloudLastError;
    for (const char* p = err; *p; ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        out += *p;
    }
    out += "\"}";

    strlcpy(s_cloudDiagResult, out.c_str(), sizeof(s_cloudDiagResult));
    s_cloudDiagReady = true;
    s_cloudDiagRunning = false;
    vTaskDelete(nullptr);
}

void playerCoreCloudDiagStart() {
    if (s_cloudDiagRunning) return;
    s_cloudDiagRunning = true;
    s_cloudDiagReady = false;
    s_cloudDiagResult[0] = '\0';
    // 16K covers DNS+TCP+TLS here (TLS heap buffers are separate from this
    // task's stack); the earlier 24K version was one more big heap consumer
    // on an already tight device.
    if (xTaskCreatePinnedToCore(cloudDiagTask, "cloudDiag", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
        s_cloudDiagRunning = false;
        MWR_LOG_ERROR("Failed to create cloud diag task");
    }
}

bool playerCoreCloudDiagReady() { return s_cloudDiagReady; }

const char* playerCoreCloudDiagResult() { return s_cloudDiagResult; }

static void cloudMusicControllerTask(void*) {
    CloudCommand cmd;
    for (;;) {
        if (xQueueReceive(s_cloudCommandQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd.type == CloudCommandType::Search) {
            if (cloudResultLock()) {
                s_cloudSearchState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            if (!s_cloudSearchResults) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudSearchState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            String query;
            urlEncodeAppend(query, cmd.param);
            char path[192];
            snprintf(path, sizeof(path), "/esp/v1/search?q=%s&limit=%u", query.c_str(), kCloudSearchMaxResults);
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue; // superseded while the request was in flight
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (ok) {
                    String items[kCloudSearchMaxResults];
                    const uint8_t n = jsonArrayItems(body, "items", items, kCloudSearchMaxResults);
                    for (uint8_t i = 0; i < n; ++i) parseCloudTrackItem(items[i], &s_cloudSearchResults[i]);
                    s_cloudSearchResultCount = n;
                    s_cloudSearchHasMore = body.indexOf("\"has_more\":true") >= 0;
                    s_cloudSearchState = CloudMusicRequestState::Loaded;
                } else {
                    s_cloudSearchResultCount = 0;
                    s_cloudSearchState = CloudMusicRequestState::Error;
                }
                cloudResultUnlock();
            }
        } else if (cmd.type == CloudCommandType::LoadHotPlaylists) {
            if (cloudResultLock()) {
                s_cloudHotState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            if (!s_cloudHotPlaylists) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudHotState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            // Optional language-category tag ("华语"/"欧美"/"日语"...) --
            // passed through the command param (see the 语言分类 feature),
            // URL-encoded since the tags are non-ASCII.
            char path[192];
            if (cmd.param[0]) {
                String catEnc;
                urlEncodeAppend(catEnc, cmd.param);
                snprintf(path, sizeof(path), "/esp/v1/playlists/hot?limit=%u&cat=%s", kCloudHotPlaylistMax, catEnc.c_str());
            } else {
                snprintf(path, sizeof(path), "/esp/v1/playlists/hot?limit=%u", kCloudHotPlaylistMax);
            }
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue;
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (ok) {
                    String items[kCloudHotPlaylistMax];
                    const uint8_t n = jsonArrayItems(body, "items", items, kCloudHotPlaylistMax);
                    for (uint8_t i = 0; i < n; ++i) parseCloudPlaylistItem(items[i], &s_cloudHotPlaylists[i]);
                    s_cloudHotPlaylistCount = n;
                    s_cloudHotState = CloudMusicRequestState::Loaded;
                } else {
                    s_cloudHotPlaylistCount = 0;
                    s_cloudHotState = CloudMusicRequestState::Error;
                }
                cloudResultUnlock();
            }
        } else if (cmd.type == CloudCommandType::LoadPlaylist) {
            if (cloudResultLock()) {
                s_cloudPlaylistState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            if (!s_cloudPlaylistTracks) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudPlaylistState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            char path[160];
            snprintf(path, sizeof(path), "/esp/v1/playlists/%s?limit=%u", cmd.param, kCloudPlaylistTrackMax);
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue;
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (ok) {
                    // "playlist" is a single object, not an array -- jsonStringField/
                    // jsonNumber already find the first (only) match directly.
                    parseCloudPlaylistItem(body, &s_cloudPlaylistInfo);
                    String items[kCloudPlaylistTrackMax];
                    const uint8_t n = jsonArrayItems(body, "tracks", items, kCloudPlaylistTrackMax);
                    for (uint8_t i = 0; i < n; ++i) parseCloudTrackItem(items[i], &s_cloudPlaylistTracks[i]);
                    s_cloudPlaylistTrackCount = n;
                    s_cloudPlaylistState = CloudMusicRequestState::Loaded;
                } else {
                    s_cloudPlaylistTrackCount = 0;
                    s_cloudPlaylistState = CloudMusicRequestState::Error;
                }
                cloudResultUnlock();
            }
        } else if (cmd.type == CloudCommandType::LoadRankings) {
            if (cloudResultLock()) {
                s_cloudRankingState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            if (!s_cloudRankings) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudRankingState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            char path[64];
            snprintf(path, sizeof(path), "/esp/v1/rankings?limit=%u", kCloudRankingMax);
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue;
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (ok) {
                    String items[kCloudRankingMax];
                    const uint8_t n = jsonArrayItems(body, "items", items, kCloudRankingMax);
                    for (uint8_t i = 0; i < n; ++i) parseCloudRankingItem(items[i], &s_cloudRankings[i]);
                    s_cloudRankingCount = n;
                    s_cloudRankingState = CloudMusicRequestState::Loaded;
                } else {
                    s_cloudRankingCount = 0;
                    s_cloudRankingState = CloudMusicRequestState::Error;
                }
                cloudResultUnlock();
            }
        } else if (cmd.type == CloudCommandType::LoadNewSongs) {
            if (cloudResultLock()) {
                s_cloudNewSongState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            if (!s_cloudNewSongs) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudNewSongState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            char path[64];
            snprintf(path, sizeof(path), "/esp/v1/new-songs?limit=%u", kCloudNewSongMax);
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue;
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (ok) {
                    String items[kCloudNewSongMax];
                    const uint8_t n = jsonArrayItems(body, "items", items, kCloudNewSongMax);
                    for (uint8_t i = 0; i < n; ++i) parseCloudTrackItem(items[i], &s_cloudNewSongs[i]);
                    s_cloudNewSongCount = n;
                    s_cloudNewSongState = CloudMusicRequestState::Loaded;
                } else {
                    s_cloudNewSongCount = 0;
                    s_cloudNewSongState = CloudMusicRequestState::Error;
                }
                cloudResultUnlock();
            }
        } else { // ResolveAndPlay
            if (cloudResultLock()) {
                s_cloudResolveState = CloudMusicRequestState::Loading;
                cloudResultUnlock();
            }
            char path[64];
            snprintf(path, sizeof(path), "/esp/v1/tracks/%s/resolve?bitrate=128", cmd.param);
            String body;
            const bool ok = cloudMusicHttpGetWithRetry(path, body, cmd.generation);
            if (cmd.generation != s_cloudRequestGeneration) continue; // user moved on to a different track before this resolved
            if (!ok) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudResolveState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            // "track" and "stream" objects have no overlapping key names
            // (id/title/artist/album/duration_ms/cover_url vs
            // url/codec/bitrate_kbps/expires_at/seekable_hint), so these can
            // read straight off the whole response body without slicing out
            // either sub-object first.
            CloudTrackItem track{};
            parseCloudTrackItem(body, &track);
            String streamUrl;
            const bool hasUrl = jsonStringField(body, "url", streamUrl) && streamUrl.length() > 0;
            if (!hasUrl) {
                if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                    s_cloudResolveState = CloudMusicRequestState::Error;
                    cloudResultUnlock();
                }
                continue;
            }
            // Actually start playback right here on this background task --
            // connecttohost() is already called from non-LVGL tasks
            // elsewhere in this codebase (e.g. usbStorageMountTask's
            // stopSong()), so this isn't a new concurrency pattern. Re-check
            // the generation once more right before committing to the
            // switch, in case the user tapped a different track during the
            // parsing above (unlikely given how fast it is, but cheap to
            // guard).
            if (cmd.generation != s_cloudRequestGeneration) continue;
            const bool started = playerCorePlayCloudUrl(streamUrl.c_str());
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                if (started) {
                    s_cloudNowPlaying = track;
                    s_cloudResolveState = CloudMusicRequestState::Loaded;
                    s_cloudPlaybackJustStarted = true;
                } else {
                    s_cloudResolveState = CloudMusicRequestState::Error;
                    strlcpy(s_cloudLastError, "无法连接音乐 CDN", sizeof(s_cloudLastError));
                }
                cloudResultUnlock();
            }
        }
    }
}

static bool cloudMusicEnqueue(CloudCommandType type, const char* param) {
    if (!s_cloudCommandQueue) return false;
    CloudCommand cmd{};
    cmd.type = type;
    cmd.generation = ++s_cloudRequestGeneration;
    if (param) strlcpy(cmd.param, param, sizeof(cmd.param));
    // Queue depth 1 (see its xQueueCreate call in setup()): a command still
    // waiting to be picked up gets overwritten by a newer one of any kind,
    // same as lyricsFetchTask's single-slot queue. A command already being
    // processed can't be aborted mid-HTTP-request, but its result is
    // discarded on completion once cmd.generation no longer matches (see
    // cloudMusicControllerTask).
    return xQueueOverwrite(s_cloudCommandQueue, &cmd) == pdTRUE;
}

bool playerCoreCloudMusicSearchStart(const char* query) {
    if (!query || !query[0]) return false;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudSearchResultCount = 0;
        s_cloudSearchState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    return cloudMusicEnqueue(CloudCommandType::Search, query);
}

uint8_t playerCoreCloudMusicSearchState() { return static_cast<uint8_t>(s_cloudSearchState); }

uint8_t playerCoreCloudMusicSearchResultCount() { return s_cloudSearchResultCount; }

bool playerCoreCloudMusicSearchResult(uint8_t index, CloudTrackItem* item) {
    if (!item || !s_cloudSearchResults || index >= s_cloudSearchResultCount) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *item = s_cloudSearchResults[index];
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

bool playerCoreCloudMusicSearchHasMore() { return s_cloudSearchHasMore; }

const char* playerCoreCloudMusicLastError() { return s_cloudLastError; }

void playerCoreCloudMusicHotPlaylistsStart(const char* cat) {
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudHotState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    cloudMusicEnqueue(CloudCommandType::LoadHotPlaylists, (cat && cat[0]) ? cat : nullptr);
}

uint8_t playerCoreCloudMusicHotPlaylistsState() { return static_cast<uint8_t>(s_cloudHotState); }

uint8_t playerCoreCloudMusicHotPlaylistCount() { return s_cloudHotPlaylistCount; }

bool playerCoreCloudMusicHotPlaylist(uint8_t index, CloudPlaylistItem* item) {
    if (!item || !s_cloudHotPlaylists || index >= s_cloudHotPlaylistCount) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *item = s_cloudHotPlaylists[index];
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

bool playerCoreCloudMusicPlaylistDetailStart(const char* playlistId) {
    if (!playlistId || !playlistId[0]) return false;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudPlaylistTrackCount = 0;
        s_cloudPlaylistState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    return cloudMusicEnqueue(CloudCommandType::LoadPlaylist, playlistId);
}

uint8_t playerCoreCloudMusicPlaylistDetailState() { return static_cast<uint8_t>(s_cloudPlaylistState); }

CloudPlaylistItem playerCoreCloudMusicPlaylistDetailInfo() {
    CloudPlaylistItem out;
    if (cloudResultLock()) {
        out = s_cloudPlaylistInfo;
        cloudResultUnlock();
    }
    return out;
}

uint8_t playerCoreCloudMusicPlaylistTrackCount() { return s_cloudPlaylistTrackCount; }

bool playerCoreCloudMusicPlaylistTrack(uint8_t index, CloudTrackItem* item) {
    if (!item || !s_cloudPlaylistTracks || index >= s_cloudPlaylistTrackCount) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *item = s_cloudPlaylistTracks[index];
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

void playerCoreCloudMusicRankingsStart() {
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudRankingState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    cloudMusicEnqueue(CloudCommandType::LoadRankings, nullptr);
}

uint8_t playerCoreCloudMusicRankingsState() { return static_cast<uint8_t>(s_cloudRankingState); }

uint8_t playerCoreCloudMusicRankingCount() { return s_cloudRankingCount; }

bool playerCoreCloudMusicRanking(uint8_t index, CloudRankingItem* item) {
    if (!item || !s_cloudRankings || index >= s_cloudRankingCount) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *item = s_cloudRankings[index];
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

void playerCoreCloudMusicNewSongsStart() {
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudNewSongState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    cloudMusicEnqueue(CloudCommandType::LoadNewSongs, nullptr);
}

uint8_t playerCoreCloudMusicNewSongsState() { return static_cast<uint8_t>(s_cloudNewSongState); }

uint8_t playerCoreCloudMusicNewSongCount() { return s_cloudNewSongCount; }

bool playerCoreCloudMusicNewSong(uint8_t index, CloudTrackItem* item) {
    if (!item || !s_cloudNewSongs || index >= s_cloudNewSongCount) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *item = s_cloudNewSongs[index];
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

// Phase 4: resolve + play a track by id (from a search result or playlist
// row). Enqueues CloudCommandType::ResolveAndPlay -- see
// cloudMusicControllerTask's own comment for why actual playback is kicked
// off from that background task rather than routed back through the LVGL
// thread.
bool playerCoreCloudMusicPlayTrackStart(const char* trackId) {
    if (!trackId || !trackId[0]) return false;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        s_cloudResolveState = CloudMusicRequestState::Loading;
        cloudResultUnlock();
    }
    return cloudMusicEnqueue(CloudCommandType::ResolveAndPlay, trackId);
}

uint8_t playerCoreCloudMusicResolveState() { return static_cast<uint8_t>(s_cloudResolveState); }

// Non-consuming copy of the last resolved cloud track (s_cloudNowPlaying
// persists after playerCoreCloudMusicConsumeNowPlaying() has reset the
// resolve state to Idle -- the title/artist/cover stay valid while the
// track keeps playing).
bool playerCoreCloudMusicNowPlayingTrack(CloudTrackItem* outTrack) {
    if (!outTrack) return false;
    bool ok = false;
    if (cloudResultLock()) {
        *outTrack = s_cloudNowPlaying;
        ok = true;
        cloudResultUnlock();
    }
    return ok;
}

// ---- Cloud cover thumbnails ----------------------------------------------
// Same SD-cache pattern as radio station logos: a background task downloads
// each cover to /cloudimg/ once; the UI decodes on demand. A zero-byte
// marker means "no cover available" (or the fetch failed) so a dead URL
// isn't retried on every page open -- same graceful-degradation rule as
// the radio icon marker.

static bool s_cloudThumbSyncInProgress = false;

static void cloudThumbPath(uint8_t kind, uint8_t index, char* out, size_t outSize) {
    snprintf(out, outSize, "/cloudimg/%c_%u.jpg", kind == 0 ? 'p' : 'r', index);
}

static void cloudThumbDownloadOne(const String& url, const char* sdPath) {
    if (url.length() == 0 || httpDownloadToFileRetrying(url, sdPath, 128 * 1024)) {
        if (url.length() == 0) {
            // No cover at all -- leave a marker so we never try again.
            File marker = SD_MMC.open(sdPath, "w", true);
            if (marker) marker.close();
        }
        return;
    }
    File marker = SD_MMC.open(sdPath, "w", true);
    if (marker) marker.close();
}

struct CloudThumbJob {
    char url[200];
    uint8_t kind;
    uint8_t index;
};

static void cloudThumbSyncTask(void*) {
    if (!SD_MMC.exists("/cloudimg")) SD_MMC.mkdir("/cloudimg");
    // Snapshot the URLs to fetch under the mutex, then download WITHOUT
    // holding it -- holding it for the whole loop would stall the UI's
    // state polls for seconds at a time.
    CloudThumbJob jobs[kCloudHotPlaylistMax + kCloudRankingMax]{};
    uint8_t jobCount = 0;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        for (uint8_t i = 0; i < s_cloudHotPlaylistCount && jobCount < kCloudHotPlaylistMax; ++i) {
            strlcpy(jobs[jobCount].url, s_cloudHotPlaylists[i].coverUrl, sizeof(jobs[jobCount].url));
            jobs[jobCount].kind = 0;
            jobs[jobCount].index = i;
            ++jobCount;
        }
        for (uint8_t i = 0; i < s_cloudRankingCount && jobCount < kCloudHotPlaylistMax + kCloudRankingMax; ++i) {
            strlcpy(jobs[jobCount].url, s_cloudRankings[i].coverUrl, sizeof(jobs[jobCount].url));
            jobs[jobCount].kind = 1;
            jobs[jobCount].index = i;
            ++jobCount;
        }
        cloudResultUnlock();
    }
    for (uint8_t j = 0; j < jobCount; ++j) {
        char sdPath[48];
        cloudThumbPath(jobs[j].kind, jobs[j].index, sdPath, sizeof(sdPath));
        if (SD_MMC.exists(sdPath)) continue;
        if (!WiFi.isConnected()) break; // no point continuing a sync that can't reach the network
        cloudThumbDownloadOne(jobs[j].url, sdPath);
        vTaskDelay(pdMS_TO_TICKS(50)); // don't hammer the CDN back to back
    }
    s_cloudThumbSyncInProgress = false;
    vTaskDelete(nullptr);
}

void playerCoreCloudThumbSyncStart() {
    if (s_cloudThumbSyncInProgress) return;
    s_cloudThumbSyncInProgress = true;
    if (xTaskCreatePinnedToCore(cloudThumbSyncTask, "cloudThumb", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        s_cloudThumbSyncInProgress = false;
        MWR_LOG_ERROR("Failed to create cloud thumb sync task");
    }
}

bool playerCoreCloudThumbSyncInProgress() { return s_cloudThumbSyncInProgress; }

bool playerCoreCloudThumbDecode(uint8_t kind, uint8_t index, uint8_t scaleFactor, uint16_t** outPixels,
                                uint16_t* outWidth, uint16_t* outHeight) {
    *outPixels = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    char sdPath[48];
    cloudThumbPath(kind, index, sdPath, sizeof(sdPath));
    File in = SD_MMC.open(sdPath, "r");
    if (!in) return false;
    const size_t len = in.size();
    if (len == 0) { // zero-byte marker -- "no cover", not an error
        in.close();
        return false;
    }
    uint8_t* data = static_cast<uint8_t*>(ps_malloc(len));
    if (!data) {
        in.close();
        return false;
    }
    const size_t readLen = in.read(data, len);
    in.close();
    if (readLen != len) {
        free(data);
        return false;
    }
    const bool ok = getTFT().decodeJpgFromMemory(data, len, scaleFactor, outPixels, outWidth, outHeight);
    free(data);
    return ok;
}

// Now-playing cover: one-shot fetch of the current cloud track's cover,
// cached as /cloudimg/np_<trackId>.jpg (keyed by track id so switching
// tracks never shows a stale cached cover). Missing/dead covers fall back
// to the UI's music-note tile.
static bool s_cloudNowPlayingCoverRequested = false;
// Fallback URL copied at start() time -- used when the track itself has no
// album art (the UI passes the cover of the playlist/ranking the track
// came from).
static char s_cloudNowPlayingCoverFallback[200] = "";

static void cloudNowPlayingCoverTask(void*) {
    if (!SD_MMC.exists("/cloudimg")) SD_MMC.mkdir("/cloudimg");
    CloudTrackItem track{};
    playerCoreCloudMusicNowPlayingTrack(&track);
    if (track.id[0]) {
        char sdPath[48];
        snprintf(sdPath, sizeof(sdPath), "/cloudimg/np_%s.jpg", track.id);
        const char* url = track.coverUrl[0] ? track.coverUrl : s_cloudNowPlayingCoverFallback;
        cloudThumbDownloadOne(url, sdPath);
    }
    s_cloudNowPlayingCoverRequested = false;
    vTaskDelete(nullptr);
}

void playerCoreCloudNowPlayingCoverStart(const char* fallbackUrl) {
    if (s_cloudNowPlayingCoverRequested) return;
    CloudTrackItem track{};
    if (!playerCoreCloudMusicNowPlayingTrack(&track) || !track.id[0]) return;
    strlcpy(s_cloudNowPlayingCoverFallback, fallbackUrl ? fallbackUrl : "", sizeof(s_cloudNowPlayingCoverFallback));
    char sdPath[48];
    snprintf(sdPath, sizeof(sdPath), "/cloudimg/np_%s.jpg", track.id);
    const char* url = track.coverUrl[0] ? track.coverUrl : s_cloudNowPlayingCoverFallback;
    if (SD_MMC.exists(sdPath) || !url[0]) {
        if (!url[0] && !SD_MMC.exists(sdPath)) {
            File marker = SD_MMC.open(sdPath, "w", true);
            if (marker) marker.close();
        }
        return; // already cached (or nothing to fetch) -- no task needed
    }
    s_cloudNowPlayingCoverRequested = true;
    if (xTaskCreatePinnedToCore(cloudNowPlayingCoverTask, "npCover", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        s_cloudNowPlayingCoverRequested = false;
        MWR_LOG_ERROR("Failed to create now-playing cover task");
    }
}

bool playerCoreCloudNowPlayingCoverDecode(uint8_t scaleFactor, uint16_t** outPixels, uint16_t* outWidth,
                                          uint16_t* outHeight) {
    *outPixels = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    CloudTrackItem track{};
    if (!playerCoreCloudMusicNowPlayingTrack(&track) || !track.id[0]) return false;
    char sdPath[48];
    snprintf(sdPath, sizeof(sdPath), "/cloudimg/np_%s.jpg", track.id);
    File in = SD_MMC.open(sdPath, "r");
    if (!in) return false;
    const size_t len = in.size();
    if (len == 0) {
        in.close();
        return false;
    }
    uint8_t* data = static_cast<uint8_t*>(ps_malloc(len));
    if (!data) {
        in.close();
        return false;
    }
    const size_t readLen = in.read(data, len);
    in.close();
    if (readLen != len) {
        free(data);
        return false;
    }
    const bool ok = getTFT().decodeJpgFromMemory(data, len, scaleFactor, outPixels, outWidth, outHeight);
    free(data);
    return ok;
}

// ---- Cloud lyrics (网易云网络歌词) -----------------------------------------
// Fetched once per track from the gateway's /esp/v1/tracks/<id>/lyrics
// (upstream /lyric -> parsed LRC) and kept in PSRAM -- separate storage and
// state from the LOCAL player's lyrics (ID3 SYLT + lrclib.net), but the UI
// interaction is the same: currentLyricLine(positionMs) drives the detail
// row on the cloud now-playing page.
static constexpr uint16_t kCloudMaxLyricLines = 80;
struct CloudLyricLine {
    uint32_t ms = 0;
    char text[96]{};
};
static CloudLyricLine* s_cloudLyricLines = nullptr; // PSRAM, allocated in setup()
static uint16_t s_cloudLyricCount = 0;
static char s_cloudLyricTrackId[24] = "";
static uint8_t s_cloudLyricsState = 0; // CloudLyricsState
static uint32_t s_cloudLyricGeneration = 0;

static void cloudLyricsFetchTask(void* param) {
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
    String trackId;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        trackId = s_cloudLyricTrackId;
        s_cloudLyricsState = static_cast<uint8_t>(CloudLyricsState::Loading);
        cloudResultUnlock();
    }
    if (!trackId.length() || !WiFi.isConnected()) {
        if (cloudResultLock(pdMS_TO_TICKS(1000))) {
            if (generation == s_cloudLyricGeneration) s_cloudLyricsState = static_cast<uint8_t>(CloudLyricsState::Error);
            cloudResultUnlock();
        }
        vTaskDelete(nullptr);
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "/esp/v1/tracks/%s/lyrics", trackId.c_str());
    String body;
    bool ok = false;
    // Small local retry loop -- cloudMusicHttpGetWithRetry is tied to the
    // browse command generation, which must not abort this fetch.
    for (uint8_t attempt = 0; attempt < 3 && !ok; ++attempt) {
        ok = cloudMusicHttpGet(path, body);
        if (!ok && attempt + 1 < 3) vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        if (generation != s_cloudLyricGeneration) {
            cloudResultUnlock();
            vTaskDelete(nullptr);
            return;
        }
        s_cloudLyricCount = 0;
        if (!ok) {
            s_cloudLyricsState = static_cast<uint8_t>(CloudLyricsState::Error);
        } else {
            String items[kCloudMaxLyricLines];
            const uint8_t n = jsonArrayItems(body, "synced", items, kCloudMaxLyricLines);
            if (s_cloudLyricLines && n) {
                for (uint8_t i = 0; i < n; ++i) {
                    double ms = 0;
                    String text;
                    if (jsonNumber(items[i], "time_ms", &ms) && jsonStringField(items[i], "text", text) && text.length()) {
                        s_cloudLyricLines[s_cloudLyricCount].ms = static_cast<uint32_t>(ms);
                        strlcpy(s_cloudLyricLines[s_cloudLyricCount].text, text.c_str(), sizeof(s_cloudLyricLines[0].text));
                        cloudTextSanitize(s_cloudLyricLines[s_cloudLyricCount].text);
                        ++s_cloudLyricCount;
                    }
                }
            }
            s_cloudLyricsState = static_cast<uint8_t>(s_cloudLyricCount ? CloudLyricsState::Loaded : CloudLyricsState::NotFound);
        }
        cloudResultUnlock();
    }
    vTaskDelete(nullptr);
}

void playerCoreCloudMusicLyricsStart(const char* trackId) {
    if (!trackId || !trackId[0]) return;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        if (strcmp(s_cloudLyricTrackId, trackId) == 0 && s_cloudLyricsState == static_cast<uint8_t>(CloudLyricsState::Loaded)) {
            cloudResultUnlock();
            return; // already have this track's lyrics
        }
        strlcpy(s_cloudLyricTrackId, trackId, sizeof(s_cloudLyricTrackId));
        s_cloudLyricsState = static_cast<uint8_t>(CloudLyricsState::Loading);
        const uint32_t generation = ++s_cloudLyricGeneration;
        cloudResultUnlock();
        if (xTaskCreatePinnedToCore(cloudLyricsFetchTask, "cloudLyr", 8192,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(generation)), 1, nullptr, 0) != pdPASS) {
            if (cloudResultLock(pdMS_TO_TICKS(1000))) {
                s_cloudLyricsState = static_cast<uint8_t>(CloudLyricsState::Error);
                cloudResultUnlock();
            }
            MWR_LOG_ERROR("Failed to create cloud lyrics fetch task");
        }
    }
}

uint8_t playerCoreCloudMusicLyricsState() { return s_cloudLyricsState; }

bool playerCoreCloudMusicLyricsForTrack(const char* trackId) {
    if (!trackId || !trackId[0]) return false;
    bool ok = false;
    if (cloudResultLock()) {
        ok = s_cloudLyricsState == static_cast<uint8_t>(CloudLyricsState::Loaded) && strcmp(s_cloudLyricTrackId, trackId) == 0;
        cloudResultUnlock();
    }
    return ok;
}

const char* playerCoreCloudMusicCurrentLyricLine(uint32_t positionMs) {
    static char lineBuf[96];
    lineBuf[0] = '\0';
    if (cloudResultLock()) {
        if (s_cloudLyricLines && s_cloudLyricCount && s_cloudLyricsState == static_cast<uint8_t>(CloudLyricsState::Loaded)) {
            int32_t idx = -1;
            for (uint16_t i = 0; i < s_cloudLyricCount; ++i) {
                if (s_cloudLyricLines[i].ms <= positionMs) idx = i;
                else break;
            }
            if (idx >= 0) strlcpy(lineBuf, s_cloudLyricLines[idx].text, sizeof(lineBuf));
        }
        cloudResultUnlock();
    }
    return lineBuf;
}

// Consume-once (same idiom as playerCoreLyricsOnlineReady): true exactly
// once per successfully-started cloud track, after which PlayerService::
// tick() has applied *outTrack to the snapshot and this returns false again
// until the next resolve succeeds.
bool playerCoreCloudMusicConsumeNowPlaying(CloudTrackItem* outTrack) {
    if (!outTrack) return false;
    bool ready = false;
    if (cloudResultLock()) {
        if (s_cloudResolveState == CloudMusicRequestState::Loaded) {
            *outTrack = s_cloudNowPlaying;
            s_cloudResolveState = CloudMusicRequestState::Idle; // consumed
            ready = true;
        }
        cloudResultUnlock();
    }
    return ready;
}

bool playerCoreCloudMusicJustStarted() {
    // Read-modify-write under the same mutex the controller task uses when
    // setting the latch -- otherwise a cleared-false race could eat the
    // "jump to Now Playing" navigation for a track that just started.
    bool v = false;
    if (cloudResultLock(pdMS_TO_TICKS(1000))) {
        v = s_cloudPlaybackJustStarted;
        s_cloudPlaybackJustStarted = false;
        cloudResultUnlock();
    }
    return v;
}

// setAudioFilePosition() takes a byte offset, not seconds -- approximates a
// target byte from a linear time/size ratio (exact for CBR MP3, a close
// enough estimate for VBR, same tradeoff any simple player's seek bar
// makes without a full VBR seek table).
bool playerCoreSeekTo(uint32_t positionSeconds) {
    const uint32_t duration = audio.getAudioFileDuration();
    if (!duration) return false;
    const uint32_t dataStart = audio.getAudioDataStartOffset();
    const uint32_t fileSize = audio.getFileSize();
    if (fileSize <= dataStart) return false;
    const uint32_t dataSize = fileSize - dataStart;
    const uint32_t clampedSeconds = std::min(positionSeconds, duration);
    const uint32_t targetByte = dataStart + static_cast<uint32_t>((static_cast<uint64_t>(dataSize) * clampedSeconds) / duration);
    const bool ok = audio.setAudioFilePosition(targetByte);
    printf("[SEEK] to %us -> byte %u (dataStart=%u size=%u) ok=%d\n", positionSeconds, targetByte, dataStart, dataSize, ok);
    return ok;
}

void playerCoreReadSnapshot(PlayerSnapshot* snapshot) {
    if (!snapshot) return;

    snapshot->wifiConnected = WiFi.isConnected();
    snapshot->muted = s_f_mute;
    snapshot->volume = s_volume.cur_volume;
    snapshot->volumeSteps = s_volume.volumeSteps;
    {
        // getVUlevel() packs both channels into one uint16_t (low byte
        // left, high byte right) -- previously stored straight into a
        // uint8_t vuLevel, which silently truncated to the low byte and
        // threw the right channel away. Split explicitly now that the UI
        // wants both (see PlayerSnapshot::vuRight).
        const uint16_t vu = audio.getVUlevel();
        snapshot->vuLevel = static_cast<uint8_t>(vu & 0xFF);
        snapshot->vuRight = static_cast<uint8_t>((vu >> 8) & 0xFF);
    }
    audio.getSpectrumBands(snapshot->spectrumBands);
    snapshot->positionSeconds = audio.getAudioCurrentTime();
    snapshot->durationSeconds = audio.getAudioFileDuration();
    snapshot->source = s_f_isFSConnected
                            ? PlayerSource::Sd
                            : (s_f_isWebConnected ? (s_cloudMusicPlaying ? PlayerSource::CloudMusic : PlayerSource::Radio) : PlayerSource::None);

    if (s_f_webFailed) {
        snapshot->transport = PlayerTransport::Error;
    } else if (s_f_pauseResume) {
        snapshot->transport = PlayerTransport::Paused;
    } else if (audio.isRunning()) {
        snapshot->transport = PlayerTransport::Playing;
    } else if (s_f_isFSConnected || s_f_isWebConnected) {
        snapshot->transport = PlayerTransport::Buffering;
    } else {
        snapshot->transport = PlayerTransport::Stopped;
    }

    // getStationName()/s_streamTitle are radio-only (ICY station name /
    // stream title) but were being written into title/detail on every tick
    // regardless of source -- for local file playback this stomped the
    // real ID3 title/artist that PlayerService::playLocalTrack() had just
    // set, within one refresh cycle (~100ms), so the UI kept showing
    // whatever radio station had played most recently instead of the
    // actual local track. Local title/detail are instead only ever set
    // once, at play time, and left alone here.
    if (snapshot->source == PlayerSource::Radio) {
        ps_ptr<char> station = getStationName();
        strlcpy(snapshot->title, station.c_get(), sizeof(snapshot->title));
        strlcpy(snapshot->detail, s_streamTitle.c_get(), sizeof(snapshot->detail));
        snapshot->radioStationIndex = s_cur_station;
    }
    if (snapshot->source == PlayerSource::CloudMusic) {
        // Cloud track title/artist come from the last resolve response (set
        // once at play time by cloudMusicControllerTask) -- the audio lib
        // reports no ICY-style metadata for a plain CDN file URL, so leave
        // the values alone here instead of overwriting with station data.
        if (cloudResultLock()) {
            strlcpy(snapshot->title, s_cloudNowPlaying.title, sizeof(snapshot->title));
            if (s_cloudNowPlaying.artist[0]) {
                snprintf(snapshot->detail, sizeof(snapshot->detail), "%s", s_cloudNowPlaying.artist);
            } else {
                snapshot->detail[0] = '\0';
            }
            cloudResultUnlock();
        }
    }
    if (s_f_webFailed) strlcpy(snapshot->error, s_streamTitle.c_get(), sizeof(snapshot->error));

    strlcpy(snapshot->codec, audio.getCodecname(), sizeof(snapshot->codec));
    snapshot->sampleRate = audio.getSampleRate();
    snapshot->bitsPerSample = audio.getBitsPerSample();
    snapshot->bitRate = audio.getBitRate();

    const uint32_t bufferSize = audio.getInBufferSize();
    uint32_t fillPercent = bufferSize ? (audio.inBufferFilled() * 100UL / bufferSize) : 0;
    if (fillPercent > 100) fillPercent = 100;
    snapshot->bufferFillPercent = static_cast<uint8_t>(fillPercent);

    // rtc.hasValidTime() reads a `timeinfo.tm_year` field that RTIME only
    // refreshes as a side effect of calling one of its own getters
    // (gettime_xs/gettime_l/...) -- called on its own, before anything else
    // has touched that cache, it reads the constructor's all-zero value
    // forever and the clock never appears. Check the real epoch instead:
    // anything before ~2023 means SNTP hasn't synced yet.
    time_t rawNow = time(nullptr);
    if (rawNow > 1700000000) {
        struct tm ti;
        localtime_r(&rawNow, &ti);
        snprintf(snapshot->timeHM, sizeof(snapshot->timeHM), "%02d:%02d", ti.tm_hour, ti.tm_min);
        static const char* kWeekdayCn[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        static const char* kMonthEn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(snapshot->dateStr, sizeof(snapshot->dateStr), "%s %02d %s", kWeekdayCn[ti.tm_wday % 7], ti.tm_mday, kMonthEn[ti.tm_mon % 12]);
    } else {
        snapshot->timeHM[0] = '\0';
        snapshot->dateStr[0] = '\0';
    }
    snapshot->wifiRssi = snapshot->wifiConnected ? static_cast<int8_t>(WiFi.RSSI()) : 0;
    if (snapshot->wifiConnected) {
        strlcpy(snapshot->wifiSsid, WiFi.SSID().c_str(), sizeof(snapshot->wifiSsid));
        strlcpy(snapshot->wifiIp, WiFi.localIP().toString().c_str(), sizeof(snapshot->wifiIp));
    } else {
        snapshot->wifiSsid[0] = '\0';
        snapshot->wifiIp[0] = '\0';
    }
    snapshot->wifiApFallbackActive = s_wifiApFallbackActive;
    if (s_wifiApFallbackActive) {
        strlcpy(snapshot->wifiApSsid, kWifiSetupApSsid, sizeof(snapshot->wifiApSsid));
        strlcpy(snapshot->wifiApIp, WiFi.softAPIP().toString().c_str(), sizeof(snapshot->wifiApIp));
    } else {
        snapshot->wifiApSsid[0] = '\0';
        snapshot->wifiApIp[0] = '\0';
    }
    snapshot->eofCount = s_eofCount;

    if (s_weatherMutex && xSemaphoreTake(s_weatherMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_weather.valid) {
            snapshot->weatherTempC = s_weather.tempC;
            strlcpy(snapshot->weatherDesc, s_weather.desc, sizeof(snapshot->weatherDesc));
            snapshot->weatherIcon = s_weather.iconCategory;
        } else {
            snapshot->weatherTempC = -999;
            snapshot->weatherDesc[0] = '\0';
            snapshot->weatherIcon = -1;
        }
        xSemaphoreGive(s_weatherMutex);
    }
}

#if MWR_LVGL_UI
static volatile bool s_lvglNetworkReady = false;

// Everything that depends on having a working connection -- started once,
// whether that connection came from the initial boot-time connectToWiFi()
// or from a network added later via the Settings > WiFi screen (see
// wifiAddTask()). Guarded by s_lvglNetworkReady so a later reconnect never
// re-runs it (double MDNS.begin()/webSrv.begin() etc. would be harmless at
// worst, but there's no reason to repeat it).
static void onWifiNetworkReady() {
    if (s_lvglNetworkReady) return;
    setRTC(s_TZString);
    webSrv.begin(80, 81);
    MDNS.begin("MiniWebRadio");
    MDNS.addService("esp32", "tcp", 80);
    // ArduinoOTA removed along with the ota_0 partition (see
    // boards/miniwebradio16MB_single.csv) -- network OTA was never actually
    // used (every flash this session was a direct USB esptool write), and
    // the second 6MB app slot it needed was worth far more as extra room
    // for the CJK font subset.
    ftpSrv.begin(SD_MMC, FTP_USERNAME, FTP_PASSWORD);
    s_f_dlnaSeekServer = true;
    s_lvglNetworkReady = true;
    if (!s_weatherMutex) s_weatherMutex = xSemaphoreCreateMutex();
    if (xTaskCreatePinnedToCore(weatherTask, "weather", 10240, nullptr, 1, nullptr, 0) != pdPASS) MWR_LOG_ERROR("Failed to create weather task");
    // Phase 3 验收：联网后拉一次真实候选。**只发现，不下载任何文件。**
    if (s_jamendo.available() && !s_jamendoProbeDone) {
        downloadSetAudioFillFn(audioFillPercentForDownload);   // 自适应节流
        // 电台卡顿的实测结果（2026-09-05）：下载要对网络音频限速 + 让路。
        downloadSetNetAudioFn(netAudioActiveForDownload);
        dailySyncSetNetAudioFn(netAudioActiveForDownload);
        if (xTaskCreatePinnedToCore(jamendoProbeTask, "jamendoProbe", 10240, nullptr, 1, nullptr, 0) != pdPASS)
            MWR_LOG_ERROR("Failed to create jamendo probe task");
    }
}

// startWifiApFallback() (WiFi.mode(WIFI_AP_STA) + softAP + DNSServer) is
// temporarily disabled -- it reliably caused the device to hang before
// app_main even started (no panic, no "Setup:" print, nothing at all) --
// reproduced on 5+ clean flash+boot cycles, survived both disabling
// LV_USE_QRCODE and switching DNSServer off the global-constructor path, so
// the actual cause is still unidentified. Needs real hardware debugging
// (JTAG, or bisecting down to just the bare WiFi.softAP() call) before
// re-enabling. The /wifi_manage admin page (see WEBSRV_onCommand) still
// works once the device is on some known network -- only the zero-network
// SoftAP bootstrap path is unavailable right now.

// ---- WiFi saved-list + reconnect (Settings > WiFi screen) -----------------
// The device itself no longer scans (that used its own background task and
// competed with audio/WiFi/LVGL for the same limited internal RAM at boot
// for no real benefit) -- it only ever lists what's already saved in NVS.
// Discovering and adding brand-new networks moved entirely to the phone-
// only /wifi_manage page below, which does its own on-demand
// WiFi.scanNetworks() when loaded rather than something running at boot.

uint8_t playerCoreWifiSavedCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < kWifiSlotCount; ++i) {
        ps_ptr<char> line = wifiPrefGet(i);
        if (playerCoreWifiLineIsSavedNetwork(line.c_get())) ++count;
    }
    return count;
}

bool playerCoreWifiSavedInfo(uint8_t index, char* outSsid, size_t ssidSize, bool* outIsDefault) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < kWifiSlotCount; ++i) {
        ps_ptr<char> line = wifiPrefGet(i);
        if (!playerCoreWifiLineIsSavedNetwork(line.c_get())) continue;
        if (seen == index) {
            const int pos = line.index_of("\t", 0);
            if (pos > 0) line[pos] = '\0';
            strlcpy(outSsid, line.get(), ssidSize);
            if (outIsDefault) *outIsDefault = (i == 0);
            return true;
        }
        ++seen;
    }
    return false;
}

// wifiMulti already has every saved AP added (see connectToWiFi() at boot);
// tapping a saved row just asks it to retry right now instead of waiting
// for loopLvglRuntime()'s once-a-second opportunistic run().
static volatile bool s_wifiReconnectInProgress = false;
static uint32_t s_wifiRetryBackoffSec = 1;
static uint32_t s_wifiNextRetryAtSec = 0;

static bool runWifiMultiOnce(const char* reason, TickType_t lockTimeout = pdMS_TO_TICKS(1000)) {
    if (!lockWifiOps(lockTimeout)) {
        MWR_LOG_WARN("WiFi operation busy; skipped {}", reason ? reason : "wifiMulti.run()");
        return false;
    }
    wifiMulti.setStrictMode(false);
    wifiMulti.run();
    unlockWifiOps();
    return true;
}

static void wifiReconnectTask(void*) {
    const bool ran = runWifiMultiOnce("reconnect", pdMS_TO_TICKS(15000));
    if (ran && WiFi.status() == WL_CONNECTED) {
        s_f_isWiFiConnected = true;
        s_wifiRetryBackoffSec = 1;
        s_wifiNextRetryAtSec = s_totalRuntime + 1;
        onWifiNetworkReady();
    }
    s_wifiReconnectInProgress = false;
    vTaskDelete(nullptr);
}

static bool startWifiReconnectTask() {
    if (s_wifiReconnectInProgress) return false;
    s_wifiReconnectInProgress = true;
    const BaseType_t created = xTaskCreatePinnedToCore(wifiReconnectTask, "wifiReconn", 4096, nullptr, 1, nullptr, 0);
    if (created != pdPASS) {
        s_wifiReconnectInProgress = false;
        MWR_LOG_ERROR("Failed to create WiFi reconnect task");
        return false;
    }
    return true;
}

void playerCoreWifiReconnect() { startWifiReconnectTask(); }

struct WifiAddRequest {
    char ssid[33]{};
    char password[64]{};
};
static volatile bool s_wifiAddInProgress = false;

static void wifiAddTask(void* param) {
    auto* request = static_cast<WifiAddRequest*>(param);
    ps_ptr<char> ssidPtr = request->ssid;
    ps_ptr<char> pwPtr = request->password;
    setWiFiCredentials(ssidPtr, pwPtr); // persists to the next free slot (or updates a matching one)
    wifiMulti.addAP(request->ssid, request->password);
    runWifiMultiOnce("wifi add", pdMS_TO_TICKS(15000));
    bool connected = false;
    for (uint8_t i = 0; i < 30; ++i) { // up to 15s, same ballpark as connectToWiFi()'s own retry window
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (connected) {
        s_f_isWiFiConnected = true;
        onWifiNetworkReady();
    }
    free(request);
    s_wifiAddInProgress = false;
    vTaskDelete(nullptr);
}

// Only called from the phone-only /wifi_manage web page now (see
// WEBSRV_onCommand's "wifi_connect") -- the device's own Settings > WiFi
// screen just lists saved networks (see playerCoreWifiSavedCount() above).
void playerCoreWifiAddNetwork(const char* ssid, const char* password) {
    if (s_wifiAddInProgress || !ssid || strlen(ssid) < 1) return;
    auto* request = static_cast<WifiAddRequest*>(malloc(sizeof(WifiAddRequest)));
    if (!request) return;
    *request = WifiAddRequest{};
    strlcpy(request->ssid, ssid, sizeof(request->ssid));
    if (password) strlcpy(request->password, password, sizeof(request->password));
    s_wifiAddInProgress = true;
    const BaseType_t created = xTaskCreatePinnedToCore(wifiAddTask, "wifiAdd", 6144, request, 1, nullptr, 0);
    if (created != pdPASS) {
        s_wifiAddInProgress = false;
        free(request);
        MWR_LOG_ERROR("Failed to create WiFi add task");
    }
}

// On-demand only (see WifiScanItem's comment), and asynchronous: an earlier
// version called WiFi.scanNetworks() directly from the LVGL button-click
// handler, blocking the same task that runs audio.loop()/the UI tick for
// the several seconds a scan takes -- long enough to starve things badly
// enough that tapping "手动添加" reliably froze/rebooted the device. Runs
// on its own task instead; HifiUi polls playerCoreWifiScanInProgress() and
// only calls playerCoreWifiScanResults() once it flips back to false.
static constexpr uint8_t kWifiScanMaxItems = 12;
static WifiScanItem s_wifiScanResults[kWifiScanMaxItems];
static uint8_t s_wifiScanResultCount = 0;
static volatile bool s_wifiScanInProgress = false;

static void wifiScanTask(void*) {
    int16_t n = 0;
    uint8_t count = 0;
    if (lockWifiOps(pdMS_TO_TICKS(15000))) {
        n = WiFi.scanNetworks();
        for (int16_t i = 0; i < n && count < kWifiScanMaxItems; ++i) {
            const String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue; // hidden network -- nothing to show/select
            bool dup = false;
            for (uint8_t j = 0; j < count; ++j) {
                if (strcmp(s_wifiScanResults[j].ssid, ssid.c_str()) == 0) {
                    dup = true;
                    if (WiFi.RSSI(i) > s_wifiScanResults[j].rssi) s_wifiScanResults[j].rssi = WiFi.RSSI(i);
                    break;
                }
            }
            if (dup) continue;
            strlcpy(s_wifiScanResults[count].ssid, ssid.c_str(), sizeof(s_wifiScanResults[count].ssid));
            s_wifiScanResults[count].rssi = WiFi.RSSI(i);
            ++count;
        }
        WiFi.scanDelete();
        unlockWifiOps();
    } else {
        MWR_LOG_WARN("WiFi operation busy; skipped async scan");
    }
    // Strongest signal first -- simple insertion sort, count is tiny (<= kWifiScanMaxItems).
    for (uint8_t i = 1; i < count; ++i) {
        WifiScanItem key = s_wifiScanResults[i];
        int j = i - 1;
        while (j >= 0 && s_wifiScanResults[j].rssi < key.rssi) { s_wifiScanResults[j + 1] = s_wifiScanResults[j]; --j; }
        s_wifiScanResults[j + 1] = key;
    }
    s_wifiScanResultCount = count;
    s_wifiScanInProgress = false;
    vTaskDelete(nullptr);
}

void playerCoreWifiScanStart() {
    if (s_wifiScanInProgress) return;
    s_wifiScanInProgress = true;
    const BaseType_t created = xTaskCreatePinnedToCore(wifiScanTask, "wifiScan", 4096, nullptr, 1, nullptr, 0);
    if (created != pdPASS) {
        s_wifiScanInProgress = false;
        s_wifiScanResultCount = 0;
        MWR_LOG_ERROR("Failed to create WiFi scan task");
    }
}

bool playerCoreWifiScanInProgress() { return s_wifiScanInProgress; }

uint8_t playerCoreWifiScanResults(WifiScanItem* items, uint8_t maxItems) {
    const uint8_t n = std::min<uint8_t>(s_wifiScanResultCount, maxItems);
    for (uint8_t i = 0; i < n; ++i) items[i] = s_wifiScanResults[i];
    return n;
}

// This is deliberately separate from the historical setup sequence.  It keeps
// MiniWebRadio's services alive without constructing any legacy screen/widget.
static bool setupLvglRuntime() {
    printfln(s_tag.setup, ANSI_ESC_GREEN "[LVGL] runtime setup begin");
    s_h_resolution = 320;
    s_v_resolution = 170;

    logMemoryState("runtime_start");
    if (!init_SD_card()) return false;
    ensureLocalMusicDir();
#if MWR_USB_DAC_SUPPORTED
    if (usbDacModeFlag()) {
        // USB 声卡模式：不起 WiFi / 音频播放器 / 音乐扫描，只跑 UAC -> I2S。
        // 屏幕照常工作，是这个模式下唯一的仪表盘——USB 口被 TinyUSB 占用后
        // USB-Serial/JTAG 控制台会消失，printf 什么都抓不到。
        s_usbDacModeActive = true;
        if (!usbDacBegin()) {
            MWR_LOG_ERROR("USB DAC mode init failed -- clearing flag and rebooting to normal mode");
            usbDacSetModeFlag(false);
            s_usbDacModeActive = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP.restart();
            return false;
        }
        printfln(s_tag.setup, ANSI_ESC_GREEN "[LVGL] display begin (USB DAC mode)");
        if (!lvglRuntimeBegin()) {
            MWR_LOG_ERROR("LVGL display begin failed in USB DAC mode");
            usbDacSetModeFlag(false);
            s_usbDacModeActive = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP.restart();
            return false;
        }
        lvglRuntimeShowUsbDacPage();
        return true;
    }
#endif
#if MWR_USB_MSC_SUPPORTED
    if (usbMscModeFlag()) {
        // USB storage mode: expose the SD card to the host immediately.
        // TinyUSB must be initialized at boot (runtime USB.begin() is
        // unsafe -- ESP_RST_USB, see DEV_LOG 2026-07-31), so this mode is
        // entered by writing an NVS flag and rebooting. Skip WiFi, audio
        // and the music scan; keep only the minimal USB page.
        s_usbMscModeActive = true;
        if (!usbStoragePrepareMsc(true)) {
            MWR_LOG_ERROR("USB storage mode init failed -- clearing flag and rebooting to normal mode");
            usbMscSetModeFlag(false);
            s_usbMscModeActive = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP.restart();
            return false;
        }
        s_usbStorageState = UsbStorageState::Mounted;
        printfln(s_tag.setup, ANSI_ESC_GREEN "[LVGL] display begin (USB storage mode)");
        if (!lvglRuntimeBegin()) {
            MWR_LOG_ERROR("LVGL display begin failed in USB storage mode");
            usbMscSetModeFlag(false);
            s_usbMscModeActive = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP.restart();
            return false;
        }
        lvglRuntimeShowUsbStoragePage();
        return true;
    }
    // Normal mode: never start TinyUSB at boot (the single USB-C port
    // stays a serial console). Only probe the SD geometry/format so the
    // USB page can display FAT/allocation/capacity before mount.
    if (!usbStorageProbeGeometry()) {
        MWR_LOG_WARN("USB storage geometry probe failed (SD may be absent)");
    }
#endif
    startLocalMusicScan();
    if (!defaultsettings()) return false;
    if (ESP.getFlashChipSize() > 80000000) FFat.begin();
    logMemoryState("after_sd_init");
    updateSettings();

    // WiFi connects HERE, before audio claims its own ~73KB (I2S DMA + the
    // audio task's own buffers) -- real measurements on this board: audio
    // alone drops free heap from ~110KB to ~37KB, and esp_wifi_init() needs
    // more than that remainder ever reliably had (tried shrinking its own
    // buffer counts via sdkconfig first; didn't help, so the two truly
    // don't both fit at once here). Whichever claims memory second starves
    // -- and audio must never fail (no sound is worse than a few seconds of
    // no network), so WiFi goes first. connectToWiFi() is bounded (~11s
    // worst case with nothing in range) so this can't hang boot forever;
    // a later reconnect (new network added via Settings > WiFi, or simply
    // coming back into range) is still handled every second in
    // loopLvglRuntime()'s s_f_1sec block regardless of this initial result.
    logMemoryState("before_wifi_init");
    s_f_isWiFiConnected = connectToWiFi();
    if (s_f_isWiFiConnected) onWifiNetworkReady();
    logMemoryState("after_wifi_connected");

    logMemoryState("before_audio_init");
    audio.setAudioTaskCore(AUDIOTASK_CORE);
    audio.setConnectionTimeout(CONN_TIMEOUT, CONN_TIMEOUT_SSL);
    audio.setVolumeSteps(s_volume.volumeSteps);
    audio.setVolume(0);
    // Was trimmed to 16 (from the library default of 32) back when the
    // internal-RAM budget was so tight that WiFi and audio almost didn't
    // both fit at all -- see the LV_ATTRIBUTE_LARGE_RAM_ARRAY comment in
    // lv_conf.h for how that changed. Restored to the library default now
    // that LVGL's own 96KB pool no longer competes for this same internal
    // RAM: measured ~110KB free after audio+WiFi+LVGL init with 16
    // descriptors, comfortably more than the ~37KB the full 32 costs (was
    // ~2.3KB/descriptor at 16), and the crackling this caused in local
    // playback was a direct, reported consequence of running below default.
    audio.settings.DMA_DESC_NUM = 32;
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
    audio.setI2SCommFMT_LSB(I2S_COMM_FMT);
    setI2STone();
    playerCoreSetOutputPolicy(s_audioOutputPolicy, false);
    audio.settings.SPECTRUM = true; // real per-band FFT for the LVGL spectrum UI, see getSpectrumBands()
    logMemoryState("after_audio_set_pinout");

    if (AMP_ENABLED >= 0) {
        pinMode(AMP_ENABLED, OUTPUT);
        digitalWrite(AMP_ENABLED, HIGH);
    }

    ticker100ms.attach(0.1, timer100ms);
    playerService.begin();
    printfln(s_tag.setup, ANSI_ESC_GREEN "[LVGL] display begin");
    logMemoryState("before_lvgl_init");
    if (!lvglRuntimeBegin()) {
        printfln(s_tag.setup, ANSI_ESC_RED "[LVGL] display begin failed");
        return false;
    }
    printfln(s_tag.setup, ANSI_ESC_GREEN "[LVGL] runtime ready");
    // lvglRuntimeBegin() above both inits LVGL (lv_init(), claiming
    // work_mem_int) and builds the Home screen, so this one snapshot covers
    // both "after_lvgl_init" and "after_home_screen_created" from the A/B
    // test plan -- there's no separate hook point between the two.
    logMemoryState("after_lvgl_init_and_home_screen");
    return true;
}

static void loopLvglRuntime() {
    processUsbStorage();
#if MWR_USB_DAC_SUPPORTED
    if (s_usbDacModeActive) {
        // 声卡模式：不跑 dlna/audio/web/ftp/播放器。音频搬运由 usb_dac.cpp 里
        // 那个钉在 core 0 的 I2S 任务负责，这里只维持 UI。
        usbDacLoop();
        lvglRuntimeTick();
        return;
    }
#endif
#if MWR_USB_MSC_SUPPORTED
    if (s_usbMscModeActive) {
        // USB storage mode: no audio/WiFi/web/dlna ticks -- just keep the
        // minimal USB page alive and watch for host eject (processUsbStorage
        // above turns that into a reboot back to normal mode).
        lvglRuntimeTick();
        return;
    }
#endif
    dlna.loop();
    audio.loop();
    if (s_lvglNetworkReady && !usbStorageBlocksSdAppAccess()) {
        webSrv.loop();
        ftpSrv.handleFTP();
    }
    bt_emitter.loop();
    playerService.tick();
    lvglRuntimeTick();

    if (s_f_playlistEnabled && !audio.isRunning() && !s_f_pauseResume) processPlaylist();

    if (s_f_100ms) {
        s_f_100ms = false;
        if (!s_f_mute && audio.getVolume() != s_volume.cur_volume) audio.setVolume(s_volume.cur_volume);
        if (s_f_mute && audio.getVolume() != 0) audio.setVolume(0);

        // R6 采样。只在播放时取样——没播放的时候水位没有意义。
        if (s_r6Sampling && audio.isRunning()) {
            const uint32_t filled = audio.inBufferFilled();
            if (!s_r6Capacity) s_r6Capacity = filled + audio.inBufferFree();
            if (filled < s_r6MinFilled) s_r6MinFilled = filled;
            s_r6SumFilled += filled;
            ++s_r6Samples;

            if (!filled) {
                s_r6Starved = true;
                ++s_r6ZeroCount;
                if (++s_r6ConsecZero > s_r6MaxConsecZero) s_r6MaxConsecZero = s_r6ConsecZero;
            } else {
                s_r6ConsecZero = 0;
            }
            if (s_r6Capacity && filled < s_r6Capacity / 4) ++s_r6LowCount;

            const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            const uint32_t dmaBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (dmaFree < s_r6DmaFreeMin) s_r6DmaFreeMin = dmaFree;
            if (dmaBlock < s_r6DmaBlockMin) s_r6DmaBlockMin = dmaBlock;
        }
    }

    if (s_f_1sec) {
        s_f_1sec = false;
        s_totalRuntime++;

        // 事件落盘。放在这里而不是播放路径里——见 libraryLogEvent() 的说明。
        libraryFlushEvents(audio.isRunning());

        // 曲库状态：每 10 秒打一次。
        //
        // 为什么要周期打而不是只在扫描结束时打一次：开机那一瞬间的 printf 在这块板
        // 上**基本抓不到**——app 启动时 USB 会重新枚举，带复位抓取会断线、不带复位
        // 抓取又错过开头。这条教训在 esp_lcd 迁移那轮就总结过（见
        // DEV_LOG_2026-09-04 §5），这次做曲库索引又踩了一遍，所以补上。
        if (s_localTrackCapacity && (s_totalRuntime % 10) == 0) {
            uint16_t missing = 0, favorite = 0, played = 0;
            for (uint16_t i = 0; i < s_localTrackCount; ++i) {
                if (s_localTracks[i].flags & kTrackFlagMissing) ++missing;
                if (s_localTracks[i].flags & kTrackFlagFavorite) ++favorite;
                if (s_localTracks[i].playCount) ++played;
            }
            printf("[LIB][STATE] tracks=%u/%u loaded_at_boot=%u missing=%u fav=%u played=%u "
                   "rec=%uB idx=%uKB psram_free=%luKB\n",
                   s_localTrackCount, s_localTrackCapacity, s_libLoadedAtBoot, missing, favorite, played,
                   static_cast<unsigned>(sizeof(TrackRecord)),
                   static_cast<unsigned>(s_localTrackCapacity * sizeof(TrackRecord) / 1024),
                   static_cast<unsigned long>(ESP.getFreePsram() / 1024));
            printf("[LIB][EVQ] queued=%u dropped=%lu\n",
                   s_libEventQueueCount, static_cast<unsigned long>(s_libEventDropped));
            // 空间评估。数字来自扫描时的一次快照，不是实时读 SD——见 s_cleanerPlan
            // 的注释。所以 free 不会随播放/下载实时变化，够用来确认策略是否合理。
            printf("[JAMENDO][STATE] selftest=%u(expect 2) client_id=%s probe_done=%d "
                   "probe_tracks=%u err=\"%s\"\n",
                   s_jamendoSelfTestParsed,
                   s_jamendo.available() ? "configured" : "MISSING",
                   s_jamendoProbeDone ? 1 : 0, s_jamendoProbeCount, s_jamendoProbeErr);
            if (s_dlTestDone) printf("[DL][STATE] %s\n", s_dlTestResult);
            if (s_r6Done) printf("[R6][STATE] %s\n", s_r6Result);

            // —— Phase 5 触发 ——
            // 条件：联网、provider 可用、RTC 已同步（dailySyncDue 内部判）、
            // 今天没跑过、当前没在跑。探测任务跑完再启动，避免两个任务
            // 同时抢 TLS 握手所需的内部 RAM。
            if (s_lvglNetworkReady && s_jamendo.available() && s_jamendoProbeDone &&
                !s_syncRunning && !s_syncDone &&
                s_totalRuntime >= s_syncRetryAtSec &&
                (kDailySyncForceOnBoot ||
                 dailySyncDue(s_syncState, s_f_rtc ? static_cast<uint32_t>(time(nullptr)) : 0))) {
                if (xTaskCreatePinnedToCore(dailySyncTask, "dailySync", 10240, nullptr, 1, nullptr, 0) == pdPASS) {
                    s_syncRunning = true;   // 立刻置位，别等任务调度起来才置，否则会重复创建
                    printf("[SYNC][STATE] started\n");
                } else {
                    MWR_LOG_ERROR("Failed to create daily sync task");
                }
            }
            if (s_syncRunning) {
                printf("[SYNC][STATE] running got=%u attempts=%u owned=%u failed=%u\n",
                       s_syncStats.downloaded, s_syncStats.attempts,
                       s_syncStats.skippedOwned, s_syncStats.failedDownload);
            } else if (!s_syncDone) {
                // ⚠️ 不触发时也要说明原因。否则"没跑"和"跑了但没输出"完全同形 ——
                // 2026-09-05 为此浪费了一轮 12 分钟的抓取。
                const uint32_t nowEp = s_f_rtc ? static_cast<uint32_t>(time(nullptr)) : 0;
                // ⚠️ epoch 用 time(nullptr) 直接取，**不要经过 s_f_rtc** ——
                // 否则"SNTP 没同步"和"标志位没置上"在日志里完全同形，
                // 2026-09-05 就是因为这个多绕了一圈。
                printf("[SYNC][STATE] idle rtc=%d probe=%d net=%d epoch=%lu day=%lu last=%lu "
                       "due=%d force=%d cooldown=%ld\n",
                       s_f_rtc ? 1 : 0, s_jamendoProbeDone ? 1 : 0,
                       s_lvglNetworkReady ? 1 : 0,
                       static_cast<unsigned long>(time(nullptr)),
                       static_cast<unsigned long>(nowEp / 86400u),
                       static_cast<unsigned long>(s_syncState.lastRunDay),
                       dailySyncDue(s_syncState, nowEp) ? 1 : 0,
                       kDailySyncForceOnBoot ? 1 : 0,
                       static_cast<long>(s_syncRetryAtSec) - static_cast<long>(s_totalRuntime));
            } else if (s_syncDone) {
                printf("[SYNC][STATE] done got=%u/%u attempts=%u owned=%u failed=%u "
                       "empty=%u nospace=%d deferred=%d %luKB %lums err=\"%s\"\n",
                       s_syncStats.downloaded, kDailySyncTracksPerDay,
                       s_syncStats.attempts, s_syncStats.skippedOwned,
                       s_syncStats.failedDownload, s_syncStats.emptyFetches,
                       s_syncStats.stoppedNoSpace ? 1 : 0,
                       s_syncStats.deferredNetAudio ? 1 : 0,
                       static_cast<unsigned long>(s_syncStats.bytes / 1024),
                       static_cast<unsigned long>(s_syncStats.elapsedMs),
                       s_syncStats.lastError);
            }
            else if (s_r6Sampling) printf("[R6][STATE] sampling n=%lu min=%lu\n",
                                          static_cast<unsigned long>(s_r6Samples),
                                          static_cast<unsigned long>(s_r6Samples ? s_r6MinFilled : 0));
            if (s_cleanerPlan.sdReadable) {
                printf("[CLEAN][STATE] free=%lluMB reserve=%lluMB need_clean=%d "
                       "candidates=%u reclaim=%lluMB user_owned=%u\n",
                       s_cleanerPlan.freeBytes / (1024 * 1024),
                       s_cleanerPlan.reserveBytes / (1024 * 1024),
                       s_cleanerPlan.needsCleaning ? 1 : 0,
                       s_cleanerPlan.candidateCount,
                       s_cleanerPlan.reclaimableBytes / (1024 * 1024),
                       s_cleanerPlan.nonDiscoveryCount);
            }
        }

        if (s_memLogRadioAtSec && s_totalRuntime >= s_memLogRadioAtSec) {
            logMemoryState("radio_playing_stable");
            s_memLogRadioAtSec = 0;
        }
        if (s_memLogLocalAtSec && s_totalRuntime >= s_memLogLocalAtSec) {
            logMemoryState("local_music_playing_stable");
            s_memLogLocalAtSec = 0;
        }
        if (!s_f_rtc) s_f_rtc = systemTimeSynced();
        if (WiFi.isConnected()) {
            s_f_WiFi_lost = false;
            // Covers the case setupLvglRuntime()'s initial connectToWiFi()
            // attempt failed (nothing in range yet) but a later retry here
            // succeeded -- onWifiNetworkReady() itself is idempotent
            // (guarded by s_lvglNetworkReady) so this is safe to call every
            // tick once connected, not just the first time.
            if (!s_lvglNetworkReady) {
                s_f_isWiFiConnected = true;
                onWifiNetworkReady();
            }
        } else {
            s_f_WiFi_lost = true;
            // Retries regardless of s_lvglNetworkReady -- previously this
            // only fired once already connected once, so a device that
            // never got WiFi up at boot would never retry at all.
            // But only if there's actually something saved to try: with
            // zero saved networks (e.g. right after an NVS wipe),
            // WiFiMulti::run() falls back to a full synchronous
            // WiFi.scanNetworks() every single call -- done here once a
            // second, that blocked the main loop long enough to make touch
            // feel unresponsive (LVGL's touch polling shares this loop).
            //
            // Even with a real saved network, every failed run() still does
            // that same blocking scan -- if the network is simply out of
            // range or briefly missed by a scan, retrying every single
            // second means touch/spectrum stay laggy for as long as it's
            // unreachable. Back off after repeated failures (capped at 16s)
            // and reset to retrying every second as soon as one succeeds,
            // so a normally-reachable network still reconnects quickly.
            if (playerCoreWifiSavedCount() > 0 && s_totalRuntime >= s_wifiNextRetryAtSec) {
                if (startWifiReconnectTask()) {
                    s_wifiNextRetryAtSec = s_totalRuntime + s_wifiRetryBackoffSec;
                    s_wifiRetryBackoffSec = std::min<uint32_t>(s_wifiRetryBackoffSec * 2, 16);
                }
            }
        }
        if (s_f_stationsChanged) {
            s_f_stationsChanged = false;
            staMgnt.updateStationsList();
        }
    }
}
#endif

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// 📌📌📌  S E T U P  📌📌📌
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————

void setup() {
    //---- BEGIN ---------
    Serial.begin(MONITOR_SPEED);
    vTaskDelay(1500); // wait for Serial to be ready
    printf("\n\n");
    printf("[BOOT] resetReason=%d\n", static_cast<int>(esp_reset_reason()));
    fflush(stdout);
    trim(Version);
    printfln(s_tag.none, "");
    printfln(s_tag.none, "             " ANSI_ESC_YELLOW " ***************************************************** ");
    printfln(s_tag.none, "             " ANSI_ESC_YELLOW " *     MiniWebRadio {:29}    * " ANSI_ESC_RESET "      ", Version);
    printfln(s_tag.none, "             " ANSI_ESC_YELLOW " ***************************************************** ");
    printfln(s_tag.none, "");

    mutex_rtc = xSemaphoreCreateMutex();
    mutex_display = xSemaphoreCreateMutex();
    s_prefMutex = xSemaphoreCreateMutex();
    s_wifiOpMutex = xSemaphoreCreateMutex();
    s_cloudMusicMutex = xSemaphoreCreateMutex();
    if (!s_prefMutex) MWR_LOG_ERROR("Failed to create Preferences mutex");
    if (!s_wifiOpMutex) MWR_LOG_ERROR("Failed to create WiFi operation mutex");
    if (!s_cloudMusicMutex) MWR_LOG_ERROR("Failed to create cloud music config mutex");
    s_cloudResultMutex = xSemaphoreCreateMutex();
    s_cloudCommandQueue = xQueueCreate(1, sizeof(CloudCommand));
    if (!s_cloudResultMutex) MWR_LOG_ERROR("Failed to create cloud music result mutex");
    // Cloud result arrays live in PSRAM -- internal RAM is the constraint
    // for TLS handshakes (~40KB contiguous heap); keeping ~24KB of list
    // data out of it is what lets WiFiClientSecure actually connect (the
    // "网关一直唤醒不了" bug: free internal heap had collapsed and TLS
    // allocation failed every attempt, while plain-HTTP radio kept working).
    s_cloudSearchResults = static_cast<CloudTrackItem*>(ps_malloc(sizeof(CloudTrackItem) * kCloudSearchMaxResults));
    s_cloudHotPlaylists = static_cast<CloudPlaylistItem*>(ps_malloc(sizeof(CloudPlaylistItem) * kCloudHotPlaylistMax));
    s_cloudPlaylistTracks = static_cast<CloudTrackItem*>(ps_malloc(sizeof(CloudTrackItem) * kCloudPlaylistTrackMax));
    s_cloudRankings = static_cast<CloudRankingItem*>(ps_malloc(sizeof(CloudRankingItem) * kCloudRankingMax));
    s_cloudNewSongs = static_cast<CloudTrackItem*>(ps_malloc(sizeof(CloudTrackItem) * kCloudNewSongMax));
    s_cloudLyricLines = static_cast<CloudLyricLine*>(ps_malloc(sizeof(CloudLyricLine) * kCloudMaxLyricLines));
    if (!s_cloudSearchResults || !s_cloudHotPlaylists || !s_cloudPlaylistTracks || !s_cloudRankings || !s_cloudNewSongs || !s_cloudLyricLines) {
        MWR_LOG_ERROR("Failed to allocate cloud music result arrays (PSRAM)");
    }
    // This task runs the full TLS resolve HTTP request AND
    // audio.connecttohost() synchronously; 10K was the original value and
    // (with the real playback-crash fix being the deferred-navigation
    // change, not this stack) remains sufficient. Keeping it at 12K leaves
    // a little headroom while sparing internal heap for mbedTLS.
    if (xTaskCreatePinnedToCore(cloudMusicControllerTask, "cloudCtrl", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
        MWR_LOG_ERROR("Failed to create cloud music controller task");
    }
    // Keep both Render free-tier services awake (see cloudKeepaliveTask) --
    // cheap (one TLS pair every 9 min), prevents the double-cold-start
    // "加载失败/502" after idle.
    if (xTaskCreatePinnedToCore(cloudKeepaliveTask, "cloudKeep", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        MWR_LOG_ERROR("Failed to create cloud keepalive task");
    }
    // Lyrics fetch task starts unconditionally (not gated on WiFi connecting
    // like weatherTask) since local playback -- and playerCoreLoadLyrics()'s
    // mutex-guarded cache -- can happen with WiFi never up. fetchLyricsOnline()
    // itself checks WiFi.isConnected() and returns immediately if it's down.
    s_lyricsMutex = xSemaphoreCreateMutex();
    s_lyricsFetchQueue = xQueueCreate(1, sizeof(uint16_t));
    if (xTaskCreatePinnedToCore(lyricsFetchTask, "lyricsFetch", 8192, nullptr, 1, nullptr, 0) != pdPASS) MWR_LOG_ERROR("Failed to create lyrics fetch task");
    Audio::audio_info_callback = my_audio_info; // audio callback
    dlna.dlna_client_callbak(on_dlna_client);   // dlna callback
    bt_emitter.kcx_bt_emitter_callback(on_kcx_bt_emitter);
    webSrv.websrv_callbak(on_websrv);
    esp_log_level_set("*", ESP_LOG_DEBUG);
    esp_log_set_vprintf(log_redirect_handler);

    if (!get_esp_items(&s_resetReason, &s_f_FFatFound)) return;

    btn_RA_bt.set_active(false);
    btn_SE_bright.set_active(false);
#if MWR_LVGL_UI
    if (TFT_BL >= 0) {
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, HIGH);
        s_f_brightnessIsChangeable = false;
    }
#else
    if (TFT_BL >= 0) {
        s_f_brightnessIsChangeable = true;
        setupBacklight(TFT_BL, 512);
        setTFTbrightness(200, 200);
        btn_SE_bright.set_active(true);
    }
#endif
    if (IR_PIN >= 0) {
        pinMode(IR_PIN, INPUT_PULLUP); // if ir_pin is read only, have a external resistor (~10...40KOhm)
    }

    pref.begin("Pref", false); // instance of preferences from AccessPoint (SSID, PW ...)

    // Cloud-music config + history must load AFTER pref.begin() -- before it
    // the Preferences namespace isn't open and every getString() comes back
    // empty, which is exactly why the saved gateway info "disappeared" after
    // every flash/reboot (it was written to NVS fine, just never reloaded).
    cloudMusicLoadConfig();
    cloudMusicLoadHistory();

    if (!detect_i2_c_devices(&i2cBusOne, I2C_SDA, I2C_SCL, &s_i2c_items)) { printfln(s_tag.setup, "No i2c device found"); }

    if (s_i2c_items.bh1750_found) {
#if MWR_LVGL_UI
        printfln(s_tag.setup, "Ambient Light Sensor BH1750 ignored in LVGL display mode");
#else
        BH1750.begin(&i2cBusOne, s_i2c_items.bh1750_addr); // init the sensor
        printfln(s_tag.setup, "Ambient Light Sensor BH1750 found at " ANSI_ESC_CYAN "0x{:02X}", s_i2c_items.bh1750_addr);
        BH1750.setResolutionMode(BH1750.ONE_TIME_H_RESOLUTION_MODE);
        BH1750.setSensitivity(BH1750.SENSITIVITY_ADJ_MAX);
#endif
    }

    if (s_i2c_items.es8311_found) {
        bool res = es8311.begin(&i2cBusOne, s_i2c_items.es8311_addr); // init the dac
        if (res) printfln(s_tag.setup, "DAC ES8311 found at " ANSI_ESC_CYAN "0x{:02X}", s_i2c_items.es8311_addr);
        es8311.setVolume(90);
    }
    vTaskDelay(3000);

#if MWR_LVGL_UI
    if (!setupLvglRuntime()) {
        printfln(s_tag.setup, ANSI_ESC_RED "LVGL runtime setup failed");
        return;
    }
    return;
#endif

    set_tft_items(); // TFT, Resolotion
    set_tp_items();  // TP, Resolotion
    if (!init_SD_card()) return;

    defaultsettings();

    if (ESP.getFlashChipSize() > 80000000) { FFat.begin(); }

    drawImage("/common/MiniWebRadioV4.jpg", 0, 0); // Welcomescreen
    updateSettings();

    s_f_isWiFiConnected = connectToWiFi();

    placingGraphicObjects();
    sdr_BR_value.setValue(s_brightness);
    sdr_EQ_lowPass.setValue(s_tone.LP);
    sdr_EQ_bandPass.setValue(s_tone.BP);
    sdr_EQ_highPass.setValue(s_tone.HP);
    sdr_EQ_balance.setValue(s_tone.BAL);
    sdr_DL_volume.setMinMaxVal(0, s_volume.volumeSteps);
    sdr_DL_volume.setValue(s_volume.cur_volume);
    sdr_PL_volume.setMinMaxVal(0, s_volume.volumeSteps);
    sdr_PL_volume.setValue(s_volume.cur_volume);
    sdr_RA_volume.setMinMaxVal(0, s_volume.volumeSteps);
    sdr_RA_volume.setValue(s_volume.cur_volume);
    sdr_CL_volume.setMinMaxVal(0, s_volume.volumeSteps);
    sdr_CL_volume.setValue(s_volume.cur_volume);
    btn_RA_mute.setValue(s_f_mute);
    btn_CL_mute.setValue(s_f_mute);
    btn_EQ_mute.setValue(s_f_mute);
    btn_PL_mute.setValue(s_f_mute);
    btn_DL_mute.setValue(s_f_mute);
    btn_BT_power.setValue(s_bt_emitter.enabled);
    lst_DLNA.client_and_history(&dlna, &s_dlnaHistory[0], 10);
    lst_RADIO.currentStationNr(&s_cur_station);
    clk_AC_red.alarm_time_and_days(&s_alarmdays, s_alarmtime);

    audio.setAudioTaskCore(AUDIOTASK_CORE);
    audio.setConnectionTimeout(CONN_TIMEOUT, CONN_TIMEOUT_SSL);
    audio.setVolumeSteps(s_volume.volumeSteps);
    audio.setVolume(0);
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
    audio.setI2SCommFMT_LSB(I2S_COMM_FMT);
    playerService.begin();

    printfln(s_tag.setup, "number of saved stations: " ANSI_ESC_CYAN "{}", staMgnt.getSumStations());
    printfln(s_tag.setup, "number of saved favourites: " ANSI_ESC_CYAN "{}", staMgnt.getSumFavStations());
    printfln(s_tag.setup, "current station number: " ANSI_ESC_CYAN "{}", s_cur_station);
    printfln(s_tag.setup, "current volume: " ANSI_ESC_CYAN "{}", s_volume.cur_volume);
    printfln(s_tag.setup, "volume steps: " ANSI_ESC_CYAN "{}", s_volume.volumeSteps);
    printfln(s_tag.setup, "volume after alarm: " ANSI_ESC_CYAN "{}", s_volume.volumeAfterAlarm);
    printfln(s_tag.setup, "last connected host: " ANSI_ESC_YELLOW "{}", s_settings.lastconnectedhost);
    printfln(s_tag.setup, "connection timeout: " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " ms", CONN_TIMEOUT);
    printfln(s_tag.setup, "connection timeout SSL: " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " ms", CONN_TIMEOUT_SSL);

    if (s_volume.volumeSteps < 21) s_volume.volumeSteps = 21;

    if (IR_PIN >= 0) ir.begin(); // Init InfraredDecoder when fitted

    if (AMP_ENABLED >= 0) { // enable onboard amplifier
        pinMode(AMP_ENABLED, OUTPUT);
        digitalWrite(AMP_ENABLED, HIGH);
        printfln(s_tag.setup, "On Board Amplifier pin is: " ANSI_ESC_CYAN "{}", AMP_ENABLED);
    }

    if (s_f_isWiFiConnected) webSrv.begin(80, 81); // HTTP port, WebSocket port

    if (s_f_mute) { printfln(s_tag.setup, "volume is muted: (from " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET ")", s_volume.cur_volume); }
    setI2STone();
    playerCoreSetOutputPolicy(s_audioOutputPolicy, false);

    ticker100ms.attach(0.1, timer100ms);

    muteChanged(s_f_mute);
    if (s_f_isWiFiConnected) {
        if (s_resetReason == ESP_RST_POWERON ||   // Simply switch on the operating voltage
            s_resetReason == ESP_RST_SW ||        // ESP.restart()
            s_resetReason == ESP_RST_SDIO ||      // The boot button was pressed
            s_resetReason == ESP_RST_DEEPSLEEP) { // Wake up
            s_state = UNDEFINED;
        }
        if (!MDNS.begin("MiniWebRadio")) {
            printfln(s_tag.wifi_info, ANSI_ESC_YELLOW "Error starting mDNS");
        } else {
            printfln(s_tag.wifi_info, "mDNS started");
            MDNS.addService("esp32", "tcp", 80);
            printfln(s_tag.wifi_info, "mDNS name: " ANSI_ESC_YELLOW "MiniWebRadio");
        }
        ArduinoOTA.setHostname("MiniWebRadio");
        ArduinoOTA.begin();
        ftpSrv.begin(SD_MMC, FTP_USERNAME, FTP_PASSWORD); // username, password for fgetTP().
        s_f_dlnaSeekServer = true;
    } else {
        s_state = UNDEFINED;
        setTFTbrightness(200, 200);
        changeState(WIFI_SETTINGS, 0);
        return;
    }

    if (BT_EMITTER_RX >= 0) bt_emitter.begin();

    rec_buffer.alloc_array(REC_BUFFER_SIZE, "rec_buffer");                             // allocate in PSRAM
    writeBuffer.alloc_array(WRITE_CHUNK_SIZE, "writeBuffer");                          // allocate in PSRAM
    if (xTaskCreatePinnedToCore(wavWriterTask, "wavWriter", 4096, nullptr, 1, nullptr, 0) == pdPASS) { // start recorder task
        printfln(s_tag.setup, "Recorder task started, free heap: " ANSI_ESC_CYAN "{}", ESP.getFreeHeap());
    } else {
        MWR_LOG_ERROR("Failed to create recorder task");
    }

    drawImage("/common/Wallpaper.jpg", 0, 0);                                                                     // Wallpaper
    getTFT().copyFramebuffer(FB_VISIBLE, FB_BACKGROUND, 0, 0, displayConfig.dispWidth, displayConfig.dispHeight); // copy wallpaper to background

    dispHeader.updateVolume(s_volume.cur_volume);
    dispHeader.speakerOnOff(!s_f_mute);
    dispHeader.updateTime("00:00:00", true);

    dispFooter.setIpAddr(WiFi.localIP().toString().c_str());
    dispFooter.updateStation(s_cur_station);
    dispFooter.updateOffTime(s_sleeptime);

    setRTC(s_TZString);
    s_stationURL = s_settings.lastconnectedhost;
    setStation(s_cur_station);
    changeState(RADIO, 0);
}

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// 📌📌📌  C O M M O N  📌📌📌
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————

static bool i2c_read_reg(TwoWire* twi, uint8_t addr, uint8_t reg, uint8_t& value) {
    twi->beginTransmission(addr);
    twi->write(reg);
    if (twi->endTransmission(false) != 0) return false;
    if (twi->requestFrom((uint16_t)addr, (uint8_t)1, (uint8_t)true) != 1) return false;
    if (!twi->available()) return false;
    value = twi->read();
    return true;
}

static bool i2c_looks_like_es8311(TwoWire* twi, uint8_t addr) {
    uint8_t r00 = 0, r01 = 0, r02 = 0, r03 = 0;
    if (!i2c_read_reg(twi, addr, 0x00, r00)) return false;
    log_w("r00 %i", r00);
    if (!i2c_read_reg(twi, addr, 0x01, r01)) return false;
    log_w("r01 %i", r01);
    if (!i2c_read_reg(twi, addr, 0x02, r02)) return false;
    log_w("r02 %i", r02);
    if (!i2c_read_reg(twi, addr, 0x03, r03)) return false;
    log_w("r03 %i", r03);
    bool res = (0x1F == r00 && 0x00 == r01 && 0xF0 == r02 && 0x10 == r03);
    log_w("res %i", res);
    return res;
}

bool detect_i2_c_devices(TwoWire* twi, int8_t sda, int8_t scl, i2c_items_s* i2c_items) {
    if (sda < 0) return false;
    if (scl < 0) return false;
    if (sda == scl) return false;
    bool log = 0;
    twi->end();
    twi->flush();
    twi->begin(sda, scl, 100000);

    for (uint8_t addr = 0; addr < 128; addr++) {
        twi->beginTransmission(addr);
        if (twi->endTransmission() == 0) {
            if (addr == 0x18 || addr == 0x19) {
                if (i2c_looks_like_es8311(twi, addr)) {
                    i2c_items->es8311_found = true;
                    i2c_items->es8311_addr = addr;
                    if (log) MWR_LOG_WARN("es8311 found at 0x{:X}", addr);
                } else {
                    MWR_LOG_WARN("unknown i2c device at 0x{:X} found", addr);
                }
            } else if (addr == 0x14 || addr == 0x5D) {
                i2c_items->gt911_found = true;
                i2c_items->gt911_addr = addr;
                if (log) MWR_LOG_WARN("gt911 found at 0x{:X}", addr);
                //-- BH1750 -------------------------------------------------------------------------------------------------------------------------------
            } else if (addr == 0x23 || addr == 0x5C) {
                i2c_items->bh1750_found = true;
                i2c_items->bh1750_addr = addr;
                //-- FT8X36U ------------------------------------------------------------------------------------------------------------------------------
            } else if (addr == 0x38) {
                i2c_items->ft6x36u_found = true;
                i2c_items->ft6x36u_addr = addr;
                if (log) MWR_LOG_WARN("ft6x36u found at 0x{X}", addr);
            } else if (addr == 0x40) {
                i2c_items->es7210_found = true;
                i2c_items->es7210_addr = addr;
                if (log) MWR_LOG_WARN("es7210 found at 0x{X}", addr);
            } else {
                MWR_LOG_WARN("unknown i2c device at 0x{X} found", addr);
            }
        }
    }
    return true;
}
//---------------------------------------------------------------------------------------

void set_tft_items() {
//---- LAYOUT -----------
#ifdef TFT_LAYOUT_WAVESHARE_19
    s_h_resolution = 320;
    s_v_resolution = 170;
#elifdef TFT_LAYOUT_S
    s_h_resolution = 320;
    s_v_resolution = 240;
#elifdef TFT_LAYOUT_M
    s_h_resolution = 480;
    s_v_resolution = 320;
#elifdef TFT_LAYOUT_L
    s_h_resolution = 800;
    s_v_resolution = 480;
#elifdef TFT_LAYOUT_XL
    s_h_resolution = 1024;
    s_v_resolution = 600;
#endif

//---- TFT_MODE ---------
#ifdef TFT_MODE_SPI
#ifndef TFT_LAYOUT_WAVESHARE_19
    spiBus.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1); // SPI1 for TFT
#endif
    getTFT().setTFTcontroller(TFT_CONTROLLER);
    getTFT().setRotation(TFT_ROTATION);
    getTFT().setDisplayInversion(DISPLAY_INVERSION);
    getTFT().begin(TFT_DC); // Init TFT interface
    getTFT().setFrequency(TFT_FREQUENCY);
    getTFT().setBackGoundColor(TFT_BLACK);
#elifdef TFT_MODE_RGB
    getTFT().begin(RGB_PINS, RGB_TIMING);
    getTFT().setDisplayInversion(false);
    getTFT().setRotation(TFT_ROTATION);
    vTaskDelay(100 / portTICK_PERIOD_MS); // wait for TFT to be ready
    getTFT().reset();
    vTaskDelay(100 / portTICK_PERIOD_MS); // wait for TFT to be ready
    getTFT().clearVsyncCounter();         // clear the vsync counter and start them
#elifdef TFT_MODE_DSI
    getTFT().begin(DSI_TIMING);
    getTFT().setRotation(TFT_ROTATION);
    getTFT().setDisplayInversion(DISPLAY_INVERSION);
    vTaskDelay(100 / portTICK_PERIOD_MS); // wait for TFT to be ready
#endif
}

void set_tp_items() {
//---- TP_MODE ---------
#ifdef TP_MODE_XPT2046 // XPT2046
    getTP().begin(TP_IRQ, s_h_resolution, s_v_resolution);
    getTP().setSize(TP_CONTROLLER); // (0) 2.8 inch, (1) 3.5 inch, (4) 4.0 inch
    getTP().setRotation(TP_ROTATION);
#elifdef TP_MODE_GT911  // GT911
    getTP().begin(&i2cBusOne, s_i2c_items.gt911_addr, s_h_resolution, s_v_resolution);
    getTP().setRotation(TP_ROTATION);
    getTP().setMirror(TP_H_MIRROR, TP_V_MIRROR);
#elifdef TP_MODE_FT6X63 // FT6x36
    getTP().begin(&i2cBusOne, 0x38, s_h_resolution, s_v_resolution);
    getTP().get_FT6x36_items();
    getTP().setRotation(TP_ROTATION);
    getTP().setMirror(TP_H_MIRROR, TP_V_MIRROR);
#elifdef TP_MODE_CST816
    getTP().begin(&i2cBusOne, 0x15, s_h_resolution, s_v_resolution);
    getTP().setRotation(TP_ROTATION);
    getTP().setMirror(TP_H_MIRROR, TP_V_MIRROR);
#endif
}

//---------------------------------------------------------------------------------------
bool init_SD_card() {
    printfln(s_tag.sd_card, "Init SD card");
    pinMode(SD_MMC_D0, INPUT_PULLUP);
    int32_t sdmmc_frequency = SDMMC_FREQUENCY / 1000; // MHz -> KHz, default is 40MHz
#ifdef CONFIG_IDF_TARGET_ESP32S3
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    s_f_sd_card_found = SD_MMC.begin("/sdcard", true, false, sdmmc_frequency);
#elifdef CONFIG_IDF_TARGET_ESP32P4
    if (SD_MMC_D1 == -1) {
        SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
        s_f_sd_card_found = SD_MMC.begin("/sdcard", true, false, sdmmc_frequency); // mode1bit
    } else {
        SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
        s_f_sd_card_found = SD_MMC.begin("/sdcard", false, false, sdmmc_frequency); // mode4bit
    }

#endif
    if (!s_f_sd_card_found) {
        clearAll(TFT_BLACK);
        getTFT().setFontSize(displayConfig.fonts[6]);
        getTFT().setTextColor(TFT_YELLOW);
        getTFT().writeText("SD Card Mount Failed", 0, 50, displayConfig.dispWidth, displayConfig.dispHeight, TFT_ALIGN_CENTER, TFT_ALIGN_TOP, false, false);
        printfln(s_tag.sd_card, ANSI_ESC_RED "SD Card Mount Failed");
        return false;
    }
    float cardSize = ((float)SD_MMC.cardSize()) / (1024 * 1024);
    float freeSize = ((float)SD_MMC.cardSize() - SD_MMC.usedBytes()) / (1024 * 1024);
    printfln(s_tag.sd_card, "SD card found, " ANSI_ESC_CYAN "{:.1}" ANSI_ESC_RESET " MB by " ANSI_ESC_CYAN "{:.1} MB free", freeSize, cardSize);
    return true;
}

//---------------------------------------------------------------------------------------

ps_ptr<char> scaleImage(ps_ptr<char> path) {
    if (path.strlen() == 0) return "/common/unknown.png";
    bool ok = false;
    if (path.ends_with("bmp")) ok = true;
    if (path.ends_with("jpg")) ok = true;
    if (path.ends_with("gif")) ok = true;
    if (path.ends_with("png")) ok = true;
    if (path.starts_with("/png")) ok = false; // is web button
    MWR_LOG_DEBUG("path {}", path.c_get());
    if (!ok) return path;

    int idx = path.index_of('/', 1);
    if (idx < 0) return path; // invalid path
    ps_ptr<char> tfts = displayConfig.tftSize;
    tfts += "/";
    path.insert(tfts.c_get(), idx + 1); // "/logo/0N 90s.jpg" --> "/logo/s/0N 90s.jpg"
    MWR_LOG_DEBUG("path {}", path.c_get());
    return path;
}

void setVolume(uint8_t vol) {
    static int16_t oldVol = -1;
    if (vol == oldVol) return;
    MWR_LOG_DEBUG("volume old: {}. new: {}", oldVol, vol);
    s_volume.cur_volume = vol;
    oldVol = vol;
    dispHeader.updateVolume(s_volume.cur_volume);
    sdr_CL_volume.setValue(s_volume.cur_volume);
    sdr_DL_volume.setValue(s_volume.cur_volume);
    sdr_PL_volume.setValue(s_volume.cur_volume);
    sdr_RA_volume.setValue(s_volume.cur_volume);
    printfln(s_tag.action, "current volume is " ANSI_ESC_CYAN "{}", s_volume.cur_volume);
}

uint8_t downvolume() {
    uint8_t steps = s_volume.volumeSteps / 20;
    if (s_volume.cur_volume == 0)
        return s_volume.cur_volume;
    else if (steps < s_volume.cur_volume)
        s_volume.cur_volume -= steps;
    else
        s_volume.cur_volume--;
    sdr_CL_volume.setValue(s_volume.cur_volume);
    sdr_DL_volume.setValue(s_volume.cur_volume);
    sdr_PL_volume.setValue(s_volume.cur_volume);
    sdr_RA_volume.setValue(s_volume.cur_volume);
    s_f_mute = false;
    muteChanged(s_f_mute); // set mute off
    return s_volume.cur_volume;
}

uint8_t upvolume() {
    uint8_t steps = s_volume.volumeSteps / 20;
    if (s_volume.cur_volume == s_volume.volumeSteps)
        return s_volume.cur_volume;
    else if (s_volume.volumeSteps > s_volume.cur_volume + steps)
        s_volume.cur_volume += steps;
    else
        s_volume.cur_volume++;
    sdr_CL_volume.setValue(s_volume.cur_volume);
    sdr_DL_volume.setValue(s_volume.cur_volume);
    sdr_PL_volume.setValue(s_volume.cur_volume);
    sdr_RA_volume.setValue(s_volume.cur_volume);
    s_f_mute = false;
    muteChanged(s_f_mute); // set mute off
    return s_volume.cur_volume;
}

void setStation(ps_ptr<char> url, ps_ptr<char> extension) {
    if (!url.valid()) {
        MWR_LOG_ERROR("url is empty");
        return;
    }
    // e.g.  http://lstn.lv/bbcradio.m3u8?station=bbc_radio_one&bitrate=96000
    // url is http://lstn.lv/bbcradio.m3u8?station=bbc_radio_one, extension is bitrate=96000
    s_stationName_air = url;
    if (extension.strlen()) url.appendf("&{}", extension);
    s_stationURL = url;
    s_cur_station = 0;
    setStation(0);
}

void setStation(uint16_t sta) {
    if (sta == 0) {
        connecttohost(s_stationURL);
        s_stationName_air = s_stationURL;
    } else {
        if (sta > staMgnt.getSumStations()) sta = s_cur_station;
        s_stationURL = staMgnt.getStationUrl(sta);
        connecttohost(s_stationURL);
    }
    printfln(s_tag.action, "switch to station " ANSI_ESC_CYAN "{}", sta);
    printfln(s_tag.country, "Country of origin " ANSI_ESC_YELLOW "{}", staMgnt.getStationCountry(s_cur_station));
    s_homepage = "";
    s_streamTitle = "";
    s_icyDescription = "";
    clearStreamTitle();
    s_f_newStreamTitle = true;
    s_f_newIcyDescription = true;
    showLogoAndStationName();
}

void nextStation() {
    setStation(staMgnt.nextStation());
}
void nextFavStation() {
    setStation(staMgnt.nextFavStation());
}
void prevStation() {
    setStation(staMgnt.prevStation());
}
void prevFavStation() {
    setStation(staMgnt.prevFavStation());
}
void setStationByNumber(uint16_t staNr) {
    setStation(staMgnt.setStationByNumber(staNr));
}

void savefile(ps_ptr<char> fileName, uint32_t contentLength, ps_ptr<char> contentType) { // save the uploadfile on SD_MMC

    if (!fileName.starts_with("/")) { fileName = "/" + fileName; }
    if (webSrv.uploadfile(SD_MMC, fileName, contentLength, contentType)) {
        printfln(s_tag.sd_card, "save file " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " in progress", fileName.c_get());
        webSrv.sendStatus(200);
    } else {
        printfln(s_tag.sd_card, "save file " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " to SD failed", fileName.c_get());
        webSrv.sendStatus(400);
    }
}

void saveImage(const char* fileName, uint32_t contentLength) { // save the jpg image on SD_MMC
    ps_ptr<char> fn;

    if (endsWith(fileName, "jpg")) {
        fn.assign("/logo/");
        fn.append(displayConfig.tftSize);
        if (!startsWith(fileName, "/")) fn.append("/");
        fn.append(fileName);
        if (webSrv.uploadB64image(SD_MMC, fn.c_get(), contentLength)) {
            printfln(s_tag.sd_card, "save image (jpg) " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET " to SD card was successfully", fn.c_get());
            webSrv.sendStatus(200);
        } else
            webSrv.sendStatus(400);
    }
}

void setI2STone() {
    audio.setTone(s_tone.LP, s_tone.BP, s_tone.HP);
    audio.setBalance(s_tone.BAL);
    return;
}

ps_ptr<char> getI2STone() {
    ps_ptr<char> tone;
    tone.assignf("LowPass={}\nBandPass={}\nHighPass={}\nBalance={}\n", s_tone.LP, s_tone.BP, s_tone.HP, s_tone.BAL);
    return tone;
}

void SD_playFile(ps_ptr<char> pathWoFileName, const char* fileName) { // pathWithoutFileName e.g. /audiofiles/playlist/
    pathWoFileName += fileName;
    int32_t idx = pathWoFileName.index_of("\033[", 1);
    if (idx == -1) { // do nothing
        SD_playFile(pathWoFileName, 0, true);
        return;
    }
    SD_playFile(pathWoFileName.substr(0, idx), 0, true); // remove color and filesize
    return;
}

void SD_playFile(ps_ptr<char> path, uint32_t fileStartTime, bool showFN) {
    if (!path.valid()) return; // avoid a possible crash

    if (path.ends_with("m3u")) {
        if (playlist.create_playlist_from_file(path)) s_f_playlistEnabled = true;
        return;
    }

    ps_ptr<char> file_name;

    if (s_subState_player != 1) { changeState(PLAYER, 1); }
    int32_t idx = path.last_index_of('/');
    if (idx < 0) return;
    s_cur_AudioFolder = path.substr(0, idx);
    file_name = path.substr(idx + 1); // without '/'

    if (showFN) {
        clearLogo();
        showPlayerFileName(path.get() + idx + 1);
    }

    printfln(s_tag.file_name, ANSI_ESC_MAGENTA "{}", file_name.c_get());
    connecttoFS("SD_MMC", (const char*)path.c_get(), fileStartTime);
    if (s_f_playlistEnabled) showPlsFileNumber();
    if (s_f_isFSConnected) { s_settings.lastconnectedfile = path; }
}

bool SD_rename(const char* src, const char* dest) {
    bool success = false;
    if (SD_MMC.exists(src)) { success = SD_MMC.rename(src, dest); }
    return success;
}

bool SD_newFolder(const char* folderPathName) {
    bool success = false;
    success = SD_MMC.mkdir(folderPathName);
    return success;
}

bool SD_delete(const char* itemPath) {
    bool success = false;
    if (SD_MMC.exists(itemPath)) {
        File dirTest = SD_MMC.open(itemPath, "r");
        bool isDir = dirTest.isDirectory();
        dirTest.close();
        if (isDir)
            success = SD_MMC.rmdir(itemPath);
        else
            success = SD_MMC.remove(itemPath);
    }
    return success;
}

void fall_asleep() {
    s_f_sleeping = true;
    muteChanged(true);
    s_f_playlistEnabled = false;
    s_f_isFSConnected = false;
    s_f_isWebConnected = false;
    audio.stopSong();
    if (s_sleepMode == 0) {
        changeState(SLEEP, 0);
    } else {
        changeState(SLEEP, 1);
    }
    if (s_bt_emitter.found) bt_emitter.power_off();
    printfln(s_tag.action, "falling asleep");
}

void wake_up(int8_t state, int8_t subState) {
    s_f_sleeping = false;
    if (s_bt_emitter.found && s_bt_emitter.enabled) bt_emitter.power_on(s_bt_emitter.mode);
    muteChanged(false);
    printfln(s_tag.action, "awake");
    clearAll(TFT_TRANSPARENT);
    clk_CL_24.hide();
    setTFTbrightness(s_brightness, s_bh1750Value);
    dispHeader.set_bg_color(TFT_TRANSPARENT);
    dispFooter.set_bg_color(TFT_TRANSPARENT);
    dispHeader.show();
    dispFooter.show();
    changeState(state, subState);
}

void setRTC(ps_ptr<char> TZString) {
    rtc.stop();
    rtc.begin(TZString.c_get());
}

boolean isAlarm(uint8_t weekDay, uint8_t alarmDays, uint16_t minuteOfTheDay, int16_t* alarmTime) {
    uint8_t mask = 0b00000001 << weekDay;
    if (alarmDays & mask) {                         // yes, is alarmDay
        if (alarmTime[weekDay] == minuteOfTheDay) { // yes, is alarmTime
            return true;
        }
    }
    return false;
}

boolean copySDtoFFat(const char* path) {
    if (!s_f_FFatFound) return false;
    uint8_t buffer[1024];
    size_t  r = 0, w = 0;
    size_t  len = 0;
    File    file1 = SD_MMC.open(path, "r");
    File    file2 = FFat.open(path, "w");
    while (true) {
        r = file1.read(buffer, 1024);
        w = file2.write(buffer, r);
        if (r != w) {
            file1.close();
            file2.close();
            FFat.remove(path);
            return false;
        }
        len += r;
        if (r == 0) break;
    }
    MWR_LOG_DEBUG("file length {}, written {}", file1.size(), len);
    if (file1.size() == len) return true;
    return false;
}

void muteChanged(bool m) {
    s_f_muteIsPressed = false;
    btn_CL_mute.setValue(m);
    btn_DL_mute.setValue(m);
    btn_EQ_mute.setValue(m);
    btn_PL_mute.setValue(m);
    btn_RA_mute.setValue(m);
    if (m) {
        s_f_mute = true;
        if (AMP_ENABLED != -1) {
            digitalWrite(AMP_ENABLED, LOW);
            printfln(s_tag.action, "mute, On Board Amplifier is off");
        }
        webSrv.send("mute=", "1");
    } else {
        s_f_mute = false;
        if (AMP_ENABLED != -1) {
            digitalWrite(AMP_ENABLED, HIGH);
            printfln(s_tag.action, "unmute, On Board Amplifier is on");
        }
        webSrv.send("mute=", "0");
    }
    dispHeader.speakerOnOff(!s_f_mute);
    dispHeader.updateVolume(s_volume.cur_volume);
    updateSettings();
};

void logAlarmItems() {
    const char wd[7][11] = {"Sunday:   ", "Monday:   ", "Tuesday:  ", "Wednesday:", "Thursday: ", "Friday:   ", "Saturday: "};
    uint8_t    mask = 0b00000001;
    for (uint8_t i = 0; i < 7; i++) {
        if (s_alarmdays & mask) {
            printfln(s_tag.alarm_time, ANSI_ESC_YELLOW "{} " ANSI_ESC_CYAN "{:02}:{:02}", wd[i], s_alarmtime[i] / 60, s_alarmtime[i] % 60);
        } else {
            printfln(s_tag.alarm_time, ANSI_ESC_YELLOW "{} No alarm is set", wd[i]);
        }
        mask <<= 1;
    }
}

void setTimeCounter(uint8_t sec) {
    if (sec) {
        s_timeCounter.timer = 10;
        s_timeCounter.factor = sec;
        s_timeCounter.tmp = sec;
    } else {
        s_timeCounter.timer = 0;
        s_timeCounter.factor = 0;
        dispFooter.updateTC(0);
        s_f_newBitRate = true;
    }
}

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// 📌📌📌  C H A N G E   S T A T E  📌📌📌
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// clang-format off
/*🟢🟡🔴*/
void changeState(int8_t state, int8_t subState) {

    printfln(s_tag.action, "required state " ANSI_ESC_GREEN "{}({})" ANSI_ESC_RESET ", current state is " ANSI_ESC_GREEN "{}({})", getStatusName(state), subState, getStatusName(s_state), s_subState);
    MWR_LOG_DEBUG("state {}, s_state {}, subState {}, s_subState_radio {}, s_subState_player {}", state, s_state, subState, s_subState_radio, s_subState_player);
    bool newState = false;
    bool newSubState = false;
    s_subState = subState;
   // disableAllObjects();
    setTimeCounter(0);
    if (state == RADIO          && s_state != RADIO)              { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == STATIONSLIST   && s_state != STATIONSLIST)       { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == PLAYER         && s_state != PLAYER)             { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == AUDIOFILESLIST && s_state != AUDIOFILESLIST)     { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == DLNA           && s_state != DLNA)               { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == DLNAITEMSLIST  && s_state != DLNAITEMSLIST)      { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == CLOCK          && s_state != CLOCK)              { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == ALARMCLOCK     && s_state != ALARMCLOCK)         { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == SLEEPTIMER     && s_state != SLEEPTIMER)         { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == SETTINGS       && s_state != SETTINGS)           { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == BRIGHTNESS     && s_state != BRIGHTNESS)         { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == EQUALIZER      && s_state != EQUALIZER)          { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == BLUETOOTH      && s_state != BLUETOOTH)          { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == IR_SETTINGS    && s_state != IR_SETTINGS)        { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == RINGING        && s_state != RINGING)            { dispHeader.set_bg_color(TFT_BLACK);       dispHeader.show(); dispFooter.set_bg_color(TFT_BLACK);       dispFooter.show(); clearWithOutHeaderFooter(TFT_BLACK);       newState = true;}
    if (state == WIFI_SETTINGS  && s_state != WIFI_SETTINGS)      { dispHeader.set_bg_color(TFT_TRANSPARENT); dispHeader.show(); dispFooter.set_bg_color(TFT_TRANSPARENT); dispFooter.show(); clearWithOutHeaderFooter(TFT_TRANSPARENT); newState = true;}
    if (state == SLEEP          && s_state != SLEEP)              { dispHeader.set_bg_color(TFT_BLACK);       dispFooter.set_bg_color(TFT_BLACK);        clearAll(TFT_BLACK);                                                            newState = true;}

    if (state == RADIO          && s_subState_radio  != subState) { newSubState = true;  }
    if (state == PLAYER         && s_subState_player != subState) { newSubState = true;  }
    if (state == CLOCK          && s_subState_clock  != subState) { newSubState = true;  }

    if(newState)disableAllObjects();
    dispHeader.enable();
    dispFooter.enable();
    if(state != s_state) { dispHeader.updateItem(_hl_item[state]); }

    s_subState_radio  = UNDEFINED;
    s_subState_player = UNDEFINED;
    s_subState_clock  = UNDEFINED;

    s_f_volBarVisible = false;
    if (state != RADIO) { dispFooter.updateFlag(""); }

    switch (state) {
        case RADIO: {
            if (newState) {
                txt_RA_staName.setText("");
                txt_RA_staName.show();
                webSrv.send("changeState=", "RADIO");
                if(!s_f_isWebConnected){
                    setStation(s_cur_station);
                }
                if(s_f_isWebConnected) showLogoAndStationName();
            }
            else{
                txt_RA_staName.enable();
            }

            pic_RA_logo.enable();
            if(newSubState) hide_objects_in_area(layout.winArea2.x, layout.winArea2.y, layout.winArea2.w, layout.winArea2.h);
            if (subState == 0) {
                if(newSubState) {
                    VUmeter_RA.show();
                    txt_RA_sTitle.setText("");
                    txt_RA_sTitle.show();
                    s_f_newIcyDescription = true;
                    s_f_newStreamTitle = true;
                }
                else {
                    VUmeter_RA.enable();
                    txt_RA_sTitle.enable();
                    txt_RA_sTitle.enable();
                }
                setTimeCounter(0);
            }
            if (subState == 1) {  // Mute, Vol+, Vol-, Sta+, Sta-, StaList
                if(newSubState) {
                    txt_RA_sTitle.hide();
                    VUmeter_RA.hide();
                    sdr_RA_volume.show();
                    btn_RA_mute.show(); btn_RA_prevSta.show(); btn_RA_nextSta.show(); btn_RA_recorder.show();
                    setTimeCounter(2);
                }
                else{
                    sdr_RA_volume.enable();
                    btn_RA_mute.enable(); btn_RA_prevSta.enable(); btn_RA_nextSta.enable(); btn_RA_recorder.enable();
                }
            }
            if (subState == 2){ // Player, DLNA, Clock, SleepTime, Brightness, EQ, BT, Off
                if(newSubState) {
                    txt_RA_sTitle.hide();
                    VUmeter_RA.hide();
                    sdr_RA_volume.hide();
                    btn_RA_staList.show();
                    btn_RA_player.show(); btn_RA_dlna.show(); btn_RA_clock.show(); btn_RA_sleep.show(); btn_RA_settings.show();
                    btn_RA_bt.show();
                    btn_RA_off.show();
                    setTimeCounter(2);
                }
                else {
                    btn_RA_staList.enable();
                    btn_RA_player.enable(); btn_RA_dlna.enable(); btn_RA_clock.enable(); btn_RA_sleep.enable(); btn_RA_settings.enable();
                    btn_RA_bt.enable();
                    btn_RA_off.enable();
                }
            }
            s_subState_radio = subState;
            break;
        }

        case STATIONSLIST: {
            lst_RADIO.show();
            setTimeCounter(LIST_TIMER);
            break;
        }
        case PLAYER: {
            if (newState) {
                stopSong();
                webSrv.send("changeState=", "PLAYER");
            }
            pic_PL_logo.enable();
            if (subState == 0){
                s_SD_content.listFilesInDir(s_cur_AudioFolder.c_get(), true, false);
                s_cur_Codec = 0;
                showFileLogo(PLAYER, subState);
                showPlayerFileName(s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr));
                txt_PL_fName.show();
                pgb_PL_progress.hide();
                sdr_PL_volume.hide();
                showAudioFileNumber();
                btn_PL_prevFile.show(); btn_PL_nextFile.show(); btn_PL_ready.show(); btn_PL_playAll.show();
                btn_PL_shuffle.show();  btn_PL_fileList.show(); btn_PL_radio.show(); btn_PL_off.show();
            }
            if (subState == 1){
                pgb_PL_progress.setValue(0);
                showPlayerFileName(s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr));
                if(newSubState){
                    btn_PL_fileList.hide(); btn_PL_radio.hide(); btn_PL_off.hide();
                    pgb_PL_progress.show();
                    sdr_PL_volume.show();
                    txt_PL_fName.show();
                    btn_PL_mute.show(); btn_PL_pause.setOff(); btn_PL_pause.show(); btn_PL_cancel.show(); btn_PL_playPrev.show(); btn_PL_playNext.show();
                }
                else{
                    pgb_PL_progress.enable();
                    sdr_PL_volume.enable();
                    txt_PL_fName.enable();
                    btn_PL_mute.enable(); btn_PL_pause.enable(); btn_PL_pause.enable(); btn_PL_cancel.enable();  btn_PL_playPrev.enable(); btn_PL_playNext.enable();
                }
            }
            s_subState_player = subState;
            break;
        }
        case AUDIOFILESLIST: {
            lst_PLAYER.show(s_cur_AudioFolder, s_cur_AudioFileNr);
            setTimeCounter(LIST_TIMER);
            break;
        }
        case DLNA: {
            if (newState && s_state != DLNAITEMSLIST) audio.stopSong();
            pic_DL_logo.enable();
            pgb_DL_progress.setValue(0);
            pgb_DL_progress.show();
            txt_DL_fName.show();
            showFileLogo(DLNA, subState);
            webSrv.send("changeState=", "DLNA");
            if (audio.isRunning()) btn_DL_pause.setActive(true);
            else                   btn_DL_pause.setActive(false);
            sdr_DL_volume.show();
            btn_DL_pause.show(); btn_DL_mute.show(); btn_DL_cancel.show(); btn_DL_fileList.show(); btn_DL_radio.show();
            break;
        }
        case DLNAITEMSLIST: {
            lst_DLNA.show(s_currDLNAsrvNr, dlna.getServer(), dlna.getBrowseResult(), &s_dlnaLevel, s_dlnaMaxItems, s_dlnaMaXServers);
            setTimeCounter(LIST_TIMER);
            break;
        }
        case CLOCK: {
            clk_CL_24.show();
            if (subState == 0) {
                btn_CL_mute.hide(); btn_CL_alarm.hide(); btn_CL_radio.hide(); sdr_CL_volume.hide(); btn_CL_off.hide();
            }
            if (subState == 1) {
                setTimeCounter(2);
                sdr_CL_volume.show();
                btn_CL_mute.show(); btn_CL_alarm.show(); btn_CL_radio.show(); btn_CL_off.show();
            }

            s_subState_clock = subState;
            break;
        }
        case ALARMCLOCK: {
            btn_AC_left.show(); btn_AC_right.show(); btn_AC_up.show(); btn_AC_down.show(); btn_AC_ready.show(); clk_AC_red.show();
            break;
        }
        case SLEEPTIMER: {
            if (newState) {
                otb_SL_stime.show(s_sleeptime);
                pic_SL_logo.setPicturePath("/common/Night_Gown.jpg");
                pic_SL_logo.align(true, true);
                pic_SL_logo.show();
            }
            btn_SL_up.show(); btn_SL_up.show(); btn_SL_down.show(); btn_SL_ready.show(); btn_SL_cancel.show();
            break;
        }
        case SETTINGS: {
            if (newState) {
                showFileLogo(SETTINGS, subState);
            }
            btn_SE_bright.show(); btn_SE_equal.show(); btn_SE_wifi.show(); btn_SE_radio.show();
            break;
        }
        case BRIGHTNESS: {
            if (newState) {
                pic_BR_logo.show();
                sdr_BR_value.setValue(s_brightness);
                sdr_BR_value.show();
                txt_BR_value.setText(int2str(s_brightness));
                txt_BR_value.show();
            } else {
                sdr_BR_value.enable();
                txt_BR_value.enable();
            }
            btn_BR_ready.show();
            break;
        }
        case EQUALIZER:
            sdr_EQ_lowPass.show();
            sdr_EQ_bandPass.show();
            sdr_EQ_highPass.show();
            sdr_EQ_balance.show();
            btn_EQ_lowPass.show();
            btn_EQ_bandPass.show();
            btn_EQ_highPass.show();
            btn_EQ_balance.show();
            btn_EQ_Player.show();
            btn_EQ_mute.show();
            txt_EQ_lowPass.show();
            txt_EQ_bandPass.show();
            txt_EQ_highPass.show();
            txt_EQ_balance.show();
            btn_EQ_Radio.show();
            break;

        case BLUETOOTH: {
            btn_BT_volUp.show(); btn_BT_volDown.show(); btn_BT_pause.show(); btn_BT_mode.show();
            btn_BT_radio.show(); btn_BT_power.show();
            pic_BT_mode.show();
            txt_BT_mode.set_bg_color(TFT_BROWN);
            if (s_bt_emitter.mode.equals("RX")) { txt_BT_mode.setText("RECEIVER"); }
            else                                { txt_BT_mode.setText("EMITTER"); }
            txt_BT_mode.show();
            ps_ptr<char> v;
            v.assignf("Vol: {:02}", bt_emitter.getVolume());
            dispFooter.updateFileNr(v);
            if (s_state != BLUETOOTH) webSrv.send("changeState=", "BLUETOOTH");
            break;
        }
        case IR_SETTINGS:
            btn_IR_radio.show();
            break;
        case RINGING:
            if (s_volume.ringVolume > 0) { // alarm with bell
                pic_RI_logo.enable();
                showFileLogo(RINGING, subState);
                setTFTbrightness(s_brightness, s_bh1750Value);
                printfln(s_tag.action, ANSI_ESC_MAGENTA "Alarm");
                setVolume(s_volume.ringVolume);
                audio.setVolume(s_volume.ringVolume);
                muteChanged(false);
                connecttoFS("SD_MMC", "/ring/alarm_clock.mp3");
                clk_RI_24small.set_bg_color(TFT_BLACK);
                clk_RI_24small.show();
            } else { // alarm without bell
                s_f_eof_alarm = true;
            }
            break;

        case WIFI_SETTINGS:
            cls_wifiSettings.clearText();
            cls_wifiSettings.setFontSize(displayConfig.listFontSize);
            {
                if (lockWifiOps(pdMS_TO_TICKS(15000))) {
                    int16_t n = WiFi.scanNetworks();
                    printfln(s_tag.wifi_info, ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " WiFi networks found", n);
                    if(n <= 0) {
                        unlockWifiOps();
                        break;
                    }
                    for (int i = 0; i < n; i++) {
                        printfln(s_tag.wifi_info, ANSI_ESC_GREEN"{} ({})", WiFi.SSID(i).c_str(), (int16_t)WiFi.RSSI(i));
                        ps_ptr<char> pw = get_WiFi_PW(WiFi.SSID(i).c_str());
                        cls_wifiSettings.add_WiFi_Items(WiFi.SSID(i).c_str(), pw.c_get());
                    }
                    WiFi.scanDelete();
                    unlockWifiOps();
                } else {
                    MWR_LOG_WARN("WiFi operation busy while opening WiFi settings");
                }
            }
            cls_wifiSettings.show();
            break;

        case SLEEP:
            dispHeader.hide();
            dispFooter.hide();
            if (subState == 0) {
                setTFTbrightness(s_brightness, s_bh1750Value);
            }
            if (subState == 1) {
                clk_CL_24.show();
            }
        break;
    }
    s_ir_btn_select = UNDEFINED;
    s_state = state;
}
// clang-format on
/*🟢🟡🔴*/
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————

ps_ptr<char> get_WiFi_PW(const char* ssid) {
    ps_ptr<char> line;
    ps_ptr<char> password = "";

    for (int j = 0; j < kWifiSlotCount; j++) {
        line = wifiPrefGet(j);
        if (line.starts_with(ssid) && line[strlen(ssid)] == '\t') {
            int idx = line.index_of("\t", 0);
            password = line.substr(idx + 1);
        }
    }
    return password;
}

/*         ╔═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
           ║                                                                                    L O O P                                                                                  ║
           ╚═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝   */

void loop() {
    vTaskDelay(1);
#if MWR_LVGL_UI
    loopLvglRuntime();
    return;
#endif
    processUsbStorage();
    dlna.loop();
    audio.loop();
    if (!usbStorageBlocksSdAppAccess()) {
        webSrv.loop();
        ftpSrv.handleFTP();
    }
    if (IR_PIN >= 0) ir.loop();
    getTP().loop();
    ArduinoOTA.handle();
    bt_emitter.loop();
    getTFT().loop();
    BH1750.loop();
    playerService.tick();

    while (s_logBuffer.size() > 0) {
        size_t i = s_logBuffer.size();
        if (s_logBuffer[i - 1].strlen() > 0 && s_logBuffer[i - 1].strlen() < 1024) {
            webSrv.send("serTerminal=", s_logBuffer[i - 1].c_get());
        } else
            log_w("%s %i: strlen %i", __FILE__, __LINE__, s_logBuffer[i - 1].strlen());
        s_logBuffer.pop_back();
        if (s_logBuffer.size() == 0) s_logBuffer.clear(); // Löscht alle Elemente und gibt den Speicher frei
    }

    if (s_f_dlnaBrowseServer) {
        s_f_dlnaBrowseServer = false;
        dlna.browseServer(s_currDLNAsrvNr, s_dlnaHistory[s_dlnaLevel].objId.c_get(), s_totalNumberReturned);
    }
    if (s_f_clearLogo) {
        s_f_clearLogo = false;
        clearLogo();
    }
    if (s_f_clearStationName) {
        s_f_clearStationName = false;
        clearStationName();
    }

    if (s_f_playlistEnabled) {
        if (!audio.isRunning() && !s_f_pauseResume) { processPlaylist(); }
    }
    //-----------------------------------------------------0.1 SEC------------------------------------------------------------------------------------
    if (s_f_100ms) { // calls every 0.1 second
        s_f_100ms = false;

        if (s_state == RADIO && s_subState_radio == 0) VUmeter_RA.update(audio.getVUlevel());

        while (s_timeCounter.timer) {
            s_timeCounter.tmp--;
            if (s_timeCounter.tmp) break;
            s_timeCounter.tmp = s_timeCounter.factor;
            s_timeCounter.timer--;
            dispFooter.updateTC(s_timeCounter.timer);
            if (s_timeCounter.timer) break;

            // s_timeCounter.timer is 0
            if (volBox.is_enabled()) volBox.hide();
            if (s_f_sleeping) return; // tc is active by pressing a button, but do nothing if "off"
            if (s_state == RADIO) {
                if (!txt_RA_staName.is_enabled()) { txt_RA_staName.show(); } // assume volBox is shown
                if (s_subState_radio == 1) { changeState(RADIO, 0); }        // Mute, Vol+, Vol-, Sta+, Sta-, StaList
                if (s_subState_radio == 2) { changeState(RADIO, 0); }        // Player, DLNA, Clock, SleepTime, Brightness, EQ, BT, Off
            } else if (s_state == STATIONSLIST) {
                changeState(RADIO, 0);
            } else if (s_state == PLAYER) {
                if (!txt_PL_fName.is_enabled()) { txt_PL_fName.show(); } // assume volBox is shown
            } else if (s_state == AUDIOFILESLIST) {
                changeState(PLAYER, 0);
            } else if (s_state == DLNA) {
                if (!txt_DL_fName.is_enabled()) { txt_DL_fName.show(); } // assume volBox is shown
            } else if (s_state == DLNAITEMSLIST) {
                changeState(DLNA, 0);
            } else if (s_state == CLOCK) {
                changeState(CLOCK, 0);
            } else {
                ;
            } // all other, do nothing
        }

        if (!s_f_rtc) { s_f_rtc = systemTimeSynced(); }
        // ------------------------------------------- volume / mute --------------------------------------------------------------------------------
        if (!s_f_mute) {
            if (audio.getVolume() != s_volume.cur_volume) { audio.setVolume(s_volume.cur_volume); }
        } else {
            if (audio.getVolume() != 0) { audio.setVolume(0); }
        }

        // ------------------------------------------- message box ----------------------------------------------------------------------------------
        if (s_f_msg_box) {                // messagebox is visible?
            if (s_timestamp < millis()) { // time to hide
                s_f_msg_box = false;
                msg_box.hide();
                if (s_f_esp_restart) { // restart after time
                    s_f_esp_restart = false;
                    ESP.restart();
                }
            }
        }
    }
    //----------------------------------------------------- 1 SEC ------------------------------------------------------------------------------------

    if (s_f_1sec) { // calls every second
        s_f_1sec = false;
        s_totalRuntime++;
        // for(int i = 0; i< 3; i++){
        //     uint8_t* sa = audio.getSpectrum();
        //     MWR_LOG_INFO("{}, {}, {}", sa[0], sa[1], sa[2]);
        // }
        uint16_t minuteOfTheDay = rtc.getMinuteOfTheDay();
        uint8_t  weekDay = rtc.getweekday();
        clk_CL_24.updateTime(minuteOfTheDay, weekDay);
        if (s_state == RINGING) clk_RI_24small.updateTime(minuteOfTheDay, weekDay);
        static uint8_t semaphore = 0;
        if (!semaphore) { s_f_alarm = isAlarm(weekDay, s_alarmdays, minuteOfTheDay, s_alarmtime) && s_f_rtc; } // alarm if rtc and CL green
        if (s_f_alarm) { semaphore++; }
        if (semaphore) { semaphore++; }
        if (semaphore >= 65) { semaphore = 0; }

        //------------------------------------------ALARM MANAGEMENT----------------------------------------------------------------------------------
        if (s_f_alarm) {
            s_f_alarm = false;
            if (s_f_sleeping) wake_up(RINGING, 0);
            else changeState(RINGING, 0);
        }
        if (s_f_eof_alarm) { // AFTER RINGING
            s_f_eof_alarm = false;
            if (!s_f_rtc) return;
            s_volume.cur_volume = s_volume.volumeAfterAlarm;
            changeState(RADIO, 0);
        }

        if (s_f_stationsChanged) {
            s_f_stationsChanged = false;
            staMgnt.updateStationsList();
        }
        //------------------------------------------UPDATE DISPLAY------------------------------------------------------------------------------------
        if (!s_f_sleeping || s_state == RINGING) {
            dispHeader.updateTime(s_time_s.c_get(), false);
            if (s_f_newBitRate) {
                s_f_newBitRate = false;
                dispFooter.updateBitRate(s_icyBitRate);
            }
            if (s_f_newStationName) {
                s_f_newStationName = false;
                showStationName();
            }
        }
        //---------------------------------------------TIME SPEECH -----------------------------------------------------------------------------------
        static bool f_resume = false;
        if (s_f_timeSpeech) { // speech the time 7 sec before a new hour is arrived
            s_f_timeSpeech = false;
            ps_ptr<char> hh = s_time_s.substr(0, 2);
            int          hour = hh.to_uint32();
            hour++;
            if (hour == 24) hour = 0; //  extract the hour
            if (s_f_mute) return;
            if (s_f_sleeping) return;
            if (s_state != RADIO) return;
            if (s_f_timeAnnouncement) {
                f_resume = true;
                s_f_eof = false;
                ps_ptr<char> p;
                p.assignf("/voice_time/{}/{}_00.mp3", s_timeSpeechLang.c_get(), hour);
                connecttoFS("SD_MMC", p.c_get());
                return;
            } else {
                printfln(s_tag.action, "Time announcement at " ANSI_ESC_BG_CYAN "{}" ANSI_ESC_RESET " o'clock is silent", hour);
            }
        }
        if (f_resume && s_f_eof) {
            f_resume = false;
            s_f_eof = false;
            setStation(s_cur_station);
            return;
        }
        //------------------------------------------AUDIO_CURRENT_TIME - DURATION---------------------------------------------------------------------
        if (audio.isRunning()) {
            s_audioFileDuration = audio.getAudioFileDuration();
            if (s_audioFileDuration > 0) {
                s_audioCurrentTime = audio.getAudioCurrentTime();
                if (s_state == PLAYER && s_audioFileDuration) {
                    pgb_PL_progress.setNewMinMaxVal(0, s_audioFileDuration);
                    pgb_PL_progress.setValue(s_audioCurrentTime);
                }
                if (s_state == DLNA && s_audioFileDuration) {
                    pgb_DL_progress.setNewMinMaxVal(0, s_audioFileDuration);
                    pgb_DL_progress.setValue(s_audioCurrentTime);
                }
                if (s_audioFileDuration) {
                    printfcr(s_tag.action, ANSI_ESC_GREEN "AudioCurrentTime " ANSI_ESC_GREEN "{}:{:02}s, " ANSI_ESC_GREEN "AudioFileDuration " ANSI_ESC_GREEN "{}:{:02}s      ", (long int)s_audioCurrentTime / 60, (long int)s_audioCurrentTime % 60, (long int)s_audioFileDuration / 60,
                             (long int)s_audioFileDuration % 60);
                }
            }
        }
        //------------------------------------------NEW STREAMTITLE-----------------------------------------------------------------------------------
        if (s_f_newStreamTitle && s_timeCounter.timer == 0) {
            s_f_newStreamTitle = false;
            if (s_state == RADIO) {
                if (s_streamTitle.strlen())
                    showStreamTitle(s_streamTitle);
                else if (s_icyDescription.strlen()) {
                    showStreamTitle(s_icyDescription);
                    s_f_newIcyDescription = false;
                    webSrv.send("icy_description=", s_icyDescription.c_get());
                } else {
                    txt_RA_sTitle.setText("");
                    txt_RA_sTitle.show();
                }
            }
            webSrv.send("streamtitle=", s_streamTitle.c_get());
        }
        if (s_f_newLyrics) {
            s_f_newLyrics = false;
            if (s_state == RADIO) showStreamTitle(s_lyrics);
            if (s_state == PLAYER) showPlayerFileName(s_lyrics.c_get());
            if (s_state == DLNA) show_DLNA_FileName(s_lyrics.c_get());
        }
        //------------------------------------------NEW ICY-DESCRIPTION-------------------------------------------------------------------------------
        if (s_f_newIcyDescription && s_timeCounter.timer == 0) {
            if (s_state == RADIO) {
                if (!s_streamTitle.strlen()) showStreamTitle(s_icyDescription);
            }
            webSrv.send("icy_description=", s_icyDescription.c_get());
            s_f_newIcyDescription = false;
        }
        //------------------------------------------DETERMINE AUDIOCODEC------------------------------------------------------------------------------
        if (s_cur_Codec == 0) {
            uint8_t c = audio.getCodec();
            if (c != 0 && c < 8) { // unknown or OGG, guard: c {1 ... 7, 9}
                s_cur_Codec = c;
                printfln(s_tag.audio_codec, ANSI_ESC_YELLOW "{}", codecname[c]);
                if (s_state == PLAYER) showFileLogo(PLAYER, s_subState_player);
            }
        }
        //------------------------------------------CONNECT TO LASTHOST-------------------------------------------------------------------------------
        if (s_f_connectToLastStation) { // not used yet
            s_f_connectToLastStation = false;
            setStation(s_cur_station);
        }
        //----------------------------------------------SD RECORDER-----------------------------------------------------------------------------------
        if (s_f_recording) {
            // MWR_LOG_WARN("recording");
            if (audio.isRunning()) {
                if (!recorder.running) {
                    recorder.startRequested = true;
                    recorder.running = true;
                    printfln(s_tag.action, "Start recording");
                }
            }
        } else {
            if (recorder.running) {
                recorder.sampleRate = audio.getSampleRate();
                printfln(s_tag.action, "Stop recording");
                recorder.stopRequested = true;
                recorder.running = false;
            }
        }
        //------------------------------------------RECONNECT AFTER FAIL------------------------------------------------------------------------------
        if (s_f_reconnect && !s_f_isWiFiConnected) { // not used yet
            s_f_reconnect = false;
            connecttohost(s_settings.lastconnectedhost.get());
        }
        //------------------------------------------SEEK DLNA SERVER----------------------------------------------------------------------------------
        if (s_f_dlnaSeekServer) {
            s_f_dlnaSeekServer = false;
            dlna.seekServer();
        }
        //------------------------------------------CREATE DLNA PLAYLIST------------------------------------------------------------------------------
        if (s_f_dlnaMakePlaylistOTF && s_f_dlna_browseReady) {
            s_f_dlnaMakePlaylistOTF = false;
            s_f_dlna_browseReady = false;
            //    if( playlist.create_playlist_from_DLNA_folder()) s_f_playlistEnabled = true;
        }
        //------------------------------------------DLNA ITEMS RECEIVED-------------------------------------------------------------------------------
        if (s_f_dlna_browseReady) { // unused
            s_f_dlna_browseReady = false;
        }
        //-------------------------------------------WIFI DISCONNECTED?-------------------------------------------------------------------------------
        if (WiFi.isConnected() == false) {
            printfln(s_tag.wifi_info, ANSI_ESC_YELLOW "Reconnecting to WiFi...");
            dispHeader.updateRSSI(-86);
            s_f_WiFi_lost = true;
        } else {
            if (s_f_WiFi_lost) {
                s_f_WiFi_lost = false;
                if (s_state == RADIO) audio.connecttohost(s_settings.lastconnectedhost.get());
            }
        }
        s_f_WiFi_lost == false ? dispHeader.updateRSSI(WiFi.RSSI()) : dispHeader.updateRSSI(-86);
        dispFooter.updateAntenna(s_f_WiFi_lost);
        //------------------------------------------GET AUDIO FILE ITEMS------------------------------------------------------------------------------
        if (s_f_isFSConnected) {
            //    uint32_t t = 0;
            //    uint32_t fs = audioGetFileSize();
            //    uint32_t br = audioGetBitRate();
            //    if(br) t = (fs * 8)/ br;
            //    MWR_LOG_DEBUG("Br {}, Dur {}s", br, t);
        }
        //--------------------------------------------- BT EMITTER ----------------------------------------------------------------------------------
        if (s_bt_emitter.found) {
            btn_RA_bt.set_active(true);
            if (s_bt_emitter.enabled) {
                if (!s_f_sleeping) {
                    if (!bt_emitter.get_power_state()) bt_emitter.power_on(s_bt_emitter.mode.c_get());
                } else {
                    if (bt_emitter.get_power_state()) bt_emitter.power_off();
                }
            } else {
                if (bt_emitter.get_power_state()) { bt_emitter.power_off(); }
            }
            if (bt_emitter.getMode().equals("NA")) {
                ; // not ready yet
            } else if (bt_emitter.get_power_state() && !bt_emitter.getMode().equals(s_bt_emitter.mode)) {
                bt_emitter.setMode(s_bt_emitter.mode);
            }
        }
    } //  END s_f_1sec
    //------------------------------------------------------------------------------------------------------------------------------------------------
    if (s_f_10sec == true) { // calls every 10 seconds
        s_f_10sec = false;
        // if(s_state == RADIO && !s_icyBitRate && !s_f_sleeping) {
        //     s_decoderBitRate = audio.getBitRate();
        //     static uint32_t oldBr = 0;
        //     if(s_decoderBitRate != oldBr){
        //         oldBr = s_decoderBitRate;
        //         dispFooter.updateBitRate(s_decoderBitRate);
        //     }
        // }
        updateSettings();
    }

    if (s_f_1min == true) { // calls every minute
        s_f_1min = false;
        if (s_sleeptime) {
            s_sleeptime--;
            if (!s_sleeptime) fall_asleep();
            dispFooter.updateOffTime(s_sleeptime);
        }
        // static uint8_t btEmitterCnt = 0;
        // if (!s_bt_emitter.found && btEmitterCnt < 1) {
        //     btEmitterCnt++;
        //     bt_emitter.begin(); // if the emitter has not yet responded
        // }
    }

    //-------------------------------------------------DEBUG / WIFI_SETTINGS ----------------------------------------------------------------------------------
    if (Serial.available()) { // input: serial terminal

        String r = Serial.readString();
        r.replace("\n", "");
        printfln(s_tag.terminal, ANSI_ESC_YELLOW "{}", r.c_str());
        if (r.startsWith("pr")) {
            if (audioPauseResumeAndUpdateState()) {
                printfln(s_tag.terminal, ANSI_ESC_YELLOW "Pause-Resume");
            } else {
                printfln(s_tag.terminal, ANSI_ESC_YELLOW "Pause-Resume not possible");
            }
        }
        if (r.startsWith("hc")) { // A make_hardcopy_on_sd of the display is created and written to the SD card
            { printfln(s_tag.terminal, ANSI_ESC_YELLOW "create hardcopy"); }
            make_hardcopy_on_sd();
        }
        if (r.startsWith("rts")) { // run time stats
            char* timeStatsBuffer = x_ps_calloc(2000, sizeof(char));
            GetRunTimeStats(timeStatsBuffer);
            { printfln(s_tag.terminal, ANSI_ESC_YELLOW "task statistics\n\n{}", timeStatsBuffer); }
            x_ps_free(&timeStatsBuffer);
        }
        if (r.startsWith("cts")) { // connect to speech
            audio.connecttospeech("Hallo, wie geht es dir? Morgen scheint die Sonne und übermorgen regnet es.Aber wir nehmen den Regenschirm mit. Und auch den Rucksack. Dann lesen wir aus dem Buch "
                                  "Hier gibt es nur gutes Wetter.",
                                  "de");
            //    audio.connecttospeech("Hallo", "de");
        }

        if (r.startsWith("bfi")) { // buffer filled
            printfln(s_tag.terminal, "inBuffer filled {} bytes", (long unsigned)audio.inBufferFilled());
            printfln(s_tag.terminal, "inBuffer free   {} bytes", (long unsigned)audio.inBufferFree());
        }
        if (r.startsWith("st")) { // testtext for streamtitle
            if (r[2] == '0') s_streamTitle = "A Ё Ю";
            if (r[2] == '1') s_streamTitle = "A B C D E F G";
            if (r[2] == '2') s_streamTitle = "A B C D E F G H I";
            if (r[2] == '3') s_streamTitle = "A B C D E F G H I J K L";
            if (r[2] == '4') s_streamTitle = "A B C D E F G H I J K J M Q O";
            if (r[2] == '5') s_streamTitle = "A B C D E F G H I K L J M y O P Q R";
            if (r[2] == '6')
                s_streamTitle = "A B C D E F G H I K L J M g O P Q R S T V A B C D E F G H I K L J M p O P Q R S T U V W K J Q p O P Q R S T U V W K J Q A B C D E F G H I K L J M p O P Q R S T "
                                "U V W K J Q p O P Q R S T U V W K J Q V A B C D E F G H I K L J M p O P Q R S T U V W K J Q p O P Q R S T U V W K J Q A B C D E F G H I K L J M p O P Q R S T U "
                                "V W K J Q p O P Q R S T U V W K J Q";
            if (r[2] == '7')
                s_streamTitle = "A B C D E F G H I K L J M j O P Q R S T U V A B C D E F G H I K L J M p O P Q R S T U V W K J Q p O P Q R S T U V W K J Q A B C D E F G H I K L J M p O P Q R S "
                                "T U V W K J Q p O P Q R S T U V W K J Q";
            if (r[2] == '8') s_streamTitle = "A B C D E F G H I K L J M p O P Q R S T U V W A B C D E F G H I K L J M p O P Q R S T U V W K J Q p O P Q R S T U V W K J Q";
            if (r[2] == '9') s_streamTitle = "A B C D E F G H I K L J M p O P Q R S T U V W K J Q p O P Q R S T U V W K J Q";
            printfln(s_tag.terminal, "st: {}", s_streamTitle.c_get());
            s_f_newStreamTitle = true;
        }
        if (r.startsWith("ais")) { // openAIspeech
            printfln(s_tag.terminal, "openAI speech");
            //    audio.openai_speech("openAI-key", "tts-1", "Today is a wonderful day to build something people love!", "", "shimer", "mp3", "1");
        }
        if (r.startsWith("ctfs")) { // connecttoFS
                                    //     MWR_LOG_INFO("SPIFFS");
            connecttoFS("SD", "/Collide.ogg");
        }
        if (r.startsWith("pl")) { // temp debug: play local library track N, e.g. "pl0"
            const uint16_t idx = static_cast<uint16_t>(r.substring(2).toInt());
            LocalTrackItem item{};
            const bool haveItem = playerCoreLocalTrack(idx, &item);
            printfln(s_tag.terminal, "pl{}: haveItem={} path={} title={}", idx, haveItem, haveItem ? item.path : "?", haveItem ? item.title : "?");
            const bool started = playerService.playLocalTrack(idx);
            printfln(s_tag.terminal, "pl{}: playLocalTrack started={}", idx, started);
        }
        if (r.startsWith("stoff")) { // setTimeOffset
            int32_t t = r.substring(3, r.length() - 1).toInt();
            printfln(s_tag.terminal, "setTimeOffset {}", t);
            audio.setTimeOffset(t);
        }

        if (r.startsWith("sapt")) { // setAudioPlayTime
            uint32_t t = r.substring(4, r.length()).toInt();
            printfln(s_tag.terminal, "setAudioPlayTime {}", t);
            audio.setAudioPlayTime(t);
        }

        if (r.startsWith("gafp")) { // getAudioFilePosition
            printfln(s_tag.terminal, "getAudioFilePosition {}", audio.getAudioFilePosition());
        }

        if (r.startsWith("safp")) { // setAudioFilePosition
            uint32_t t = r.substring(4, r.length()).toInt();
            printfln(s_tag.terminal, "setAudioFilePosition {}", t);
            audio.setAudioFilePosition(t);
        }

        if (r.startsWith("grn")) { // list of all self registered objects
            get_registered_names();
        }
        if (r.startsWith("fm")) { // force mono
            static bool f_mono = false;
            f_mono = !f_mono;
            audio.forceMono(f_mono);
            if (f_mono)
                printfln(s_tag.terminal, "mono");
            else
                printfln(s_tag.terminal, "stereo");
        }
        if (r.startsWith("sm")) { // force mono
            static bool f_mute = false;
            f_mute = !f_mute;
            audio.setMute(f_mute);
            if (f_mute)
                printfln(s_tag.terminal, "mute on");
            else
                printfln(s_tag.terminal, "mute off");
        }
        if (r.startsWith("o48")) { // output48KHz
            static bool f_o48 = false;
            f_o48 = !f_o48;
            if (f_o48) {
                audio.setOutputSampleRate(Audio::SR_48000);
                printfln(s_tag.terminal, "output 48KHz");
            } else {
                audio.setOutputSampleRate(Audio::SR_ORIGIN);
                printfln(s_tag.terminal, "normal output {} Hz", audio.getSampleRate());
            }
        }
        if (r.startsWith("o44")) { // output48KHz
            static bool f_o44 = false;
            f_o44 = !f_o44;
            if (f_o44) {
                audio.setOutputSampleRate(Audio::SR_44100);
                printfln(s_tag.terminal, "output 44.1KHz");
            } else {
                audio.setOutputSampleRate(Audio::SR_ORIGIN);
                printfln(s_tag.terminal, "normal output {} Hz", audio.getSampleRate());
            }
        }
        if (r.startsWith("btp")) { // bluetooth RX/TX protocol
            bt_emitter.list_protokol();
        }
        if (r.startsWith("btstr")) { // bluetooth string, send to bt emitter e.g. btstr:AT+
            bt_emitter.userCommand(r.substring(6, r.length() - 1).c_str());
            printfln(s_tag.terminal, "btstr: {}", r.substring(6, r.length() - 1).c_str());
        }
        if (r.startsWith("tsp")) { s_f_timeSpeech = true; }
        if (r.startsWith("pwd")) { // set password for WiFi
            changeState(WIFI_SETTINGS, 0);
        }
        if (r.startsWith("gif")) { // draw gif image
            printfln(s_tag.terminal, "gif");
            drawImage("/common/Tom_Jerry.gif", 100, 100);
        }
        static uint32_t time = 0;
        if (r.startsWith("stops")) { // stop song
            time = audio.stopSong();
            printfln(s_tag.terminal, "file {} stopped at time {}", s_cur_AudioFileName.c_get(), time);
        }
        if (r.startsWith("starts")) { // start song
            ps_ptr<char> path = "/audiofiles/" + s_cur_AudioFileName;
            bool         ret = audio.connecttoFS(SD_MMC, path.c_get(), time);
            printfln(s_tag.terminal, "file {} started at time {}, ret {}", s_cur_AudioFileName.c_get(), time, ret);
        }

        if (r.startsWith("gbr")) { // get bitrate
            uint32_t br = audio.getBitRate();
            printfln(s_tag.terminal, "bitrate: {}", br);
        }
        if (r.startsWith("ibs")) { // inbuff status
            audio.inBufferStatus();
        }
        if (r.startsWith("ir")) { // is running?
            printfln(s_tag.terminal, "is running: {}", audio.isRunning());
        }
        if (r.startsWith("vfs")) { // volume fading speed
            float t = r.substring(3, r.length() - 1).toFloat();
            printfln(s_tag.terminal, "set volume fading speed {}, current: {}", t, audio.settings.VOL_FADING_SPEED);
            audio.settings.VOL_FADING_SPEED = t;
        }
    }
}

/*         ╔═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
           ║                                                                                  E V E N T S                                                                                ║
           ╚═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝   */

// Events from audioI2S library
void my_audio_info(Audio::msg_t m) {
    switch (m.e) {
        case Audio::evt_info:
            if (endsWith(m.msg, "failed!")) {
                playerService.onError(m.msg);
                printflnCut(s_tag.audio_info, "", ANSI_ESC_YELLOW, m.msg);

                s_streamTitle.assignf(ANSI_ESC_ORANGE "{}", m.msg);
                s_f_newStreamTitle = true;
                s_f_webFailed = true;
                return;
            }
            if (startsWith(m.msg, "FLAC")) {
                printflnCut(s_tag.audio_info, "", ANSI_ESC_GREEN, m.msg);
                return;
            }
            if (endsWith(m.msg, "Stream lost")) {
                printflnCut(s_tag.audio_info, "", ANSI_ESC_YELLOW, m.msg);
                return;
            }
            if (startsWith(m.msg, "authent")) {
                printflnCut(s_tag.audio_info, "", ANSI_ESC_GREEN, m.msg);
                return;
            }
            if (startsWith(m.msg, "StreamTitle=")) { return; }
            if (startsWith(m.msg, "BitsPerSample")) {
                // es8311.setBitsPerSample(m.arg1);
            }
            if (startsWith(m.msg, "SampleRate (Hz)")) {
                // es8311.setSampleRate(m.arg1);
            }
            if (startsWith(m.msg, "HTTP/") && m.msg[9] > '3') {
                printflnCut(s_tag.audio_info, "", ANSI_ESC_RED, m.msg);
                return;
            }
            if (startsWith(m.msg, "ERROR:")) {
                playerService.onError(m.msg);
                printflnCut(s_tag.audio_info, "", ANSI_ESC_RED, m.msg);
                return;
            }
            if (CORE_DEBUG_LEVEL >= ARDUHAL_LOG_LEVEL_WARN) {
                printfln(s_tag.audio_info, ANSI_ESC_GREEN "{}", m.msg);
                return;
            } // all other
            break;

        case Audio::evt_name:
            s_stationName_air = m.msg; // set max length
            playerService.onMetadata(m.msg, nullptr);
            printfln(s_tag.audio_info, "StationName: " ANSI_ESC_MAGENTA "{}", m.msg);
            s_f_newStationName = true;
            break;

        case Audio::evt_streamtitle:
            s_streamTitle = m.msg;
            playerService.onMetadata(nullptr, m.msg);
            printfln(s_tag.audio_info, "StreamTitle: " ANSI_ESC_YELLOW "{}", m.msg);
            s_f_newStreamTitle = true;
            break;

        case Audio::evt_eof:
            playerService.onEndOfFile();
            ++s_eofCount; // see PlayerSnapshot::eofCount
            // 播到结尾 = 完整播放。先记完成、再清状态，否则下面的
            // libraryFinishCurrentTrack() 会把它误判成跳过。
            // 注意 evt_eof 对网络电台也会触发，但那时 s_playingLocalId 是 0
            //（只有 playerCorePlaySdFile 会设它），所以不会误记。
            if (s_playingLocalId) {
                libraryLogEvent(s_playingLocalId, kEventPlayCompleted);
                s_playCompleted = true;
            }
            s_f_isWebConnected = false;
            s_f_eof = true;
            s_f_isFSConnected = false;
            printflnCut(s_tag.audio_info, "end of file: ", ANSI_ESC_YELLOW, m.msg);
            if (s_state == PLAYER) {
                webSrv.send("SD_playFile=", "end of audiofile");
                if (!s_f_playlistEnabled) {
                    //    s_f_clearLogo = true;
                    //    s_f_clearStationName = true;
                    changeState(PLAYER, 0);
                }
            }
            if (s_state == RADIO) {}
            if (s_state == DLNA) {
                txt_DL_fName.setText("");
                txt_DL_fName.show();
                btn_DL_pause.setActive(false);
                btn_DL_pause.show();
            }
            if (s_state == RINGING) {
                if (startsWith(m.msg, "alarm")) s_f_eof_alarm = true;
            }
            s_f_eof = true;
            break;

        case Audio::evt_lasthost:
            if (s_f_playlistEnabled) return;
            if (s_state == RADIO) s_settings.lastconnectedhost.assign(m.msg);
            printflnCut(s_tag.audio_info, "lastURL: ", ANSI_ESC_YELLOW, m.msg);
            webSrv.send("stationURL=", m.msg);
            break;

        case Audio::evt_icyurl:
            if (strlen(m.msg) > 5) {
                printflnCut(s_tag.audio_info, "icy-url: ", ANSI_ESC_YELLOW, m.msg);
                s_homepage = m.msg;
                if (!s_homepage.starts_with("http")) s_homepage = "http://" + s_homepage;
            }
            break;

        case Audio::evt_icylogo:
            if (strlen(m.msg) > 5) { printflnCut(s_tag.audio_info, "icy-logo: ", ANSI_ESC_RESET, m.msg); }
            break;

        case Audio::evt_id3data: printfln(s_tag.audio_info, "id3data: " ANSI_ESC_GREEN "{}", m.msg); break;

        case Audio::evt_image:
            for (int i = 0; i < m.vec.size(); i += 2) { printfln(s_tag.audio_info, "CoverImage: " ANSI_ESC_GREEN "segment {:02}, pos {:08}, len {:08}", i / 2, m.vec[i], m.vec[i + 1]); }
            break;

        case Audio::evt_icydescription:
            s_icyDescription = m.msg;
            s_f_newIcyDescription = true;
            if (strlen(m.msg)) printfln(s_tag.audio_info, "icy-descr: " ANSI_ESC_YELLOW "{}", m.msg);
            break;

        case Audio::evt_bitrate:
            if (!strlen(m.msg)) return; // guard
            s_icyBitRate = str2int(m.msg);
            s_f_newBitRate = true;
            printfln(s_tag.audio_info, "bitRate: " ANSI_ESC_CYAN "{}", s_icyBitRate);
            break;

        case Audio::evt_lyrics:
            printfln(s_tag.audio_info, "sync lyrics: " ANSI_ESC_YELLOW "{}", m.msg);
            s_lyrics = m.msg;
            s_f_newLyrics = true;
            break;

        case Audio::evt_genre: printfln(s_tag.audio_info, "genre: " ANSI_ESC_YELLOW "{}", m.msg); break;

        case Audio::evt_log: printfln(m.s, "{}", m.msg); break;

        default: printfln("message", "{}", m.msg); break;
    }
}

// ————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {

//     int16_t* buff16 = reinterpret_cast<int16_t*>(outBuff);

//     int16_t sineWaveTable[44] = {
//          0,   3743,   7377,  10793,  14082,  17136,  19848,  22113,  23825,  24908,
//       25311,  24908,  23825,  22113,  19848,  17136,  14082,  10793,   7377,   3743,
//          0,  -3743,  -7377, -10793, -14082, -17136, -19848, -22113, -23825, -24908,
//      -25311, -24908, -23825, -22113, -19848, -17136, -14082, -10793,  -7377,  -3743
//     };

//     static uint8_t tabPtr = 0;
//     for(int i= 0; i < validSamples; i++){
//        buff16[i * 4 + 1] += sineWaveTable[tabPtr] /50; // channel left
//        buff16[i * 4 + 3] += sineWaveTable[tabPtr] /50; // channel right
//         tabPtr++;
//         if(tabPtr == 44) tabPtr = 0;
//     }
//     *continueI2S = true;
// }
// ————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void on_BH1750(int32_t ambVal) { //--AMBIENT LIGHT SENSOR BH1750--
    int16_t bh1750Value = 0;
    s_bh1750Value = map_l(ambVal, 0, 1600, displayConfig.brightnessMin, displayConfig.brightnessMax);
    MWR_LOG_DEBUG("ambVal {}, bh1750Value {}, s_brightness {}", ambVal, bh1750Value, s_brightness);
    setTFTbrightness(s_brightness, s_bh1750Value);
}
// ————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void ftp_debug(const char* info) {
    if (startsWith(info, "File Name")) return;
    printfln(s_tag.ftp_server, "{}", info);
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
// Events from rtime library
void RTIME_info(const char* info) {
    printfln(s_tag.rtime_info, "{}", info);
}
// Events from tft library
void tft_info(const char* info) {
    printfln(s_tag.tft_info, "{}", info);
}
// Events from tp library
void tp_info(const char* info) {
    printfln(s_tag.tp_info, "{}", info);
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
// Events from IR Library
void ir_code(uint8_t addr, uint8_t cmd) {
    printfln(s_tag.ir_info, "ir_code: " ANSI_ESC_YELLOW "IR address " ANSI_ESC_BLUE "0x{:02X}, " ANSI_ESC_YELLOW "IR command " ANSI_ESC_BLUE "0x{:02X}", addr, cmd);
    char buf[20];
    sprintf(buf, "0x%02x", addr);
    webSrv.send("IR_address=", buf);
    sprintf(buf, "0x%02x", cmd);
    webSrv.send("IR_command=", buf);
}

void ir_res(uint32_t res) {
    if (s_state != RADIO) return;
    if (s_f_sleeping == true) return;
    printfln(s_tag.ir_info, "ir_result: " ANSI_ESC_YELLOW "Stationnumber " ANSI_ESC_BLUE "{}", (long unsigned)res);
    nbr_RA_staBox.hide();
    setStationByNumber(res);
    return;
}
void ir_number(uint16_t num) {
    if (s_state != RADIO) return;
    if (s_f_sleeping) return;
    nbr_RA_staBox.enable();
    nbr_RA_staBox.setNumbers(num);
    nbr_RA_staBox.show(TFT_ORANGE);
}

void ir_released(int8_t key) {
    printfln(s_tag.ir_info, "ir_code: " ANSI_ESC_YELLOW "released ir key nr: " ANSI_ESC_BLUE "{:02}, <{}>", key, ir_symbols[key]);
    tp_released(0, 0);
    return;
}
// ————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void ir_long_key(int8_t key) {
    printfln(s_tag.ir_info, "ir_code: " ANSI_ESC_YELLOW "long pressed ir key nr: " ANSI_ESC_BLUE "{:02}, <{}>", key, ir_symbols[key]);
    if (key == 16) {
        if (!s_f_sleeping)
            fall_asleep(); // long OK
        else
            wake_up(RADIO, 0);
    }
}
// ————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// clang-format off
void ir_short_key(int8_t key) {
    s_f_ok_from_ir = false;
    printfln(s_tag.ir_info, "ir_code: " ANSI_ESC_YELLOW "short pressed ir key nr: " ANSI_ESC_BLUE "{:02}, <{}>", key, ir_symbols[key]);
    if (s_f_sleeping == true && key != 20) return;
    if (s_state == IR_SETTINGS) return; // nothing todo

    switch (key) {
        case 10: // MUTE  ----------------------------------------------------------------------------------------------------------------------------
            muteChanged(!s_f_mute);
            return;
        case 11: // ARROW RIGHT  ---------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                if (s_subState_radio == 0) { nextFavStation(); } // NEXT STATION
                if (s_subState_radio == 2) { set_ir_pos_RA(IR_RIGHT); setTimeCounter(2); } // scroll right
                return;
            }
            if (s_state == STATIONSLIST) { // next page
                lst_RADIO.nextPage(); setTimeCounter(LIST_TIMER);
                break;
            }
            if (s_state == PLAYER) {
                set_ir_pos_PL(IR_RIGHT); // scroll right
                return;
            }
            if (s_state == AUDIOFILESLIST) {
                lst_PLAYER.nextPage();
                setTimeCounter(LIST_TIMER);
                return; // next page
            }
            if (s_state == DLNA) {
                set_ir_pos_DL(IR_RIGHT); // scroll forward (mute, pause, cancel, prev, next)
                return;
            }
            if (s_state == DLNAITEMSLIST) {
                lst_DLNA.nextPage(); setTimeCounter(LIST_TIMER); return; // nextpage
            }
            if (s_state == CLOCK) {
                if (s_subState_clock == 1) { // scroll backward (alarm, radio, mute, off)
                    set_ir_pos_CL(IR_RIGHT); // scroll right
                    setTimeCounter(2);
                }
                return;
            }
            if (s_state == ALARMCLOCK) {
                set_ir_pos_AC(IR_RIGHT); // scroll forward (left, right, up, down, ready)
                return;
            }
            if (s_state == SLEEPTIMER) {
                set_ir_pos_SL(IR_RIGHT); // scroll forward (up, down, ready, cancel)
                return;
            }
            if (s_state == SETTINGS) {
                set_ir_pos_SE(IR_RIGHT); // scroll forward (bright, equal, wifi, radio)
                return;
            }
            if (s_state == BRIGHTNESS) {
                s_brightness += 5;
                s_brightness = clamp_min_max(s_brightness, displayConfig.brightnessMin, displayConfig.brightnessMax);
                sdr_BR_value.setValue(s_brightness);
            }
            if (s_state == EQUALIZER) { // scroll forward (radio, player, mute)
                if(s_ir_btn_select < 3) set_ir_pos_EQ(IR_RIGHT);
                else {
                    if (s_ir_btn_select == 3) { sdr_EQ_balance.setValue(sdr_EQ_balance.getValue() + 1);   setI2STone();} // balance
                    if (s_ir_btn_select == 4) { sdr_EQ_lowPass.setValue(sdr_EQ_lowPass.getValue() + 1);   setI2STone();} // lowpass
                    if (s_ir_btn_select == 5) { sdr_EQ_bandPass.setValue(sdr_EQ_bandPass.getValue() + 1); setI2STone();} // bandpass
                    if (s_ir_btn_select == 6) { sdr_EQ_highPass.setValue(sdr_EQ_highPass.getValue() + 1); setI2STone();} // highpass
                }
            }
            if (s_state == BLUETOOTH) { // scroll forward (bright, equal, radio)
                set_ir_pos_BT(IR_RIGHT);
            }
            break;
        case 12: // ARROW LEFT  ----------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                if (s_subState_radio == 0) { prevFavStation(); return; } // PREV STATION
                if (s_subState_radio == 2) { set_ir_pos_RA(IR_LEFT);  setTimeCounter(2);  return; } // scroll left
            }
            if (s_state == STATIONSLIST) {
                lst_RADIO.prevPage();
                setTimeCounter(LIST_TIMER);
                return;
            } // prev page
            if (s_state == PLAYER) {
                if(s_subState_player == 0){ // scroll backward (prev file, next file, ready, play all, shuffle, file list, radio, off)
                    set_ir_pos_PL(IR_LEFT); setTimeCounter(2);
                    return;
                }
                if(s_subState_player == 1){ // scroll backward (mute, pause, cancel, prev, next)
                    set_ir_pos_PL(IR_LEFT); setTimeCounter(2);
                    return;
                }
            }
            if (s_state == AUDIOFILESLIST) { // prev page
                lst_PLAYER.prevPage();
                setTimeCounter(LIST_TIMER);
                break;
            }
            if (s_state == DLNA) { // scroll backward (mute, pause, cancel, prev, next)
                set_ir_pos_DL(IR_LEFT);
                return;
            }
            if (s_state == DLNAITEMSLIST) { // prev page
                lst_DLNA.prevPage(); setTimeCounter(LIST_TIMER); return;
            }
            if (s_state == CLOCK) {
                if (s_subState_clock == 1) { // scroll backward (alarm, radio, mute, off)
                    set_ir_pos_CL(IR_LEFT);
                    setTimeCounter(2);
                    return;
                }
            }
            if (s_state == ALARMCLOCK) { // scroll backward (left, right, up, down, ready)
                    set_ir_pos_AC(IR_LEFT);
                    return;
            }
            if (s_state == SLEEPTIMER) { // scroll backward (up, down, ready, cancel)
                    set_ir_pos_SL(IR_LEFT);
                    return;
            }
            if (s_state == SETTINGS) { // scroll forward (bright, equal, radio)
                    set_ir_pos_SE(IR_LEFT);
                    return;
            }
            if (s_state == BRIGHTNESS) {
                s_brightness -= 5;
                s_brightness = clamp_min_max(s_brightness, displayConfig.brightnessMin, displayConfig.brightnessMax);
                sdr_BR_value.setValue(s_brightness);
                setTimeCounter(2);
                return;
            }
            if (s_state == EQUALIZER) { // scroll backward (radio, player, mute)
                if(s_ir_btn_select < 3) set_ir_pos_EQ(IR_LEFT);
                else {
                    if (s_ir_btn_select == 3) { sdr_EQ_balance.setValue(sdr_EQ_balance.getValue() - 1);   setI2STone();} // balance
                    if (s_ir_btn_select == 4) { sdr_EQ_lowPass.setValue(sdr_EQ_lowPass.getValue() - 1);   setI2STone();} // lowpass
                    if (s_ir_btn_select == 5) { sdr_EQ_bandPass.setValue(sdr_EQ_bandPass.getValue() - 1); setI2STone();} // bandpass
                    if (s_ir_btn_select == 6) { sdr_EQ_highPass.setValue(sdr_EQ_highPass.getValue() - 1); setI2STone();} // highpass
                }
            }
            if (s_state == BLUETOOTH) { // scroll forward (bright, equal, radio)
                set_ir_pos_BT(IR_LEFT);
            }
            break;
        case 13: // ARROW DOWN  ----------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                txt_RA_staName.hide();
                volBox.enable();
                downvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == STATIONSLIST) {
                lst_RADIO.nextStation();
                setTimeCounter(LIST_TIMER);
                break;
            } // station++
            if (s_state == PLAYER) {
                txt_PL_fName.hide();
                volBox.enable();
                downvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == AUDIOFILESLIST) {
                lst_PLAYER.nextFile();
                setTimeCounter(LIST_TIMER);
                break;
            } // file++
            if (s_state == DLNA) {
                txt_DL_fName.hide();
                volBox.enable();
                downvolume(); // VOLUME--
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            }
                if(s_state == DLNAITEMSLIST){lst_DLNA.nextItem(); setTimeCounter(LIST_TIMER); return;} // item++
            if (s_state == CLOCK) {
                downvolume();
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == SLEEPTIMER) {
                downvolume();
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == EQUALIZER) {
                set_ir_pos_EQ(IR_DOWN);
                break;
            }
            break;
        case 14: // ARROW UP  ------------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                txt_RA_staName.hide();
                volBox.enable();
                upvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == STATIONSLIST) {
                lst_RADIO.prevStation();
                setTimeCounter(LIST_TIMER);
                break;
            } // station--
            if (s_state == PLAYER) {
                txt_PL_fName.hide();
                volBox.enable();
                upvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == AUDIOFILESLIST) {
                lst_PLAYER.prevFile();
                setTimeCounter(LIST_TIMER);
                break;
            } // file-
            if (s_state == DLNA) {
                txt_DL_fName.hide();
                volBox.enable();
                upvolume(); // VOLUME++
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                break;
            }
            if(s_state == DLNAITEMSLIST) { lst_DLNA.prevItem(); setTimeCounter(LIST_TIMER); return; } // item++
            if (s_state == CLOCK) {
                upvolume();
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == SLEEPTIMER) {
                upvolume();
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == EQUALIZER) {
                set_ir_pos_EQ(IR_UP);
                return;
            }
            break;
        case 15: // MODE  ----------------------------------------------------------------------------------------------------------------------------
            if (s_state == SLEEPTIMER) {
                setStation(s_cur_station);
                changeState(RADIO, 0);
                break;
            }
            if (s_state == RADIO) {
                changeState(STATIONSLIST, 0);
                setTimeCounter(40);
                break;
            }
            if (s_state == STATIONSLIST) {
                changeState(PLAYER, 0);
                break;
            }
            if (s_state == PLAYER) {
                changeState(AUDIOFILESLIST, 0);
                break;
            }
            if (s_state == AUDIOFILESLIST) {
                changeState(DLNA, 0);
                break;
            }
            if (s_state == DLNA) {
                changeState(CLOCK, 0);
                break;
            }
            if (s_state == CLOCK) {
                s_sleepTimerSubMenue = 0;
                changeState(SLEEPTIMER, 0);
                break;
            }
            break;
        case 16: // OK -------------------------------------------------------------------------------------------------------------------------------
            s_f_ok_from_ir = true;
            if (s_state == RADIO) {
                if (s_subState_radio == 2) {
                    if (s_ir_btn_select == 0) { btn_RA_staList.click(); }
                    if (s_ir_btn_select == 1) { btn_RA_player.click(); }
                    if (s_ir_btn_select == 2) { btn_RA_dlna.click(); }
                    if (s_ir_btn_select == 3) { btn_RA_clock.click(); }
                    if (s_ir_btn_select == 4) { btn_RA_sleep.click(); }
                    if (s_ir_btn_select == 5) { btn_RA_settings.click(); }
                    if (s_ir_btn_select == 6) { btn_RA_bt.click(); }
                    if (s_ir_btn_select == 7) { btn_RA_off.click(); }
                } else {
                    changeState(RADIO, 2);
                    s_ir_btn_select = 0;
                    set_ir_pos_RA(0);
                }
                break;
            }
            if (s_state == STATIONSLIST) {
                setStationByNumber(lst_RADIO.getSelectedStation());
                changeState(RADIO, 0);
                break;
            }
            if (s_state == PLAYER) {
                if (s_subState_player == 0) {
                    if (s_ir_btn_select == 0) { btn_PL_prevFile.click(); }
                    if (s_ir_btn_select == 1) { btn_PL_nextFile.click(); }
                    if (s_ir_btn_select == 2) { btn_PL_ready.click(); }
                    if (s_ir_btn_select == 3) { btn_PL_playAll.click(); }
                    if (s_ir_btn_select == 4) { btn_PL_shuffle.click(); }
                    if (s_ir_btn_select == 5) { btn_PL_fileList.click(); }
                    if (s_ir_btn_select == 6) { btn_PL_radio.click(); }
                    if (s_ir_btn_select == 7) { btn_PL_off.click(); }
                }
                else if(s_subState_player == 1) {
                    if (s_ir_btn_select == 0) { btn_PL_mute.click(); }
                    if (s_ir_btn_select == 1) { btn_PL_pause.click(); }
                    if (s_ir_btn_select == 2) { btn_PL_cancel.click(); }
                    if (s_ir_btn_select == 3) { btn_PL_playPrev.click(); }
                    if (s_ir_btn_select == 4) { btn_PL_playNext.click(); }
                }
                if(s_ir_btn_select == -1){ s_ir_btn_select = 0; set_ir_pos_PL(0); }
                break;
            }
            if (s_state == AUDIOFILESLIST) {
                ps_ptr<char> r = lst_PLAYER.getSelectedFile();
                if (r != "") {
                    stopSong();
                    SD_playFile(lst_PLAYER.getSelectedFilePath(), 0, true);
                    s_cur_AudioFileNr = lst_PLAYER.getSelectedFileNr();
                }
                break;
            }
            if (s_state == DLNA) {
                if (s_ir_btn_select == 0) { btn_DL_mute.click(); }
                if (s_ir_btn_select == 1) { btn_DL_pause.click(); }
                if (s_ir_btn_select == 2) { btn_DL_cancel.click(); }
                if (s_ir_btn_select == 3) { btn_DL_fileList.click(); }
                if (s_ir_btn_select == 4) { btn_DL_radio.click(); }
                if (s_ir_btn_select == -1){ s_ir_btn_select = 0; set_ir_pos_DL(0); }
                break;
            }
            if(s_state == DLNAITEMSLIST) {
                ps_ptr<char> r = lst_DLNA.getSelectedURL();
                if(r) { txt_DL_fName.setTextColor(TFT_CYAN); txt_DL_fName.setText(lst_DLNA.getSelectedTitle()); changeState(DLNA, 0); connecttohost(r); }
                else setTimeCounter(2);
                break;
            }
            if (s_state == CLOCK) {
                if (s_subState_clock == 0) {
                    changeState(CLOCK, 1);
                    setTimeCounter(2);
                    if(s_ir_btn_select == -1){ s_ir_btn_select = 0; set_ir_pos_CL(0); }
                    break;
                }
                if (s_subState_clock == 1) {
                    if (s_ir_btn_select == 0) { btn_CL_alarm.click(); }
                    if (s_ir_btn_select == 1) { btn_CL_radio.click(); }
                    if (s_ir_btn_select == 2) { btn_CL_mute.click(); }
                    if (s_ir_btn_select == 3) { btn_CL_off.click(); }
                }
                break;
            }
            if (s_state == ALARMCLOCK) {
                    if (s_ir_btn_select == 0) { btn_AC_left.click(); }
                    if (s_ir_btn_select == 1) { btn_AC_right.click(); }
                    if (s_ir_btn_select == 2) { btn_AC_up.click(); }
                    if (s_ir_btn_select == 3) { btn_AC_down.click(); }
                    if (s_ir_btn_select == 4) { btn_AC_ready.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_AC(0); }
                    break;
            }
            if (s_state == SLEEPTIMER) {
                    if (s_ir_btn_select == 0) { btn_SL_up.click(); }
                    if (s_ir_btn_select == 1) { btn_SL_down.click(); }
                    if (s_ir_btn_select == 2) { btn_SL_ready.click(); }
                    if (s_ir_btn_select == 3) { btn_SL_cancel.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_SL(0); }
                    break;
            }
            if (s_state == SETTINGS) {
                    if (s_ir_btn_select == 0) { btn_SE_bright.click(); }
                    if (s_ir_btn_select == 1) { btn_SE_equal.click(); }
                    if (s_ir_btn_select == 2) { btn_SE_wifi.click(); }
                    if (s_ir_btn_select == 3) { btn_SE_radio.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_SE(0); }
                    break;
            }
            if (s_state == BRIGHTNESS) {
                    if (s_ir_btn_select == 0) { btn_BR_ready.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_BR(0); }
                    break;
            }
            if (s_state == EQUALIZER) {
                    if (s_ir_btn_select == 0) { btn_EQ_Radio.click(); }
                    if (s_ir_btn_select == 1) { btn_EQ_Player.click(); }
                    if (s_ir_btn_select == 2) { btn_EQ_mute.click(); }
                    if (s_ir_btn_select == 3) { btn_EQ_balance.click(); }
                    if (s_ir_btn_select == 4) { btn_EQ_lowPass.click(); }
                    if (s_ir_btn_select == 5) { btn_EQ_bandPass.click(); }
                    if (s_ir_btn_select == 6) { btn_EQ_highPass.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_EQ(0); }
                    break;
            }
            if (s_state == BLUETOOTH) {
                    if (s_ir_btn_select == 0) { btn_BT_volDown.click(); }
                    if (s_ir_btn_select == 1) { btn_BT_volUp.click(); }
                    if (s_ir_btn_select == 2) { btn_BT_pause.click(); }
                    if (s_ir_btn_select == 3) { btn_BT_mode.click(); }
                    if (s_ir_btn_select == 4) { btn_BT_radio.click(); }
                    if (s_ir_btn_select == 5) { btn_BT_power.click(); }
                    if(s_ir_btn_select == -1) { s_ir_btn_select = 0; set_ir_pos_BT(0); }
                    break;
            }
            break;
        case 18: // PAUSE/RESUME  --------------------------------------------------------------------------------------------------------------------
            if (s_state == PLAYER) {
                if (s_f_isFSConnected) audioPauseResumeAndUpdateState();
            }
            break;
        case 19: // STOP  ----------------------------------------------------------------------------------------------------------------------------
            if (s_state == PLAYER) {
                if (s_f_isFSConnected) audio.stopSong();
                changeState(PLAYER, 0);
            }
            break;
        case 20: // ON/OFF  --------------------------------------------------------------------------------------------------------------------------
            if (!s_f_sleeping){
                fall_asleep();
            } else {
                wake_up(RADIO, 0);
            }
            break;
        case 21: // RADIO  ---------------------------------------------------------------------------------------------------------------------------
            if (s_state != RADIO) {
                setStation(s_cur_station);
                changeState(RADIO, 0);
            }
            break;
        case 22: // PLAYER  --------------------------------------------------------------------------------------------------------------------------
            if (s_state != PLAYER) { changeState(PLAYER, 0); }
            break;
        case 23: // DLNA  ----------------------------------------------------------------------------------------------------------------------------
            if (s_state != DLNA) {
                changeState(DLNA, 0);
            }
            break;
        case 24: // CLOCK  ---------------------------------------------------------------------------------------------------------------------------
            if (s_state != CLOCK) {
                changeState(CLOCK, 1);
            }
            break;
        case 25: // OFF TIIMER  ----------------------------------------------------------------------------------------------------------------------
            if (s_state != SLEEPTIMER) {
                s_sleepTimerSubMenue = 0;
                changeState(SLEEPTIMER, 0);
            }
            break;
        case 26: // VOLUME+  -------------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                txt_RA_staName.hide();
                volBox.enable();
                upvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            }
            if (s_state == PLAYER) {
                txt_PL_fName.hide();
                volBox.enable();
                upvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == DLNA) {
                txt_DL_fName.hide();
                volBox.enable();
                upvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == CLOCK) {
                upvolume();
                setTimeCounter(2);
                break;
            } // VOLUME++
            if (s_state == SLEEPTIMER) {
                upvolume();
                setTimeCounter(2);
                break;
            } // VOLUME++
            upvolume();
            break;
        case 27: // VOLUME-  -------------------------------------------------------------------------------------------------------------------------
            if (s_state == RADIO) {
                txt_RA_staName.hide();
                volBox.enable();
                downvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == PLAYER) {
                txt_PL_fName.hide();
                volBox.enable();
                downvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == DLNA) {
                txt_DL_fName.hide();
                volBox.enable();
                downvolume();
                volBox.setNumbers(s_volume.cur_volume);
                volBox.show(TFT_BLUE);
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == CLOCK) {
                downvolume();
                setTimeCounter(2);
                break;
            } // VOLUME--
            if (s_state == SLEEPTIMER) {
                downvolume();
                setTimeCounter(2);
                break;
            } // VOLUME--
            downvolume();
            break;
        case 28: // -30sec  --------------------------------------------------------------------------------------------------------------------------
            if (s_state == PLAYER) {
                if (audio.isRunning()) audio.setTimeOffset(-30);
            }
            break;
        case 29: // +30sec  --------------------------------------------------------------------------------------------------------------------------
            if (s_state == PLAYER) {
                if (audio.isRunning()) audio.setTimeOffset(+30);
            }
            break;
        case 30: // NEXT STATION  --------------------------------------------------------------------------------------------------------------------
            nextStation();
            break;
        case 31: // PREV STATION  --------------------------------------------------------------------------------------------------------------------
            prevStation();
            break;
        default: //  ---------------------------------------------------------------------------------------------------------------------------------
            MWR_LOG_WARN("unknown IR code: {}", key);
            break;
    }
}
// clang-format on
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// Events from websrv /*🟢🟡🔴*/
// clang-format off

// Minimal standalone WiFi setup page -- deliberately not part of the big
// index_html SPA (which has no working credential-add flow, see the
// WiFi-settings feature investigation) so it works standing alone when
// broadcasting the AP fallback hotspot, reachable at any known IP (usually
// 192.168.4.1) without needing the rest of the app's JS. Network list is
// built server-side into a plain <select> (a synchronous WiFi.scanNetworks()
// at request time) rather than an async JS fetch, to keep the page trivial.
static void serveWifiSetupPage() {
    ps_ptr<char> options(2048);
    if (lockWifiOps(pdMS_TO_TICKS(15000))) {
        const int16_t n = WiFi.scanNetworks();
        for (int16_t i = 0; i < n && i < 24; ++i) {
            ps_ptr<char> ssidEsc(64);
            const char* raw = WiFi.SSID(i).c_str();
            // Escape '"' and '<' so a crafted SSID can't break out of the
            // attribute/tag it's placed into (WiFi.SSID() is untrusted input).
            size_t o = 0;
            for (const char* p = raw; *p && o < 60; ++p) {
                if (*p == '"') { strlcpy(ssidEsc.get() + o, "&quot;", 64 - o); o += 6; }
                else if (*p == '<') { strlcpy(ssidEsc.get() + o, "&lt;", 64 - o); o += 4; }
                else ssidEsc.get()[o++] = *p;
            }
            ssidEsc.get()[o] = '\0';
            char opt[160];
            snprintf(opt, sizeof(opt), "<option value=\"%s\">%s (%d dBm)</option>", ssidEsc.c_get(), ssidEsc.c_get(), static_cast<int>(WiFi.RSSI(i)));
            options += opt;
        }
        WiFi.scanDelete();
        unlockWifiOps();
    } else {
        MWR_LOG_WARN("WiFi operation busy while serving setup page");
    }

    ps_ptr<char> page(2560 + options.strlen());
    page.assignf(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>MiniWebRadio WiFi 设置</title><style>"
        "body{{font-family:sans-serif;background:#111;color:#eee;padding:20px;max-width:420px;margin:0 auto}}"
        "h2{{color:#3ddc97}}select,input,button{{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;font-size:16px;border-radius:6px;border:1px solid #444;background:#222;color:#eee}}"
        "button{{background:#3ddc97;color:#111;border:none;font-weight:bold}}"
        "</style></head><body>"
        "<h2>连接到WiFi</h2>"
        "<form onsubmit=\"return doSubmit()\">"
        "<select id=\"ssid\"><option value=\"\">-- 选择扫描到的网络 --</option>{}</select>"
        "<input id=\"ssidManual\" placeholder=\"或手动输入SSID\">"
        "<input id=\"pw\" type=\"password\" placeholder=\"密码\">"
        "<button type=\"submit\">连接</button>"
        "</form><p id=\"msg\"></p><script>"
        "function doSubmit(){{"
        "var s=document.getElementById('ssidManual').value||document.getElementById('ssid').value;"
        "var p=document.getElementById('pw').value;"
        "if(!s){{alert('请输入或选择网络名');return false;}}"
        "document.getElementById('msg').innerText='正在保存并重启设备...';"
        "fetch('wifi_add?'+encodeURIComponent(s)+'&'+encodeURIComponent(p));"
        "return false;}}"
        "</script></body></html>",
        options.c_get());
    webSrv.show(page.c_get(), webSrv.TEXT);
}

namespace {
// Minimal JSON string escaping -- SSIDs/passwords are untrusted input and
// could contain '"' or backslashes; control characters are stripped rather
// than escaped since a WiFi name containing them would be pathological.
void jsonEscapeAppend(ps_ptr<char>& out, const char* raw) {
    for (const char* p = raw; *p; ++p) {
        if (*p == '"' || *p == '\\') out += "\\";
        if (static_cast<unsigned char>(*p) < 0x20) continue;
        char one[2] = {*p, '\0'};
        out += one;
    }
}
} // namespace

// Companion to the always-available minimal /wifi setup page: a fuller,
// phone-Settings-style WiFi admin page for once the device is already on a
// network (reached via the "后台管理" QR in Settings > WiFi on the device
// itself, see HifiUi::onWifiManageOpenAction). Static HTML shell; all data
// (scan results, saved slots, connect status) comes from the JSON endpoints
// below via fetch(), so this function itself needs no per-request state.
static void serveWifiManagePage() {
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>MiniWebRadio WiFi 后台管理</title><style>"
        "body{font-family:-apple-system,sans-serif;background:#0A0B12;color:#F2F3F7;padding:16px;max-width:480px;margin:0 auto}"
        "h2{color:#C77DFF;font-size:20px;margin:18px 0 8px}"
        "h2:first-child{margin-top:0}"
        ".card{background:#161A2B;border-radius:10px;padding:12px 14px;margin-bottom:8px;display:flex;align-items:center;justify-content:space-between;gap:10px}"
        // Scan rows need more than the simple two-child .card row (name+rssi,
        // an optional password field, and a connect button all stacked) --
        // a separate block-layout class rather than forcing that into the
        // flex-row .card, which is what caused everything to overlap.
        ".scanCard{background:#161A2B;border-radius:10px;padding:10px 14px;margin-bottom:8px}"
        ".scanCard .info{display:flex;align-items:center;justify-content:space-between;gap:10px}"
        ".scanCard button{width:100%;margin-top:8px}"
        ".ssid{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
        ".sub{color:#9AA0B4;font-size:12px;flex-shrink:0}"
        ".tag{color:#C77DFF;font-size:12px;flex-shrink:0}"
        "button{background:#6D28D9;color:#F2F3F7;border:none;border-radius:8px;padding:8px 14px;font-size:14px;flex-shrink:0}"
        "button.danger{background:#3A1620;color:#E4574B}"
        "button.link{background:none;color:#9AA0B4;text-decoration:underline;padding:4px}"
        "input{width:100%;box-sizing:border-box;padding:8px 10px;margin-top:8px;border-radius:8px;border:1px solid #565C70;background:#0E1019;color:#F2F3F7;font-size:14px}"
        ".row{flex:1;min-width:0}"
        ".empty{color:#565C70;font-size:13px;padding:6px 2px}"
        "</style></head><body>"
        "<h2>当前连接</h2><div class=\"card\"><div class=\"row\"><div class=\"ssid\" id=\"curSsid\">--</div>"
        "<div class=\"sub\" id=\"curSub\"></div></div></div>"
        "<h2>附近的WiFi <button class=\"link\" onclick=\"loadScan()\">刷新</button></h2>"
        "<div id=\"scanList\" class=\"empty\">扫描中...</div>"
        "<h2>已保存的网络</h2><div id=\"savedList\" class=\"empty\">加载中...</div>"
        "<script>"
        "function esc(s){return s.replace(/[&<>\"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c];});}"
        "function loadStatus(){fetch('wifi_status_json').then(r=>r.json()).then(function(j){"
        "document.getElementById('curSsid').textContent=j.connected?j.ssid:'未连接';"
        "document.getElementById('curSub').textContent=j.connected?(j.rssi+' dBm  ·  '+j.ip):'';"
        "});}"
        "function loadSaved(){fetch('wifi_saved_json').then(r=>r.json()).then(function(list){"
        "var el=document.getElementById('savedList');"
        "if(!list.length){el.innerHTML='<div class=\"empty\">还没有保存的网络</div>';return;}"
        "el.innerHTML=list.map(function(n){"
        "return '<div class=\"card\"><div class=\"row ssid\">'+esc(n.ssid)+'</div>'"
        "+(n.isDefault?'<span class=\"tag\">出厂默认</span>':'<button class=\"danger\" onclick=\"forget(\\''+esc(n.ssid).replace(/'/g,\"\\\\'\")+'\\')\">删除</button>')"
        "+'</div>';"
        "}).join('');"
        "});}"
        "function loadScan(){"
        "document.getElementById('scanList').innerHTML='扫描中...';"
        "fetch('wifi_scan_json').then(r=>r.json()).then(function(list){"
        "var el=document.getElementById('scanList');"
        "if(!list.length){el.innerHTML='<div class=\"empty\">未找到WiFi网络</div>';return;}"
        "el.innerHTML=list.map(function(n,i){"
        "return '<div class=\"scanCard\"><div class=\"info\">'"
        "+'<div class=\"ssid\">'+esc(n.ssid)+(n.encrypted?' 🔒':'')+'</div>'"
        "+'<div class=\"sub\">'+n.rssi+' dBm</div></div>'"
        "+(n.encrypted?'<input type=\"password\" placeholder=\"密码\" id=\"pw'+i+'\">':'')"
        "+'<button onclick=\"connect(\\''+esc(n.ssid).replace(/'/g,\"\\\\'\")+'\\','+i+')\">连接</button>'"
        "+'</div>';"
        "}).join('');"
        "});}"
        "function connect(ssid,i){"
        "var pwEl=document.getElementById('pw'+i);var pw=pwEl?pwEl.value:'';"
        "fetch('wifi_connect?'+encodeURIComponent(ssid)+'&'+encodeURIComponent(pw)).then(function(){"
        "setTimeout(function(){loadStatus();loadSaved();},4000);"
        "});"
        "alert('正在连接 '+ssid+'...');"
        "}"
        "function forget(ssid){"
        "if(!confirm('删除已保存的网络 \"'+ssid+'\"？'))return;"
        "fetch('wifi_forget?'+encodeURIComponent(ssid)).then(loadSaved);"
        "}"
        "loadStatus();loadSaved();loadScan();"
        "</script></body></html>";
    webSrv.show(page, webSrv.TEXT);
}

// Phone page behind the cloud-music settings QR (Settings > 在线音乐 >
// 扫码配置): type the gateway URL + device key on the phone, tap save, and
// the board picks the result up via cloud_config_set. Mirrors the
// /wifi_manage phone page (same dark card style, same encodeURIComponent
// GET convention -> cmd?param&arg).
static void serveCloudConfigPage() {
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>在线音乐网关配置</title><style>"
        "body{font-family:-apple-system,sans-serif;background:#0A0B12;color:#F2F3F7;padding:16px;max-width:480px;margin:0 auto}"
        "h2{color:#C77DFF;font-size:20px;margin:0 0 12px}"
        ".card{background:#161A2B;border-radius:10px;padding:14px}"
        "label{display:block;color:#9AA0B4;font-size:13px;margin:10px 0 4px}"
        "input{width:100%;box-sizing:border-box;padding:8px 10px;border-radius:8px;border:1px solid #565C70;background:#0E1019;color:#F2F3F7;font-size:14px}"
        "button{width:100%;background:#6D28D9;color:#F2F3F7;border:none;border-radius:8px;padding:10px;font-size:15px;margin-top:14px}"
        "#msg{margin-top:10px;font-size:13px;color:#34D399}"
        "</style></head><body>"
        "<h2>在线音乐网关配置</h2><div class=\"card\">"
        "<label>网关地址</label><input id=\"url\" placeholder=\"https://esp32-ncm-gateway...\">"
        "<label>设备密钥</label><input id=\"key\" type=\"password\" placeholder=\"设备密钥\">"
        "<button onclick=\"save()\">保存到开发板</button><div id=\"msg\"></div>"
        "</div><script>"
        "fetch('cloud_config_json').then(r=>r.json()).then(function(j){"
        "if(j.configured){document.getElementById('url').value=j.base_url;document.getElementById('key').value=j.device_key;}"
        "});"
        "function save(){"
        "var u=document.getElementById('url').value;var k=document.getElementById('key').value;"
        "if(!u||!k){document.getElementById('msg').style.color='#E4574B';document.getElementById('msg').textContent='地址和密钥都要填';return;}"
        "document.getElementById('msg').style.color='#34D399';document.getElementById('msg').textContent='保存中…';"
        "fetch('cloud_config_set?'+encodeURIComponent(u)+'&'+encodeURIComponent(k)).then(function(r){return r.json();}).then(function(j){"
        "document.getElementById('msg').textContent=j.ok?('保存成功,请回到开发板查看'):('保存失败:'+j.message);"
        "});"
        "}"
        "</script></body></html>";
    webSrv.show(page, webSrv.TEXT);
}

static void serveCloudConfigJson() {
    const CloudMusicConfig cfg = playerCoreCloudMusicConfig();
    ps_ptr<char> urlEsc;
    ps_ptr<char> keyEsc;
    jsonEscapeAppend(urlEsc, cfg.baseUrl);
    jsonEscapeAppend(keyEsc, cfg.deviceKey);
    ps_ptr<char> json(320);
    json.assignf("{{\"configured\":{},\"base_url\":\"{}\",\"device_key\":\"{}\"}}",
                 cfg.configured ? "true" : "false", urlEsc.c_get(), keyEsc.c_get());
    webSrv.show(json.c_get(), webSrv.JSON);
}

static void serveWifiStatusJson() {
    ps_ptr<char> json(192);
    const bool connected = WiFi.status() == WL_CONNECTED;
    ps_ptr<char> ssidEsc;
    jsonEscapeAppend(ssidEsc, connected ? WiFi.SSID().c_str() : "");
    json.assignf("{{\"connected\":{},\"ssid\":\"{}\",\"rssi\":{},\"ip\":\"{}\"}}", connected ? "true" : "false", ssidEsc.c_get(),
                 connected ? static_cast<int>(WiFi.RSSI()) : 0, connected ? WiFi.localIP().toString().c_str() : "");
    webSrv.show(json.c_get(), webSrv.JSON);
}

// Synchronous scan, same as serveWifiSetupPage() -- simpler than threading
// through the LVGL side's background wifiScanTask, and this page is only
// loaded on demand (or "刷新" tapped), not polled.
static void serveWifiScanJson() {
    ps_ptr<char> json(2048);
    json = "[";
    if (lockWifiOps(pdMS_TO_TICKS(15000))) {
        const int16_t n = WiFi.scanNetworks();
        for (int16_t i = 0; i < n && i < 24; ++i) {
            ps_ptr<char> ssidEsc;
            jsonEscapeAppend(ssidEsc, WiFi.SSID(i).c_str());
            char entry[192];
            snprintf(entry, sizeof(entry), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"encrypted\":%s}", i ? "," : "", ssidEsc.c_get(),
                     static_cast<int>(WiFi.RSSI(i)), WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
            json += entry;
        }
        WiFi.scanDelete();
        unlockWifiOps();
    } else {
        MWR_LOG_WARN("WiFi operation busy while serving scan json");
    }
    json += "]";
    webSrv.show(json.c_get(), webSrv.JSON);
}

static void serveWifiSavedJson() {
    ps_ptr<char> json(1024);
    json = "[";
    bool first = true;
    for (uint8_t i = 0; i < kWifiSlotCount; ++i) {
        ps_ptr<char> line = wifiPrefGet(i);
        if (!playerCoreWifiLineIsSavedNetwork(line.c_get())) continue;
        const int pos = line.index_of("\t", 0);
        line[pos] = '\0';
        ps_ptr<char> ssidEsc;
        jsonEscapeAppend(ssidEsc, line.get());
        char entry[160];
        snprintf(entry, sizeof(entry), "%s{\"index\":%u,\"ssid\":\"%s\",\"isDefault\":%s}", first ? "" : ",", i, ssidEsc.c_get(), i == 0 ? "true" : "false");
        json += entry;
        first = false;
    }
    json += "]";
    webSrv.show(json.c_get(), webSrv.JSON);
}

void WEBSRV_onCommand(ps_ptr<char> cmd, ps_ptr<char> param, ps_ptr<char> arg){  // called via html

    if(CORE_DEBUG_LEVEL == ARDUHAL_LOG_LEVEL_DEBUG){
        printfln(s_tag.webserver, "WS_onCmd: " ANSI_ESC_YELLOW "cmd=\"{}\", params=\"{}\", arg=\"{}\"", cmd.c_get(), param.c_get(), arg.c_get());
    }
    #define CMD_EQUALS(x) if(cmd.equals(x) == true)

    CMD_EQUALS("ping"){                 webSrv.send("pong"); return;}                                                                                     // via websocket

    CMD_EQUALS("index.html"){           printfln(s_tag.webserver, "Webpage: " ANSI_ESC_ORANGE "index.html");                                                     // via XMLHttpRequest
                                        // While broadcasting the AP setup hotspot, serve the WiFi setup
                                        // page for the default path too -- most phones request "/" (or a
                                        // captive-portal probe path that falls through to it) right after
                                        // joining, and the normal full SPA is useless with no real network.
                                        if (s_wifiApFallbackActive) { serveWifiSetupPage(); return; }
                                        webSrv.show(index_html, webSrv.TEXT);
                                        return;}

    CMD_EQUALS("wifi"){                 printfln(s_tag.webserver, "Webpage: " ANSI_ESC_ORANGE "wifi setup");
                                        serveWifiSetupPage();
                                        return;}

    CMD_EQUALS("wifi_add"){             // param=SSID, arg=password (see serveWifiSetupPage()'s fetch() call)
                                        setWiFiCredentials(param, arg);
                                        printfln(s_tag.wifi_info, ANSI_ESC_GREEN "WiFi credentials added via setup page for \"{}\", restarting", param.c_get());
                                        webSrv.show("OK, restarting...", webSrv.TEXT);
                                        vTaskDelay(pdMS_TO_TICKS(1500));
                                        ESP.restart();
                                        return;}

    CMD_EQUALS("wifi_manage"){          printfln(s_tag.webserver, "Webpage: " ANSI_ESC_ORANGE "wifi manage");
                                        serveWifiManagePage();
                                        return;}

    CMD_EQUALS("cloud_config"){         printfln(s_tag.webserver, "Webpage: " ANSI_ESC_ORANGE "cloud config (phone)");
                                        serveCloudConfigPage();
                                        return;}
    CMD_EQUALS("cloud_config_json"){    serveCloudConfigJson(); return;}
    CMD_EQUALS("cloud_config_set"){     printfln(s_tag.webserver, "cloud config set via phone page");
                                        const bool ok = playerCoreSetCloudMusicConfig(param.c_get(), arg.c_get());
                                        if (ok) playerCoreCloudMusicWakeStart();
                                        ps_ptr<char> json(192);
                                        json.assignf("{{\"ok\":{},\"message\":\"{}\"}}", ok ? "true" : "false",
                                                     ok ? "saved" : "invalid config (need http(s) URL + key)");
                                        webSrv.show(json.c_get(), webSrv.JSON);
                                        return;}
    CMD_EQUALS("cloud_diag_start"){     // Diagnostics: replicate the wake path (DNS->TCP443->TLS health) on a
                                        // dedicated 24K-stack task and stash the JSON result.
                                        playerCoreCloudDiagStart();
                                        webSrv.show("{\"started\":true}", webSrv.JSON);
                                        return;}
    CMD_EQUALS("cloud_diag_json"){      if (playerCoreCloudDiagReady()) webSrv.show(playerCoreCloudDiagResult(), webSrv.JSON);
                                        else webSrv.show("{\"ready\":false}", webSrv.JSON);
                                        return;}

    CMD_EQUALS("wifi_status_json"){     serveWifiStatusJson(); return;}

    CMD_EQUALS("wifi_scan_json"){       serveWifiScanJson(); return;}

    CMD_EQUALS("wifi_saved_json"){      serveWifiSavedJson(); return;}

    CMD_EQUALS("wifi_connect"){         // param=SSID, arg=password -- like wifi_add but live (no restart),
                                        // same background reconnect path as the on-device Settings > WiFi flow.
                                        printfln(s_tag.wifi_info, ANSI_ESC_GREEN "WiFi connect requested via admin page for \"{}\"", param.c_get());
                                        playerCoreWifiAddNetwork(param.c_get(), arg.c_get());
                                        webSrv.show("{\"started\":true}", webSrv.JSON);
                                        return;}

    CMD_EQUALS("wifi_forget"){          // param=SSID -- clears that saved slot (setWiFiCredentials() with an
                                        // empty password deletes; slot 0's hardcoded default is protected there too).
                                        setWiFiCredentials(param, "");
                                        printfln(s_tag.wifi_info, ANSI_ESC_YELLOW "WiFi credentials forgotten via admin page for \"{}\"", param.c_get());
                                        webSrv.show("{\"ok\":true}", webSrv.JSON);
                                        return;}

    CMD_EQUALS("index.js"){             printfln(s_tag.webserver, "Script: " ANSI_ESC_ORANGE "index.js");                                                       // via XMLHttpRequest
                                        webSrv.show(index_js, webSrv.JS); return;}

    CMD_EQUALS("favicon.ico"){          webSrv.streamfile(SD_MMC, "/favicon.ico"); return;}                                                               // via XMLHttpRequest

    CMD_EQUALS("test"){                 ps_ptr<char>p; p.assignf("free heap: {}, Inbuff filled: {}, Inbuff free: {}, PSRAM filled {}, PSRAM free {},",
                                        ESP.getFreeHeap(), audio.inBufferFilled(), audio.inBufferFree(), (ESP.getPsramSize() - ESP.getFreePsram()), ESP.getFreePsram());
                                        webSrv.send("test=", p.c_get());
                                        printfln(s_tag.webserver, "audiotask .. stackHighWaterMark: {} bytes", (long unsigned)audio.getHighWatermark() * 4);
                                        printfln(s_tag.webserver, "looptask ... stackHighWaterMark: {} bytes", (long unsigned)uxTaskGetStackHighWaterMark(NULL) * 4);
                                        return;}

    CMD_EQUALS("get_mute"){             s_f_mute == true ? webSrv.send("mute=", "1") : webSrv.send("mute=", "0"); return;}
    CMD_EQUALS("set_mute"){             muteChanged(!s_f_mute); return;}
    CMD_EQUALS("upvolume"){             webSrv.send("volume=", int2str(upvolume()));  return;}                                                            // via websocket
    CMD_EQUALS("downvolume"){           webSrv.send("volume=", int2str(downvolume())); return;}                                                           // via websocket
    CMD_EQUALS("get_volumeSteps"){      webSrv.send("volumeSteps=", int2str(s_volume.volumeSteps)); return;}

    CMD_EQUALS("set_volumeSteps"){      s_volume.cur_volume = map_l(s_volume.cur_volume, 0, s_volume.volumeSteps, 0, param.to_uint32());
                                        s_volume.ringVolume = map_l(s_volume.ringVolume, 0, s_volume.volumeSteps, 0, param.to_uint32()); webSrv.send("ringVolume=", int2str(s_volume.ringVolume));
                                        s_volume.volumeAfterAlarm = map_l(s_volume.volumeAfterAlarm, 0, s_volume.volumeSteps, 0, param.to_uint32()); webSrv.send("volAfterAlarm=", int2str(s_volume.volumeAfterAlarm));
                                        s_volume.volumeSteps = param.to_uint32(); webSrv.send("volumeSteps=", param.c_get()); audio.setVolumeSteps(s_volume.volumeSteps);
                                        MWR_LOG_DEBUG("s_volumeSteps  {}", s_volume.volumeSteps);
                                        sdr_CL_volume.setMinMaxVal(0, s_volume.volumeSteps);
                                        sdr_DL_volume.setMinMaxVal(0, s_volume.volumeSteps);
                                        sdr_PL_volume.setMinMaxVal(0, s_volume.volumeSteps);
                                        sdr_RA_volume.setMinMaxVal(0, s_volume.volumeSteps);
                                        setVolume(s_volume.cur_volume);
                                        printfln(s_tag.webserver, "new volume steps: " ANSI_ESC_CYAN "{}", s_volume.volumeSteps);
                                        return;}

    CMD_EQUALS("get_ringVolume"){       webSrv.send("ringVolume=", int2str(s_volume.ringVolume)); return;}
    CMD_EQUALS("set_ringVolume"){       s_volume.ringVolume = param.to_int32(); webSrv.send("ringVolume=", int2str(s_volume.ringVolume));
                                        printfln(s_tag.webserver, "new ring volume: " ANSI_ESC_CYAN "{}", s_volume.ringVolume); return;}

    CMD_EQUALS("get_volAfterAlarm"){    webSrv.send("volAfterAlarm=", int2str(s_volume.volumeAfterAlarm)); return;}
    CMD_EQUALS("set_volAfterAlarm"){    s_volume.volumeAfterAlarm = param.to_int32(); webSrv.send("volAfterAlarm=", int2str(s_volume.volumeAfterAlarm));
                                        printfln(s_tag.webserver, "new volume after alarm: " ANSI_ESC_CYAN "{}", s_volume.volumeAfterAlarm); return;}
    CMD_EQUALS("homepage"){             webSrv.send("homepage=", s_homepage.c_get()); return;}

    CMD_EQUALS("to_listen"){            webSrv_send_station_items(); return;}   // via websocket, return the name and number of the current station
    CMD_EQUALS("get_tone"){             webSrv.send("settone=", getI2STone().c_get()); return;}

    CMD_EQUALS("get_streamtitle"){      webSrv.reply(s_streamTitle.c_get(), webSrv.TEXT); return;}

    CMD_EQUALS("LowPass"){              s_tone.LP = param.to_int32(); sdr_EQ_lowPass.setValue(s_tone.LP); // audioI2S tone
                                        ps_ptr<char>lp; lp = "Lowpass set to " + param  + "dB";
                                        setI2STone(); return;}

    CMD_EQUALS("BandPass"){             s_tone.BP = param.to_int32(); sdr_EQ_bandPass.setValue(s_tone.BP); // audioI2S tone
                                        ps_ptr<char>bp; bp = "Bandpass set to " + param + "dB";
                                        setI2STone(); return;}

    CMD_EQUALS("HighPass"){             s_tone.HP = param.to_int32(); sdr_EQ_highPass.setValue(s_tone.HP); // audioI2S tone
                                        ps_ptr<char> hp; hp = "Highpass set to " + param + "dB";
                                        setI2STone(); return;}

    CMD_EQUALS("Balance"){              s_tone.BAL = param.to_int32(); sdr_EQ_balance.setValue(s_tone.BAL); // audioI2S tone
                                        ps_ptr<char> bal = "Balance set to " + param;
                                        setI2STone(); return;}

    CMD_EQUALS("prev_station"){         prevFavStation(); return;}                                                                                           // via websocket

    CMD_EQUALS("next_station"){         nextFavStation(); return;}                                                                                           // via websocket

    CMD_EQUALS("set_station"){          uint16_t staNr = param.to_uint32(); if(staNr != s_cur_station) setStationByNumber(staNr); return;}                                                                      // via websocket

    CMD_EQUALS("stationURL"){           playerCorePlayRadioUrl(param.c_get());                                                                        // via websocket
                                        printfln(s_tag.webserver, "StationURL: " ANSI_ESC_MAGENTA "{}", param.c_get());
                                        return;}

    CMD_EQUALS("webFileURL"){           audio.connecttohost(param.c_get())? changeState(PLAYER, 1) : changeState(PLAYER, 0); return;}                        // via websocket

    CMD_EQUALS("get_networks"){         webSrv.send("networks=", WiFi.SSID().c_str()); return;}                                                              // via websocket

    CMD_EQUALS("get_tftSize"){          webSrv.send("tftSize=",displayConfig.tftSize); return;};

    CMD_EQUALS("get_timeZones"){        webSrv.send("timezones=", timezones_json); return;}

    CMD_EQUALS("set_timeZone"){         s_TZName = param;  s_TZString = arg.c_get();
                                        printfln(s_tag.webserver, "Timezone: .. " ANSI_ESC_BLUE "{}, {}", param.c_get(), arg.c_get());
                                        setRTC(s_TZString);
                                        updateSettings(); // write new TZ items to settings.json
                                        return;}

    CMD_EQUALS("get_timeZoneName"){     webSrv.reply(s_TZName, webSrv.TEXT); return;}

    CMD_EQUALS("change_state"){         if     (!strcmp(param.c_get(), "RADIO")       && s_state != RADIO)       { changeState(RADIO, 0); return; }
                                        else if(!strcmp(param.c_get(), "PLAYER")      && s_state != PLAYER)      { stopSong(); changeState(PLAYER, 0); return; }
                                        else if(!strcmp(param.c_get(), "DLNA")        && s_state != DLNA)        { stopSong(); changeState(DLNA, 0);   return; }
                                        else if(!strcmp(param.c_get(), "BLUETOOTH")   && s_state != BLUETOOTH)   { changeState(BLUETOOTH, 0); return; }
                                        else if(!strcmp(param.c_get(), "IR_SETTINGS") && s_state != IR_SETTINGS) { changeState(IR_SETTINGS, 0); return; }
                                        else return; }

    CMD_EQUALS("stopfile"){             if(!s_f_isFSConnected && !s_f_isWebConnected) {webSrv.send("resumefile=", "There is no audio file active"); return;}
                                        stopSong(); changeState(PLAYER, 0); webSrv.send("stopfile=", "audiofile stopped");
                                        return;}

    CMD_EQUALS("pause_resume"){         if(!s_f_isFSConnected && !s_f_isWebConnected) {webSrv.send("resumefile=", "There is no audio file active"); return;}
                                        audioPauseResumeAndUpdateState();
                                        if(audio.isRunning()){webSrv.send("resumefile=", "audiofile resumed"); btn_PL_pause.setOff(); btn_PL_pause.show();}
                                        else {                webSrv.send("resumefile=", "audiofile paused");  btn_PL_pause.setOn(); btn_PL_pause.show();}
                                        return;}

    CMD_EQUALS("get_alarmdays"){        webSrv.send("alarmdays=", s_alarmdays); return;}

    CMD_EQUALS("set_alarmdays"){        s_alarmdays = param.to_uint32(); updateSettings(); return;}

    CMD_EQUALS("get_alarmtime"){        return;} // not used yet

    CMD_EQUALS("set_alarmtime"){        return;}

    CMD_EQUALS("get_timeAnnouncement"){ if(s_f_timeAnnouncement) webSrv.send("timeAnnouncement=", "1");
                                        if(  !s_f_timeAnnouncement) webSrv.send("timeAnnouncement=", "0");
                                        return;}

    CMD_EQUALS("set_timeAnnouncement"){ if(param == "true" ) {s_f_timeAnnouncement = true;}
                                        if(   param == "false") {s_f_timeAnnouncement = false;}
                                        printfln(s_tag.webserver, "Timespeech, hourly time announcement is " ANSI_ESC_YELLOW "{}", (s_f_timeAnnouncement == 1) ? "on" : "off");
                                        return;}

    CMD_EQUALS("get_timeSpeechLang"){   webSrv.send("get_timeSpeechLang=", s_timeSpeechLang); printfln(s_tag.webserver, "Timespeech language is " ANSI_ESC_YELLOW "{}", s_timeSpeechLang.c_get()); return;}

    CMD_EQUALS("set_timeSpeechLang"){   if(param.strlen() > 2){MWR_LOG_ERROR("set_timeSpeechLang too long {}", param.c_get()); return;}
                                        s_timeSpeechLang = param;
                                        printfln(s_tag.webserver, "Timespeech, set language " ANSI_ESC_YELLOW "{}", param.c_get());
                                        return;}

    CMD_EQUALS("DLNA_getServer")  {     webSrv.send("DLNA_Names=", dlna.stringifyServer()); s_currDLNAsrvNr = -1; return;}

    CMD_EQUALS("DLNA_getRoot")    {     s_currDLNAsrvNr = param.to_uint32(); dlna.browseServer(s_currDLNAsrvNr, "0"); return;}

    CMD_EQUALS("DLNA_getContent") {     if(param.starts_with("http")) {connecttohost(param.c_get()); showPlayerFileName(arg.c_get()); return;}
                                        s_dlnaHistory[s_dlnaLevel].objId = param;
                                        s_totalNumberReturned = 0;
                                        dlna.browseServer(s_currDLNAsrvNr, s_dlnaHistory[s_dlnaLevel].objId.c_get());
                                        return;}

    CMD_EQUALS("SD/"){                  param = scaleImage(param); if(!SD_MMC.exists(param.c_get())) param = scaleImage("/common/unknown.png");
                                        if(!webSrv.streamfile(SD_MMC, param)){ printfln(s_tag.webserver, "The file could not be transferred " ANSI_ESC_RED "\"{}\"", param.get()); } // via XMLHttpRequest
                                        return;}

    CMD_EQUALS("SD_Download"){          webSrv.streamfile(SD_MMC, param.c_get());                                                                         // via XMLHttpRequest
                                        printfln(s_tag.webserver, "Load from SD  " ANSI_ESC_YELLOW "\"{}\"", param.c_get());
                                        return;}

    CMD_EQUALS("SD_GetFolder"){         webSrv.reply(s_SD_content.stringifyDirContent(param), webSrv.JS);                                                           // via XMLHttpRequest
                                        printfln(s_tag.webserver, "GetFolder " ANSI_ESC_YELLOW "\"{}\"", param.c_get());
                                        return;}

    CMD_EQUALS("SD_newFolder"){         bool res = SD_newFolder(param.c_get());                                                                           // via XMLHttpRequest
                                        if(res) webSrv.sendStatus(200); else webSrv.sendStatus(400);
                                        printfln(s_tag.webserver, "NewFolder " ANSI_ESC_YELLOW "\"{}\"", param.c_get());
                                        return;}

    CMD_EQUALS("SD_playFile"){          stopSong();
                                        webSrv.reply("SD_playFile=" + param, webSrv.TEXT);                                                                // via XMLHttpRequest
                                        printfln(s_tag.webserver, "Play " ANSI_ESC_YELLOW "\"{}\"", param.c_get());
                                        SD_playFile(param.c_get());
                                        return;}

    CMD_EQUALS("SD_playAllFiles"){      stopSong();
                                        webSrv.send("SD_playFolder=", param);                                                                                      // via websocket
                                        printfln(s_tag.webserver, "Play Folder" ANSI_ESC_YELLOW "\"{}\"", param.c_get());
                                        if(playlist.create_playlist_from_SD_folder(param)){
                                            s_f_playlistEnabled = true;
                                            s_subState_player = 1;
                                        }
                                        return;}

    CMD_EQUALS("SD_rename"){            ps_ptr<char> _arg = arg.substr(0, arg.index_of("&")); // only the first argument is used                              // via XMLHttpRequest
                                        printfln(s_tag.webserver, "Rename " ANSI_ESC_YELLOW "old \"{}\" new \"%s\"",
                                        param.c_get(), _arg.c_get());
                                        bool res = SD_rename(param.c_get(), _arg.c_get());
                                        if(res) webSrv.reply("refresh", webSrv.TEXT);
                                        else webSrv.sendStatus(400);
                                        return;}

    CMD_EQUALS("set_IRcmd"){            int32_t command = param.to_int32(16);
                                        int32_t btnNr = arg.to_int32(10);
                                        printfln(s_tag.webserver, "set_IR_cmd: " ANSI_ESC_YELLOW "IR command " ANSI_ESC_CYAN "0x{:02X}, " ANSI_ESC_YELLOW "IR Button Number " ANSI_ESC_CYAN "{:02}", command, btnNr);
                                        ir.set_irButtons(btnNr,  command);
                                        s_settings.irbuttons[btnNr].val = command;
                                        return;}

    CMD_EQUALS("set_IRaddr"){           printfln(s_tag.webserver, "set_IR_addr: " ANSI_ESC_CYAN "{}", param.c_get());
                                        int32_t address = (int32_t)strtol(param.c_get(), NULL, 16);
                                        ir.set_irAddress(address);
                                        s_settings.irbuttons[42].val = address;
                                        return;}

    CMD_EQUALS("get_sleepMode"){        webSrv.send("sleepMode=", s_sleepMode); return;}

    CMD_EQUALS("set_sleepMode"){        s_sleepMode = param.to_uint32();
                                        if(s_sleepMode == 0) printfln(s_tag.webserver, "SleepMode: " ANSI_ESC_YELLOW "Display off");
                                        if(s_sleepMode == 1) printfln(s_tag.webserver, "SleepMode: " ANSI_ESC_YELLOW "Show the time");
                                        return;}

    CMD_EQUALS("DLNA_GetFolder"){       webSrv.sendStatus(306); return;}  // todo
    CMD_EQUALS("KCX_BT_connected") {    if(!bt_emitter.get_power_state()) webSrv.send("KCX_BT_connected=", "-1");
                                        else if(bt_emitter.isConnected()) webSrv.send("KCX_BT_connected=",  "1");
                                        else                              webSrv.send("KCX_BT_connected=",  "0");
                                        return;}
    CMD_EQUALS("KCX_BT_clearItems"){    bt_emitter.deleteVMlinks(); return;}
    CMD_EQUALS("KCX_BT_addName"){       bt_emitter.addLinkName(param.c_get()); return;}
    CMD_EQUALS("KCX_BT_addAddr"){       bt_emitter.addLinkAddr(param.c_get()); return;}
    CMD_EQUALS("KCX_BT_mem"){           bt_emitter.getVMlinks(); return;}
    CMD_EQUALS("KCX_BT_scanned"){       webSrv.send("KCX_BT_SCANNED=", bt_emitter.stringifyScannedItems()); return;}
    CMD_EQUALS("KCX_BT_getMode"){       webSrv.send("KCX_BT_MODE=", bt_emitter.getMode().c_get()); return;}
    CMD_EQUALS("KCX_BT_changeMode"){    if(s_bt_emitter.mode.equals("RX")) s_bt_emitter.mode = "TX"; else  s_bt_emitter.mode = "RX"; return;}
    CMD_EQUALS("KCX_BT_pause"){         bt_emitter.pauseResume(); return;}
    CMD_EQUALS("KCX_BT_downvolume"){    bt_emitter.downvolume(); return;}
    CMD_EQUALS("KCX_BT_upvolume"){      bt_emitter.upvolume();   return;}
    CMD_EQUALS("KCX_BT_getPower"){      bt_emitter.get_power_state() ? webSrv.send("KCX_BT_power=", "1") : webSrv.send("KCX_BT_power=", "0"); return;}
    CMD_EQUALS("KCX_BT_power"){         s_bt_emitter.enabled = !s_bt_emitter.enabled ; return;}

    CMD_EQUALS("hardcopy"){             printfln(s_tag.webserver, "Webpage: " ANSI_ESC_YELLOW "create a display hardcopy"); make_hardcopy_on_sd(); webSrv.send("hardcopy=", "/hardcopy.bmp"); return;}

    printfln(s_tag.webserver, ANSI_ESC_RED "unknown HTMLcommand {}, param={}", cmd, param.c_get());
    webSrv.sendStatus(400);
}
// clang-format on
/*🟢🟡🔴*/
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// POST Events from websrv /*🟢🟡🔴*/
// clang-format off

void WEBSRV_onRequest(const char* cmd,  const char* param, const char* arg, const char* contentType, uint32_t contentLength){
    MWR_LOG_DEBUG("cmd {}, param {}, arg {}, ct {}, cl {}", cmd, param, arg, contentType, contentLength);
    if(strcmp(cmd, "SD_Upload") == 0) {savefile(param, contentLength, contentType); // PC --> SD
                                       if(strcmp(param, "/stations.json") == 0) staMgnt.updateStationsList();
                                       return;}

    if(strcmp(cmd, "upload_player2sd") == 0) {savefile(param, contentLength, contentType); return; }
    if(strcmp(cmd, "upload_text_file") == 0) {savefile(param, contentLength, contentType); return; }
    if(strcmp(cmd, "uploadfile") == 0){saveImage(param, contentLength); return;}
    printfln(s_tag.webserver, ANSI_ESC_RED "unknown HTMLcommand {}, param={}", cmd, param);
    webSrv.sendStatus(400);
}

void WEBSRV_onDelete(const char* cmd,  const char* param, const char* arg){  // via XMLHttpRequest
    if(startsWith(cmd, "SD")){      bool res = SD_delete(param);
                                    if(res) webSrv.sendStatus(200); else webSrv.sendStatus(400);
                                    printfln(s_tag.webserver, "Delete " ANSI_ESC_ORANGE "\"{}\"", param);
                                    return;}
    printfln(s_tag.webserver, ANSI_ESC_RED "unknown HTMLcommand {}, param={}", cmd, param);
    webSrv.sendStatus(400);
}
// clang-format on
/*🟢🟡🔴*/
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
//  Events from DLNA
void on_dlna_client(const DLNA_Client::msg_s& msg) {
    if (msg.e == DLNA_Client::evt_content) {
        if (!msg.items) return; // security check
        for (size_t i = 0; i < msg.items->size(); i++) {
            const auto& item = msg.items->at(i);
            if (item.isAudio) {
                if (item.duration.equals("?") != 0) { // no duration given
                    if (item.itemSize) {
                        printfln(s_tag.dlna_server, "title " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET ", itemSize " ANSI_ESC_CYAN "{}", item.title, item.itemSize);
                    } else {
                        printfln(s_tag.dlna_server, "title " ANSI_ESC_YELLOW "{}", item.title.c_get());
                    }
                } else {
                    printfln(s_tag.dlna_server,
                             "title " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET ", duration "
                             "{}",
                             item.title, item.duration);
                }
            }
            if (item.childCount) {
                printfln(s_tag.dlna_server, "title " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET ", childCount " ANSI_ESC_CYAN "{}", item.title, item.childCount);
            } else {
                printfln(s_tag.dlna_server, "title " ANSI_ESC_YELLOW "{}" ANSI_ESC_RESET ", childCount " ANSI_ESC_CYAN "{}", item.title, 0);
            }
        }
        if (msg.totalMatches >= 0) printfln(s_tag.dlna_server, "returned " ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " from " ANSI_ESC_CYAN "{}", msg.numberReturned, msg.totalMatches);
        s_dlnaMaxItems = msg.totalMatches;
        s_totalNumberReturned += msg.numberReturned;
        if (msg.numberReturned == 50 && !s_f_dlnaMakePlaylistOTF) { // next round
            if (s_totalNumberReturned < msg.totalMatches && s_totalNumberReturned < 500) { s_f_dlnaBrowseServer = true; }
        }
        if (s_f_dlnaWaitForResponse) {
            s_f_dlnaWaitForResponse = false;
            lst_DLNA.show(s_dlnaItemNr, dlna.getServer(), dlna.getBrowseResult(), &s_dlnaLevel, s_dlnaMaxItems, s_dlnaMaXServers);
            setTimeCounter(LIST_TIMER);
        } else {
            webSrv.send("dlnaContent=", dlna.stringifyContent());
        }
        if (s_totalNumberReturned == msg.totalMatches || s_totalNumberReturned == 500 || s_f_dlnaMakePlaylistOTF) {
            s_totalNumberReturned = 0;
            s_f_dlna_browseReady = true; // last item received
        }
    }
    if (msg.e == DLNA_Client::evt_server) {
        for (size_t i = 0; i < msg.server->size(); i++) {
            const auto& server = msg.server->at(i);
            printfln(s_tag.dlna_server, "[{}] " ANSI_ESC_ORANGE "{}:{} " ANSI_ESC_YELLOW " {}", i, server.ip, server.port, server.friendlyName);
        }
        s_dlnaMaXServers = msg.server->size();
        printfln(s_tag.dlna_server, ANSI_ESC_CYAN "{}" ANSI_ESC_RESET " media server found", msg.server->size());
    }
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void on_kcx_bt_emitter(const KCX_BT_Emitter::msg_s& msg) {
    if (msg.e == KCX_BT_Emitter::evt_info) { printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_GREEN "{}", msg.arg); }
    if (msg.e == KCX_BT_Emitter::evt_found) {
        s_bt_emitter.found = true;
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "KCX_BT_Emitter found");
        bt_emitter.userCommand("AT+GMR?");                 // get version
        bt_emitter.userCommand("AT+PAUSE?");               // pause or play?
        bt_emitter.userCommand("AT+NAME+BT-MiniWebRadio"); // set BT receiver name
        bt_emitter.setVolume(s_bt_emitter.volume);
    }
    if (msg.e == KCX_BT_Emitter::evt_connect) {
        s_bt_emitter.connect = true;
        if (s_bt_emitter.mode.equals("TX")) {
            txt_BT_mode.setText("EMITTER");
            pic_BT_mode.setPicturePath("/common/BT_TX.png");
            if (s_state == BLUETOOTH) {
                pic_BT_mode.show();
                txt_BT_mode.show();
            }
            webSrv.send("KCX_BT_MODE=", "TX");
        } else {
            txt_BT_mode.setText("RECEIVER");
            pic_BT_mode.setPicturePath("/common/BT_RX.png");
            if (s_state == BLUETOOTH) {
                pic_BT_mode.show();
                txt_BT_mode.show();
            }
            webSrv.send("KCX_BT_MODE=", "RX");
        }
        webSrv.send("KCX_BT_connected=", "1");
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "connected");
    }
    if (msg.e == KCX_BT_Emitter::evt_disconnect) {
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "disconnected");
        pic_BT_mode.setPicturePath("/common/BTnc.png"); // not connected
        if (s_state == BLUETOOTH) pic_BT_mode.show();
        webSrv.send("KCX_BT_connected=", "0");
    }
    if (msg.e == KCX_BT_Emitter::evt_reset) {
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "reset");
        s_bt_emitter.connect = false;
    }
    if (msg.e == KCX_BT_Emitter::evt_power_on) {
        webSrv.send("KCX_BT_power=", "1");
        webSrv.send("KCX_BT_connected=", "0");
        btn_BT_power.setValue(true);
        pic_BT_mode.setPicturePath("/common/BTnc.png");
        if (s_state == BLUETOOTH) pic_BT_mode.show();
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "power on");
        bt_emitter.userCommand("AT+BT_MODE?");
    }
    if (msg.e == KCX_BT_Emitter::evt_power_off) {
        webSrv.send("KCX_BT_power=", "0");
        webSrv.send("KCX_BT_connected=", "-1");
        btn_BT_power.setValue(false);
        pic_BT_mode.setPicturePath("/common/BToff.png");
        if (s_state == BLUETOOTH) pic_BT_mode.show();
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "power off");
    }
    if (msg.e == KCX_BT_Emitter::evt_scan) {
        s_bt_emitter.connect = false;
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "scan...");
    }
    if (msg.e == KCX_BT_Emitter::evt_volume) {
        s_bt_emitter.volume = msg.val;
        ps_ptr<char> v;
        v.assignf("Vol: {:02}", s_bt_emitter.volume);
        if (s_state == BLUETOOTH) dispFooter.updateFileNr(v);
        printfln(s_tag.bt_emitter, "Volume: " ANSI_ESC_CYAN "{}", msg.val);
    }
    if (msg.e == KCX_BT_Emitter::evt_version) {
        s_bt_emitter.version = msg.arg;
        printfln(s_tag.bt_emitter, "Version: " ANSI_ESC_CYAN "{}", msg.arg);
    }
    if (msg.e == KCX_BT_Emitter::evt_mode) {
        webSrv.send("KCX_BT_MODE=", msg.arg);
        if (s_state == BLUETOOTH) {
            txt_BT_mode.setText(msg.arg[0] == 'R' ? "RECEIVER" : "EMITTER");
            txt_BT_mode.show();
        }
        s_bt_emitter.mode = msg.arg;
        printfln(s_tag.bt_emitter, "RX_TX_mode: " ANSI_ESC_YELLOW "{}", msg.arg);
    }
    if (msg.e == KCX_BT_Emitter::evt_pause) {
        s_bt_emitter.play = false;
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "pause");
    }
    if (msg.e == KCX_BT_Emitter::evt_play) {
        s_bt_emitter.play = true;
        printfln(s_tag.bt_emitter, "Info: " ANSI_ESC_YELLOW "play");
    }
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void on_websrv(const WebSrv::msg_s& msg) {
    if (msg.e == WebSrv::evt_info) {
        if (msg.arg.starts_with("WebSocket")) return;      // suppress WebSocket client available
        if (msg.arg.starts_with("ping")) return;           // suppress ping
        if (msg.arg.starts_with("to_listen")) return;      // suppress to_isten
        if (msg.arg.starts_with("Command client")) return; // suppress Command client available
        if (msg.arg.starts_with("test=")) return;          // suppress stackHighWaterMark
        if (msg.arg.starts_with("get_")) return;           // suppress all getters
        if (msg.arg.starts_with("set_")) return;           // suppress all setters
        if (msg.arg.starts_with("SD")) return;             // suppress all SD commands
        if (msg.arg.starts_with("Length")) return;         // suppress all file length infos
        printfln(s_tag.webserver, ANSI_ESC_GREEN "{} ", msg.arg.c_get());
    }
    if (msg.e == WebSrv::evt_error) { printfln(s_tag.webserver, ANSI_ESC_RED "{}", msg.arg); }
    if (msg.e == WebSrv::evt_warn) { printfln(s_tag.webserver, ANSI_ESC_YELLOW "{}", msg.arg); }
    if (msg.e == WebSrv::evt_command) { WEBSRV_onCommand(msg.cmd, msg.param1, msg.arg1); }
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void kcx_bt_memItems(const char* jsonItems) { // Every time an item (name or address) was added, a JSON string is passed here
    // printfln(s_tag.bt_emitter, "bt_memItems {}", jsonItems);
    webSrv.send("KCX_BT_MEM=", jsonItems);
}

void kcx_bt_scanItems(const char* jsonItems) { // Every time an item (name and address) was scanned, a JSON string is passed here
    // printfln(s_tag.bt_emitter, "bt_scanItems {}", jsonItems);
    webSrv.send("KCX_BT_SCANNED=", jsonItems);
}

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
//  📌📌📌         T O U C H            📌📌📌
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// clang-format off
/*🟢🟡🔴*/
void tp_pressed(uint16_t x, uint16_t y) {
    // printfln(s_tag.tp_info, "Touchpoint x={}, y={}", x, y);
    if (s_f_sleeping) return; // awake in tp_released()
    const char* objName = NULL;
    if(y > layout.winHeader.y + layout.winHeader.h && y < layout.winProgbar.y) {
        objName = "backpane";
        if (s_state == RADIO){
            changeState(RADIO, s_subState_radio + 1 == 3 ? 0 : s_subState_radio + 1);
            goto exit;
        }
        if (s_state == CLOCK){
            changeState(CLOCK, s_subState_clock + 1 == 2 ? 0 : s_subState_clock + 1);
            goto exit;
        }
    }
    objName = isObjectClicked(x, y);
exit:
    if (objName) { printfln(s_tag.tp_info, "click on ..  {}", objName); }
    return;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void tp_long_pressed(uint16_t x, uint16_t y) {

    if (s_state == DLNAITEMSLIST) {
        //    lst_DLNA.longPressed(x, y);
    }
    MWR_LOG_INFO("tp long pressed");
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void tp_moved(uint16_t x, uint16_t y) {
    if (s_state == RADIO)          { if (sdr_RA_volume.positionXY(x, y))     return; }
    if (s_state == STATIONSLIST)   { if (lst_RADIO.positionXY(x, y))         return; }
    if (s_state == PLAYER)         { if (sdr_PL_volume.positionXY(x, y))     return; }
    if (s_state == AUDIOFILESLIST) { if (lst_PLAYER.positionXY(x, y))        return; }
    if (s_state == DLNA)           { if (sdr_DL_volume.positionXY(x, y))     return; }
    if (s_state == CLOCK)          { if (sdr_CL_volume.positionXY(x, y))     return; }
    if (s_state == DLNAITEMSLIST)  { if (lst_DLNA.positionXY(x, y))          return; }
    if (s_state == BRIGHTNESS)     { if (sdr_BR_value.positionXY(x, y))      return; }
    if (s_state == EQUALIZER)      { if (sdr_EQ_lowPass.positionXY(x, y))  { return; }
                                     if (sdr_EQ_bandPass.positionXY(x, y)) { return; }
                                     if (sdr_EQ_highPass.positionXY(x, y)) { return; }
                                     if (sdr_EQ_balance.positionXY(x, y))  { return; }}
    if (s_state == IR_SETTINGS)    { if (btn_IR_radio.positionXY(x, y))      return; }
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void tp_released(uint16_t x, uint16_t y){

    if(s_f_sleeping && x > 0 && y > 0){ wake_up(RADIO, 0); return;}   // if sleeping

    // all state
    dispHeader.released();
    dispFooter.released();

    switch(s_state){
        case RADIO:
            VUmeter_RA.released();    sdr_RA_volume.released(); btn_RA_mute.released();  btn_RA_prevSta.released(); btn_RA_nextSta.released();
            btn_RA_player.released(); btn_RA_dlna.released();   btn_RA_clock.released(); btn_RA_sleep.released();   btn_RA_settings.released();
            btn_RA_bt.released();     btn_RA_off.released();    btn_RA_staList.released(); btn_RA_recorder.released();
            break;
        case STATIONSLIST:
            lst_RADIO.released();
            break;
        case PLAYER:
            btn_PL_prevFile.released(); btn_PL_nextFile.released(); btn_PL_ready.released(); btn_PL_playAll.released(); btn_PL_shuffle.released();
            btn_PL_fileList.released(); btn_PL_radio.released();    btn_PL_off.released();
            btn_PL_mute.released();     btn_PL_pause.released();    btn_PL_cancel.released(); sdr_PL_volume.released(); btn_PL_playNext.released();
            btn_PL_playPrev.released(); pgb_PL_progress.released();
            break;
        case AUDIOFILESLIST:
            lst_PLAYER.released(x, y);
            break;
        case DLNA:
            sdr_DL_volume.released(); btn_DL_mute.released(); btn_DL_pause.released(); btn_DL_radio.released(); btn_DL_fileList.released(); btn_DL_cancel.released(); pgb_DL_progress.released();
            break;
        case DLNAITEMSLIST:
            lst_DLNA.released(x, y);
            break;
        case CLOCK:
            btn_CL_mute.released(); btn_CL_alarm.released(); btn_CL_radio.released(); clk_CL_24.released(); sdr_CL_volume.released(); btn_CL_off.released();
            break;
        case ALARMCLOCK:
            clk_AC_red.released(); btn_AC_left.released(); btn_AC_right.released(); btn_AC_up.released(); btn_AC_down.released(); btn_AC_ready.released();
            break;
        case SLEEPTIMER:
            btn_SL_up.released(); btn_SL_down.released(); btn_SL_ready.released(); btn_SL_cancel.released();
            break;
        case SETTINGS:
            btn_SE_bright.released(); btn_SE_equal.released();  btn_SE_wifi.released(); btn_SE_radio.released();
            break;
        case BRIGHTNESS:
            sdr_BR_value.released();  btn_BR_ready.released(); pic_BR_logo.released();
            break;
        case EQUALIZER:
            sdr_EQ_lowPass.released(); sdr_EQ_bandPass.released(); sdr_EQ_highPass.released(); sdr_EQ_balance.released(); btn_EQ_lowPass.released(); btn_EQ_bandPass.released();
            btn_EQ_highPass.released(); btn_EQ_balance.released(); txt_EQ_lowPass.released(); txt_EQ_bandPass.released(); txt_EQ_highPass.released(); txt_EQ_balance.released();
            btn_EQ_Radio.released(); btn_EQ_Player.released(); btn_EQ_mute.released();
            break;
        case BLUETOOTH:
            btn_BT_pause.released(); btn_BT_radio.released(); btn_BT_volDown.released(); btn_BT_volUp.released(); btn_BT_mode.released(); btn_BT_power.released();
            break;
        case IR_SETTINGS:
            btn_IR_radio.released();
            break;
        case WIFI_SETTINGS:
            cls_wifiSettings.released();
            break;
        default:
            break;
    }
    // printfln(s_tag.tp_info, "tp_released, state is: {}", s_state);
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void tp_long_released(uint16_t x, uint16_t y){
    MWR_LOG_INFO("tp long released");
//    if(s_state == DLNAITEMSLIST) {lst_DLNA.longReleased();}
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void graphicObjects_OnChange(ps_ptr<char> name, int32_t val) {
    ps_ptr<char> c;
    if (name.equals("sdr_RA_volume"))   { setTimeCounter(2); setVolume(val); goto exit; }
    if (name.equals("sdr_PL_volume"))   { setVolume(val); goto exit; }
    if (name.equals("sdr_DL_volume"))   { setVolume(val); goto exit; }
    if (name.equals("sdr_CL_volume"))   { setVolume(val); goto exit; }
    if (name.equals("sdr_BR_value"))    { s_brightness = val; setTFTbrightness(s_brightness, s_bh1750Value); txt_BR_value.setText(int2str(val)); txt_BR_value.show(); goto exit; }
    if (name.equals("sdr_EQ_LP"))       { c.assignf("{} dB", val); s_tone.LP  = val; webSrv.send("settone=", getI2STone().c_get()); setI2STone(); txt_EQ_lowPass.setText(c.c_get());  txt_EQ_lowPass.show(); goto exit; }
    if (name.equals("sdr_EQ_BP"))       { c.assignf("{} dB", val); s_tone.BP  = val; webSrv.send("settone=", getI2STone().c_get()); setI2STone(); txt_EQ_bandPass.setText(c.c_get()); txt_EQ_bandPass.show(); goto exit; }
    if (name.equals("sdr_EQ_HP"))       { c.assignf("{} dB", val); s_tone.HP  = val; webSrv.send("settone=", getI2STone().c_get()); setI2STone(); txt_EQ_highPass.setText(c.c_get()); txt_EQ_highPass.show(); goto exit; }
    if (name.equals("sdr_EQ_BAL"))      { if(val < 0)       c.assignf("{}/0 dB", val);  // e.g. -10/0 dB
                                          else if (val > 0) c.assignf("0/-{} dB", val); // e.g. 0/-8 dB
                                          else              c.assignf("0/0 dB", val);   // 0/0 dB
                                          s_tone.BAL = val; webSrv.send("settone=", getI2STone().c_get()); setI2STone(); txt_EQ_balance.setText(c.c_get()); txt_EQ_balance.show(); goto exit; }
    if (name.equals("pgb_PL_progress")) { goto exit; }
    if (name.equals("pgb_DL_progress")) { goto exit; }

    MWR_LOG_WARN("unused event: graphicObject {} was changed, val {}", name.c_get(), val);
exit:
    return;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void graphicObjects_OnClick(ps_ptr<char> name, uint8_t val) { // val = 0 --> is inactive

    // all state
    if (name.equals("dispHeader"))                 { goto exit; }
    if (name.equals("header_Item"))                { goto exit; }
    if (name.equals("timeString"))                 { goto exit; }
    if (name.equals("dispFooter"))                 { goto exit; }
    if (name.equals("footer_StaNr"))               { goto exit; }
    if (name.equals("footer_Antenna"))             { goto exit; }
    if (name.equals("footer_Flag"))                { goto exit; }
    if (name.equals("footer_OffTimer"))            { goto exit; }
    if (name.equals("footer_Hourglass"))           { goto exit; }
    if (name.equals("footer_BitRate"))             { goto exit; }
    if (name.equals("footer_IPaddr"))              { goto exit; }

    if (s_state == RADIO) {
        if (val && name.equals("btn_RA_mute"))     { setTimeCounter(2); if (!s_f_mute) s_f_muteIsPressed = true; goto exit; }
        if (val && name.equals("btn_RA_recorder")) { goto exit; }
        if (val && name.equals("btn_RA_prevSta"))  { setTimeCounter(2); goto exit; }
        if (val && name.equals("btn_RA_nextSta"))  { setTimeCounter(2); goto exit; }
        if (val && name.equals("btn_RA_staList"))  { goto exit; }
        if (val && name.equals("btn_RA_player"))   { goto exit; }
        if (val && name.equals("btn_RA_dlna"))     { goto exit; }
        if (val && name.equals("btn_RA_clock"))    { goto exit; }
        if (val && name.equals("btn_RA_sleep"))    { goto exit; }
        if (val && name.equals("btn_RA_bright"))   { goto exit; }
        if (!val && name.equals("btn_RA_bright"))  { setTimeCounter(2); goto exit; }
        if (val && name.equals("btn_RA_equal"))    { goto exit; }
        if (val && name.equals("btn_RA_bt"))       { goto exit; }
        if (!val && name.equals("btn_RA_bt"))      { setTimeCounter(2); goto exit; }
        if (val && name.equals("btn_RA_off"))      { goto exit; }
        if (val && name.equals("btn_RA_settings")) { goto exit; }
        if (val && name.equals("VUmeter_RA"))      { goto exit; }
        if (val && name.equals("txt_RA_sTitle"))   { goto exit; }
        if (       name.equals("sdr_RA_volume"))   { goto exit; }
    }
    if (s_state == STATIONSLIST) {
        if (val && name.equals("lst_RADIO"))       { setTimeCounter(LIST_TIMER); goto exit; }
    }
    if (s_state == PLAYER) {
        if (val && name.equals("btn_PL_mute"))     { if (!s_f_mute) s_f_muteIsPressed = true; goto exit; }
        if (val && name.equals("btn_PL_pause"))    { goto exit; }
        if (val && name.equals("btn_PL_cancel"))   { goto exit; }
        if (val && name.equals("btn_PL_prevFile")) {
            if (s_cur_AudioFileNr > 0) {
                s_cur_AudioFileNr--;
                showPlayerFileName(s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr));
                showAudioFileNumber();
            }
            goto exit;
        }
        if (val && name.equals("btn_PL_nextFile")) {
            if (s_cur_AudioFileNr + 1 < s_SD_content.getSize()) {
                s_cur_AudioFileNr++;
                showPlayerFileName(s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr));
                showAudioFileNumber();
            }
            goto exit;
        }
        if (val && name.equals("btn_PL_ready"))    { goto exit; }
        if (val && name.equals("btn_PL_playAll"))  { goto exit; }
        if (val && name.equals("btn_PL_shuffle"))  { goto exit; }
        if (val && name.equals("btn_PL_fileList")) { goto exit; }
        if (val && name.equals("btn_PL_radio"))    { goto exit; }
        if (val && name.equals("btn_PL_off"))      { goto exit; }
        if (val && name.equals("btn_PL_playPrev")) { s_cur_AudioFileNr = s_SD_content.getPrevAudioFile(s_cur_AudioFileNr); goto exit; }
        if (val && name.equals("btn_PL_playNext")) { s_cur_AudioFileNr = s_SD_content.getNextAudioFile(s_cur_AudioFileNr); goto exit; }
        if (val && name.equals("pgb_PL_progress")) { goto exit; }
        if (val && name.equals("txt_PL_fName"))    { goto exit; }
        if (val && name.equals("sdr_PL_volume"))   { goto exit; }
    }
    if (s_state == AUDIOFILESLIST) {
        if (val && name.equals("lst_PLAYER")) { setTimeCounter(LIST_TIMER); goto exit; }
    }
    if (s_state == DLNA) {
        if (val && name.equals("btn_DL_mute"))     { if (!s_f_mute) s_f_muteIsPressed = true; goto exit; }
        if (val && name.equals("btn_DL_pause"))    { goto exit; }
        if (val && name.equals("btn_DL_radio"))    { goto exit; }
        if (val && name.equals("btn_DL_fileList")) { goto exit; }
        if (val && name.equals("btn_DL_cancel"))   { clearStationName(); btn_DL_pause.setActive(false); goto exit; }
        if (val && name.equals("pgb_DL_progress")) { goto exit; }
    }
    if (s_state == DLNAITEMSLIST) {
        if (val && name.equals("lst_DLNA")) {
            setTimeCounter(LIST_TIMER * 3);
            s_f_dlnaWaitForResponse = true;
            goto exit;
        }
    }
    if (s_state == CLOCK) {
        if (val && name.equals("btn_CL_mute"))         { if (!s_f_mute) { s_f_muteIsPressed = true; } goto exit; }
        if (val && name.equals("btn_CL_alarm"))        { goto exit; }
        if (val && name.equals("btn_CL_radio"))        { goto exit; }
        if (val && name.equals("clk_CL_24"))           { goto exit; }
        if (val && name.equals("btn_CL_off"))          { goto exit; }
    }
    if (s_state == ALARMCLOCK) {
        if (val && name.equals("clk_AC_red"))          { goto exit; }
        if (val && name.equals("btn_AC_left"))         { goto exit; }
        if (val && name.equals("btn_AC_right"))        { goto exit; }
        if (val && name.equals("btn_AC_up"))           { goto exit; }
        if (val && name.equals("btn_AC_down"))         { goto exit; }
        if (val && name.equals("btn_AC_ready"))        { goto exit; }
        if (val && name.starts_with("txt_alarm_days")) { goto exit; }
        if (val && name.starts_with("txt_alarm_time")) { goto exit; }
    }
    if (s_state == SLEEPTIMER) {
        if (val && name.equals("btn_SL_up"))      { goto exit; }
        if (val && name.equals("btn_SL_down"))    { goto exit; }
        if (val && name.equals("btn_SL_ready"))   { goto exit; }
        if (val && name.equals("btn_SL_cancel"))  { goto exit; }
    }
    if (s_state == SETTINGS) {
        if (val && name.equals("btn_SE_bright"))  { goto exit; }
        if (val && name.equals("btn_SE_equal"))   { goto exit; }
        if (val && name.equals("btn_SE_wifi"))    { goto exit; }
        if (val && name.equals("btn_SE_radio"))   { goto exit; }
    }
    if (s_state == BRIGHTNESS) {
        if (val && name.equals("btn_BR_ready"))   { goto exit; }
        if (val && name.equals("pic_BR_logo"))    { goto exit; }
    }
    if (s_state == EQUALIZER) {
        if (val && name.equals("btn_EQ_LP"))      { sdr_EQ_lowPass.setValue(0);  goto exit; }
        if (val && name.equals("btn_EQ_BP"))      { sdr_EQ_bandPass.setValue(0); goto exit; }
        if (val && name.equals("btn_EQ_HP"))      { sdr_EQ_highPass.setValue(0); goto exit; }
        if (val && name.equals("btn_EQ_BAL"))     { sdr_EQ_balance.setValue(0);  goto exit; }
        if (val && name.equals("btn_EQ_Radio"))   { goto exit; }
        if (val && name.equals("btn_EQ_Player"))  { goto exit; }
        if (val && name.equals("btn_EQ_mute"))    { if (!s_f_mute) s_f_muteIsPressed = true; goto exit; }
    }
    if (s_state == BLUETOOTH) {
        if (val && name.equals("btn_BT_pause"))   { bt_emitter.pauseResume(); goto exit; }
        if (val && name.equals("btn_BT_radio"))   { goto exit; }
        if (val && name.equals("btn_BT_volDown")) { bt_emitter.downvolume(); goto exit; }
        if (val && name.equals("btn_BT_volUp"))   { bt_emitter.upvolume();   goto exit; }
        if (val && name.equals("btn_BT_mode"))    { if(s_bt_emitter.mode.equals("RX")) s_bt_emitter.mode = "TX"; else s_bt_emitter.mode = "RX"; goto exit; }
        if (val && name.equals("btn_BT_power"))   { goto exit; }
        if (val && name.equals("txt_BT_mode"))    { goto exit; }
    }
    if (s_state == IR_SETTINGS) {
        if (val && name.equals("btn_IR_radio"))   { goto exit; }
    }
    if (s_state == WIFI_SETTINGS) {
        s_timestamp = millis() + 4000; //  every click
        if (val && name.equals("key_WI_input")) {
            MWR_LOG_DEBUG("val {}", val);
            if (val == 13) {
                changeState(RADIO, 0);
                goto exit;
            }
        }
        if (name.starts_with("txt_btn"))                       { goto exit; }
        if (val && name.equals("wifiSettings"))                { goto exit; }
        if (       name.equals("wifiSettings_keyBoard"))       { goto exit; }
        if (val && name.equals("btn_SE_wifi"))                 { goto exit; }
        if (val && name.equals("select_txtbtn_down"))          { goto exit; }
        if (val && name.equals("wifiSettings_selectbox_ssid")) { goto exit; }
        if (val && name.equals("wifiSettings_txtbox_pwd"))     { goto exit; }
        if (val && name.equals("wifiSettings_keyBoard"))       { goto exit; }
        if (val && name.equals("select_txtbox_ssid"))          { goto exit; }
        if (val && name.equals("select_txtbtn_up"))            { goto exit; }
        if (val && name.equals("select_txtbtn_down"))          { goto exit; }
    }
    MWR_LOG_WARN("unused event: graphicObject {} was clicked", name.c_get());
exit:
    return;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void graphicObjects_OnRelease(ps_ptr<char> name, releasedArg ra) {

    // all state
    if (name.equals("dispHeader")) { goto exit; }
    if (name.equals("dispFooter")) { goto exit; }

    if (s_state == RADIO) {
        if (name.equals("btn_RA_mute"))     { muteChanged(btn_RA_mute.getValue()); goto exit; }
        if (name.equals("btn_RA_recorder")) { s_f_recording = btn_RA_recorder.getValue(); goto exit; }
        if (name.equals("btn_RA_prevSta"))  { prevFavStation(); dispFooter.updateStation(s_cur_station); goto exit; }
        if (name.equals("btn_RA_nextSta"))  { nextFavStation(); dispFooter.updateStation(s_cur_station); goto exit; }
        if (name.equals("btn_RA_staList"))  { changeState(STATIONSLIST, 0); goto exit; }
        if (name.equals("btn_RA_player"))   { changeState(PLAYER, 0);     if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_PL(0); } goto exit; }
        if (name.equals("btn_RA_dlna"))     { changeState(DLNA, 0);       if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_DL(0); } goto exit; }
        if (name.equals("btn_RA_clock"))    { changeState(CLOCK, 0);                                                                    goto exit; }
        if (name.equals("btn_RA_sleep"))    { changeState(SLEEPTIMER, 0); if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_SL(0); } goto exit; }
        if (name.equals("btn_RA_settings")) { changeState(SETTINGS, 0);   if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_SE(0); } goto exit; }
        if (name.equals("btn_RA_equal"))    { changeState(EQUALIZER, 0);  if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_EQ(0); } goto exit; }
        if (name.equals("btn_RA_bt"))       { changeState(BLUETOOTH, 0);  if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_BT(0); } goto exit; }
        if (name.equals("btn_RA_off"))      { fall_asleep(); goto exit; }
        if (name.equals("VUmeter_RA"))      { goto exit; }
        if (name.equals("sdr_RA_volume"))   { goto exit; }
    }
    if (s_state == STATIONSLIST) {
        if (name.equals("lst_RADIO"))       { if (ra.val1) { setStationByNumber(ra.val1); changeState(RADIO, 0); } goto exit; }
    }
    if (s_state == PLAYER) {
        if (name.equals("btn_PL_mute"))     { muteChanged(btn_PL_mute.getValue()); goto exit; }
        if (name.equals("btn_PL_pause"))    { if (s_f_isFSConnected) { audioPauseResumeAndUpdateState(); } goto exit; }
        if (name.equals("btn_PL_cancel"))   { stopSong(); changeState(PLAYER, 0); if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_PL(0); } goto exit; }
        if (name.equals("btn_PL_prevFile")) { if(s_ir_btn_select == 0) set_ir_pos_PL(0); goto exit; }
        if (name.equals("btn_PL_nextFile")) { if(s_ir_btn_select == 1) set_ir_pos_PL(0); goto exit; }
        if (name.equals("btn_PL_ready"))    { SD_playFile(s_cur_AudioFolder.c_get(), s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr));
                                              changeState(PLAYER, 1); if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_PL(0); } showAudioFileNumber(); goto exit; }
        if (name.equals("btn_PL_playAll"))  { if(playlist.create_playlist_from_SD_folder(s_cur_AudioFolder)){
                                                  playlist.sort_alphabetical(); s_subState_player = 1; s_f_playlistEnabled = true; }
                                              goto exit; }
        if (name.equals("btn_PL_shuffle"))  { if(playlist.create_playlist_from_SD_folder(s_cur_AudioFolder)){
                                                  playlist.sort_random(); s_subState_player = 1; s_f_playlistEnabled = true; }
                                              goto exit; }
        if (name.equals("btn_PL_fileList")) { s_SD_content.listFilesInDir(s_cur_AudioFolder.c_get(), true, false); changeState(AUDIOFILESLIST, 0); goto exit; }
        if (name.equals("btn_PL_radio"))    { stopSong(); changeState(RADIO, 0); goto exit; }
        if (name.equals("btn_PL_off"))      { fall_asleep(); goto exit; }
        if (name.equals("sdr_PL_volume"))   { goto exit; }
        if (name.equals("btn_PL_playNext")) { SD_playFile(s_cur_AudioFolder.c_get(), s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr)); showAudioFileNumber(); goto exit; }
        if (name.equals("btn_PL_playPrev")) { SD_playFile(s_cur_AudioFolder.c_get(), s_SD_content.getColouredSStringByIndex(s_cur_AudioFileNr)); showAudioFileNumber(); goto exit; }
        if (name.equals("pgb_PL_progress")) { audio.setTimeOffset(ra.val2); goto exit; }
    }
    if (s_state == AUDIOFILESLIST) {
        if (name.equals("lst_PLAYER"))      { if (ra.val1 == 1) { ; } // wipe up/down
                                              if (ra.val1 == 2) {     // next prev folder
                                                  s_cur_AudioFolder = ra.arg1;
                                                  s_cur_AudioFileNr = ra.val2;
                                                  lst_PLAYER.show(s_cur_AudioFolder, s_cur_AudioFileNr);
                                              }
                                              if (ra.val1 == 3) {     // audio file
                                                  s_cur_AudioFolder = ra.arg1;
                                                  s_cur_AudioFileNr = ra.val2;
                                                  stopSong();
                                                  SD_playFile(ra.arg3.c_get());
                                              }
                                              goto exit; }
    }
    if (s_state == DLNA) {
        if (name.equals("btn_DL_mute"))     { muteChanged(btn_DL_mute.getValue());   if(s_ir_btn_select == 0) set_ir_pos_DL(0); goto exit; }
        if (name.equals("btn_DL_pause"))    { audioPauseResumeAndUpdateState(); if(s_ir_btn_select == 1) set_ir_pos_DL(0); goto exit; }
        if (name.equals("btn_DL_cancel"))   { stopSong();
                                              txt_DL_fName.setText("");
                                              txt_DL_fName.show();
                                              pgb_DL_progress.reset();
                                              btn_DL_pause.setActive(false);
                                              btn_DL_pause.show();
                                              if(s_ir_btn_select == 3) set_ir_pos_DL(0);
                                              goto exit; }
        if (name.equals("btn_DL_fileList")) { changeState(DLNAITEMSLIST, 0); txt_DL_fName.setText(""); goto exit; }
        if (name.equals("btn_DL_radio"))    { stopSong(); changeState(RADIO, 0); goto exit; }
        if (name.equals("sdr_DL_volume"))   { goto exit; }
        if (name.equals("pgb_DL_progress")) { audio.setTimeOffset(ra.val2); goto exit; }
    }
    if (s_state == DLNAITEMSLIST) {
        if (name.equals("lst_DLNA"))        {   if (ra.val1 == 0) { // wipe up/down
                                                    goto exit;
                                                }
                                                if (ra.val1 == 1) { // play a file
                                                    txt_DL_fName.setTextColor(TFT_CYAN);
                                                    txt_DL_fName.setText(ra.arg2.c_get());
                                                    connecttohost(ra.arg1);
                                                    changeState(DLNA, 0);
                                                    goto exit;
                                                }
                                                if (ra.val1 == 2) {// browse dlna object, waiting for content and create a playlist
                                                    dlna.browseServer(ra.val2, ra.arg1.c_get(), 0, 50);
                                                    s_f_dlnaMakePlaylistOTF = true;
                                                    goto exit;
                                                }
                                                else {
                                                    MWR_LOG_WARN("unknown val: {}", ra.val1);
                                                }
                                            }
    }
    if (s_state == CLOCK) {
        if (name.equals("btn_CL_mute"))     { muteChanged(btn_CL_mute.getValue()); if(s_ir_btn_select == 2) set_ir_pos_CL(2); goto exit; }
        if (name.equals("btn_CL_alarm"))    { changeState(ALARMCLOCK, 0); if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_AC(0); } goto exit; }
        if (name.equals("btn_CL_radio"))    { changeState(RADIO, 0); goto exit; }
        if (name.equals("clk_CL_24"))       { changeState(CLOCK, 0); goto exit; }
        if (name.equals("btn_CL_off"))      { fall_asleep(); goto exit; }
        if (name.equals("sdr_CL_volume"))   { goto exit; }
    }
    if (s_state == ALARMCLOCK) {
        if (name.equals("clk_AC_red"))      { goto exit; }
        if (name.equals("btn_AC_left"))     { clk_AC_red.shiftLeft();  if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_AC(0); } goto exit; }
        if (name.equals("btn_AC_right"))    { clk_AC_red.shiftRight(); if(s_f_ok_from_ir) { s_ir_btn_select = 1; set_ir_pos_AC(1); } goto exit; }
        if (name.equals("btn_AC_up"))       { clk_AC_red.digitUp();    if(s_f_ok_from_ir) { s_ir_btn_select = 2; set_ir_pos_AC(2); } goto exit; }
        if (name.equals("btn_AC_down"))     { clk_AC_red.digitDown();  if(s_f_ok_from_ir) { s_ir_btn_select = 3; set_ir_pos_AC(3); } goto exit; }
        if (name.equals("btn_AC_ready"))    { updateSettings(); changeState(CLOCK, 0); logAlarmItems(); goto exit; }
    }
    if (s_state == SLEEPTIMER) {
        if (name.equals("btn_SL_up"))       { display_sleeptime(1);  if(s_ir_btn_select == 0) set_ir_pos_SL(0); goto exit; }
        if (name.equals("btn_SL_down"))     { display_sleeptime(-1); if(s_ir_btn_select == 1) set_ir_pos_SL(1); goto exit; }
        if (name.equals("btn_SL_ready"))    { dispFooter.updateOffTime(s_sleeptime); changeState(RADIO, 0); goto exit; }
        if (name.equals("btn_SL_cancel"))   { changeState(RADIO, 0); goto exit; }
    }
    if (s_state == BRIGHTNESS) {
        if (name.equals("btn_BR_ready"))    { changeState(RADIO, 0); goto exit;}
        if (name.equals("pic_BR_logo"))     { goto exit; }
        if (name.equals("sdr_BR_value"))    { goto exit; }
    }
    if (s_state == EQUALIZER) {
        if (name.equals("btn_EQ_Radio"))    { changeState(RADIO, 0); goto exit; }
        if (name.equals("btn_EQ_Player"))   { changeState(PLAYER, 0); goto exit; }
        if (name.equals("btn_EQ_mute"))     { muteChanged(btn_EQ_mute.getValue()); if(s_ir_btn_select == 2) set_ir_pos_EQ(2); goto exit; }
        if (name.equals("btn_EQ_BAL"))      {                                      if(s_ir_btn_select == 3) set_ir_pos_EQ(3); goto exit; }
        if (name.equals("btn_EQ_LP"))       {                                      if(s_ir_btn_select == 4) set_ir_pos_EQ(4); goto exit; }
        if (name.equals("btn_EQ_BP"))       {                                      if(s_ir_btn_select == 5) set_ir_pos_EQ(5); goto exit; }
        if (name.equals("btn_EQ_HP"))       {                                      if(s_ir_btn_select == 6) set_ir_pos_EQ(6); goto exit; }
        if (name.equals("sdr_EQ_HP"))       { goto exit; }
        if (name.equals("sdr_EQ_BP"))       { goto exit; }
        if (name.equals("sdr_EQ_LP"))       { goto exit; }
        if (name.equals("sdr_EQ_BAL"))      { goto exit; }
    }
    if (s_state == SETTINGS) {
        if (name.equals("btn_SE_bright"))   { changeState(BRIGHTNESS, 0);    if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_BR(0); } goto exit; }
        if (name.equals("btn_SE_equal"))    { changeState(EQUALIZER, 0);     if(s_f_ok_from_ir) { s_ir_btn_select = 0; set_ir_pos_EQ(0); } goto exit; }
        if (name.equals("btn_SE_wifi"))     { changeState(WIFI_SETTINGS, 0); goto exit; }
        if (name.equals("btn_SE_radio"))    { changeState(RADIO, 0); goto exit; }
    }
    if (s_state == BLUETOOTH) {
        if (name.equals("btn_BT_volDown"))  { if(s_ir_btn_select == 0) set_ir_pos_BT(0); goto exit; }
        if (name.equals("btn_BT_volUp"))    { if(s_ir_btn_select == 1) set_ir_pos_BT(1); goto exit; }
        if (name.equals("btn_BT_pause"))    { if(s_ir_btn_select == 2) set_ir_pos_BT(2); goto exit; }
        if (name.equals("btn_BT_mode"))     { if(s_ir_btn_select == 3) set_ir_pos_BT(3); goto exit; }
        if (name.equals("btn_BT_radio"))    { changeState(RADIO, 0); goto exit; }
        if (name.equals("btn_BT_power"))    { if(s_ir_btn_select == 5 && s_bt_emitter.found) set_ir_pos_BT(5); s_bt_emitter.enabled = !s_bt_emitter.enabled; goto exit; }
    }
    if (s_state == IR_SETTINGS) {
        if (name.equals("btn_IR_radio"))    { changeState(RADIO, 0); goto exit; }
    }
    if (s_state == WIFI_SETTINGS) {
        if (name.equals("wifiSettings"))    { setWiFiCredentials(ra.arg1.c_get(), ra.arg2.c_get());
                                              msg_box.setText("ESP restart", false, false);
                                              msg_box.show();
                                              vTaskDelay(2000);
                                              s_f_msg_box = true;
                                              s_f_esp_restart = true;
                                              goto exit; }
        if (name.starts_with("txt_btn"))                { goto exit; }
        if (name.equals("wifiSettings_selectbox_ssid")) { goto exit; }
        if (name.equals("wifiSettings_txtbox_pwd"))     { goto exit; }
        if (name.equals("select_txtbtn_up"))            { goto exit; }
        if (name.equals("select_txtbtn_down"))          { goto exit; }
        if (name.equals("select_txtbox_ssid"))          { goto exit; }
    }
    MWR_LOG_WARN("unused event: graphicObject {} was released", name);
exit:
    s_f_ok_from_ir = false;
    return;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// clang-format on
/*🟢🟡🔴*/
