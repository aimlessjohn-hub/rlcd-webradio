#include <Arduino.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "Audio.h"
#include "es8311.h"
#include "ST7305_U8g2.h"
#include "config.h"
#include "adc_bsp.h"
#include "shtc3.h"
#include "pcf85063.h"
#include <esp_sntp.h>

// --- Global Objects ---
Audio audio;
ES8311 es;
static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *u8g2 = nullptr;
WebServer server(80);
Preferences prefs;

// --- Clock & RTC State ---
bool rtcAvailable = false;
bool ntpSynced = false;

// --- Battery State ---
float batteryVoltage = 0.0f;
int batteryPercent = 0;
uint32_t lastBatteryRead = 0;

// --- SHTC3 Room Sensor State ---
bool shtc3Available = false;
float roomTemp = 0.0f;
float roomHumi = 0.0f;
uint32_t lastShtc3Read = 0;

// --- Radio State ---
String currentStation = "Rock Antenne";
String currentTitle = "Puffere Stream...";
String streamStatus = "IDLE";
int currentVolume = 8; // Default to comfortable volume
int currentPresetIdx = 0;
bool isPlaying = false;
bool displayNeedsUpdate = true;
uint32_t lastDisplayUpdate = 0;

// --- Preset Radio Stations (im NVS-Flash anpassbar) ---
struct ActiveStation {
    String name;
    String url;
};
ActiveStation currentPresets[NUM_PRESETS];
void loadPresets();
void savePreset(int slot, const String &name, const String &url);
void resetPresets();

// --- Klang & Psychoakustik Zustand ---
struct SoundProfile {
    const char *name;
    const char *desc;
    float gainLow;
    float gainMid;
    float gainHigh;
};

const SoundProfile SOUND_PROFILES[] = {
    {"Warm / Musik",    "Dezenter Bass, warme Mitten gegen Quäken",         1.8f, -1.5f,  1.0f},
    {"Sprache / News", "Klare Sprachverständlichkeit, schlanke Bässe",     -3.5f,  3.0f,  1.0f},
    {"Loudness Boost",  "Psychoakustische Fülle bei Zimmerlautstärke",       5.0f, -4.0f,  3.0f},
    {"Neutral / Flat",  "Unverändertes Originalsignal (linear)",             0.0f,  0.0f,  0.0f}
};

const int NUM_SOUND_PROFILES = sizeof(SOUND_PROFILES) / sizeof(SOUND_PROFILES[0]);
int currentSoundProfile = 0;   // Default: Warm / Musik
bool drcEnabled = true;        // Default: Hardware DRC aktiv
bool forceMonoEnabled = true;  // Default: Force Mono aktiv

// --- Wecker Zustand ---
bool alarmEnabled = false;
int alarmHour = 7;
int alarmMinute = 0;
int alarmStation = 0;
int alarmVolume = 14;
bool isAlarmActive = false;
bool isRamping = false;
uint32_t lastRampTime = 0;
bool alarmTriggeredThisMinute = false;
int lastAlarmCheckMinute = -1;

// Schlummerfunktion (Snooze - 9 Minuten)
bool snoozeActive = false;
int snoozeHour = 0;
int snoozeMinute = 0;

// NTP Zeit-Zustand
char timeStr[16] = "--:--";
char dateStr[16] = "--.--.";
int lastMinute = -1;

// Wecker & Sound Funktionsdeklarationen
void applySoundSettings();
void saveSoundSettings();
void loadSoundSettings();
void saveAlarmSettings();
void loadAlarmSettings();
void triggerAlarm();
void dismissAlarm();
void snoozeAlarm();
void checkAlarm(int curHour, int curMin);
void updateTime();

// --- Homelab & Weather State ---
const char *STATUS_ENDPOINT = "http://192.168.178.83:8123/status?compact=1";
uint32_t lastStatusFetch = 0;
const uint32_t STATUS_POLL_INTERVAL = 120000; // 2 Minuten

float weather_temp_c = 0.0;
float weather_t_max = 0.0;
float weather_t_min = 0.0;
int weather_rain_pct = 0;
int weather_code = 0;
float homelab_w = 0.0;
float pve_cpu_pct = 0.0;
float pve_temp_c = 0.0;
float pve_ram_gb = 0.0;
float pve_ram_total_gb = 0.0;
int vms_running = 0;
int vms_total = 0;
String serverGenerated = "--:--";
bool hasServerData = false;

// Button state (Short / Long press)
bool lastBootState = HIGH;
bool lastKeyState = HIGH;
uint32_t bootPressTime = 0;
uint32_t keyPressTime = 0;
uint32_t lastBootRepeat = 0;
uint32_t lastKeyRepeat = 0;
bool bootHandledLong = false;
bool keyHandledLong = false;

bool wifiServicesInitialized = false;

// Function Declarations
void playStation(const String &url, const String &name);
void stopStation();
void setVolume(int vol);
void updateDisplay();
void handleSerialCommands();
void setupWebServer();
void setupOTA();
void fetchHomelabStatus();
const char *getWmoText(int code);
void readBattery();
void onWifiConnected();


// --- Audio Callback for ESP32-audioI2S v4 ---
void onAudioInfo(Audio::msg_t m) {
    if (m.e == Audio::evt_streamtitle && m.msg) {
        currentTitle = String(m.msg);
        displayNeedsUpdate = true;
        Serial.printf("[STREAM TITLE] %s\n", m.msg);
    } else if (m.e == Audio::evt_name && m.msg) {
        currentStation = String(m.msg);
        displayNeedsUpdate = true;
        Serial.printf("[STATION] %s\n", m.msg);
    } else if (m.e == Audio::evt_eof) {
        streamStatus = "STOPPED";
        isPlaying = false;
        digitalWrite(PA_ENABLE, LOW); // Endstufe ausschalten
        es.standby();                 // Codec in Standby
        displayNeedsUpdate = true;
        Serial.println("[AUDIO] Stream beendet -> Standby (PA off)");
    } else if (m.msg) {
        Serial.printf("[%s] %s\n", m.s ? m.s : "INFO", m.msg);
    }
}

const char *getWmoText(int code) {
    switch (code) {
        case 0: return "Klar / Sonnig";
        case 1: return "Meist sonnig";
        case 2: return "Teils bewoelkt";
        case 3: return "Bedeckt";
        case 45: case 48: return "Nebel";
        case 51: case 53: case 55: return "Nieselregen";
        case 61: case 63: case 65: return "Regen";
        case 71: case 73: case 75: return "Schneefall";
        case 80: case 81: case 82: return "Schauer";
        case 95: case 96: case 99: return "Gewitter";
        default: return "Wetter OK";
    }
}

void readBattery() {
    float newV = Adc_GetBatteryVoltage();
    if (newV > 0.5f) {
        if (batteryVoltage == 0.0f) {
            batteryVoltage = newV;
        } else {
            batteryVoltage = 0.85f * batteryVoltage + 0.15f * newV;
        }
        int newPct = Adc_GetBatteryLevel(batteryVoltage);
        if (newPct != batteryPercent) {
            batteryPercent = newPct;
            displayNeedsUpdate = true;
        }
    }
}

void fetchHomelabStatus() {
    if (WiFi.status() != WL_CONNECTED) return;

    Serial.println("[FETCH] Rufe Status von 192.168.178.83 ab...");
    HTTPClient http;
    http.begin(STATUS_ENDPOINT);
    http.setTimeout(4000);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            if (doc["wetter"].is<JsonObjectConst>()) {
                weather_temp_c = doc["wetter"]["temp_c"] | 0.0f;
                weather_t_max = doc["wetter"]["t_max"] | 0.0f;
                weather_t_min = doc["wetter"]["t_min"] | 0.0f;
                weather_rain_pct = doc["wetter"]["rain_pct"] | 0;
                weather_code = doc["wetter"]["code"] | 0;
            }

            if (doc["homelab_dose"].is<JsonObjectConst>()) {
                homelab_w = doc["homelab_dose"]["lab_w"] | 0.0f;
            } else if (doc["homelab_w"].is<float>()) {
                homelab_w = doc["homelab_w"].as<float>();
            }

            if (doc["pve_host"]["cpu_pct"].is<float>()) {
                pve_cpu_pct = doc["pve_host"]["cpu_pct"].as<float>();
            } else if (doc["pve_cpu"].is<float>()) {
                pve_cpu_pct = doc["pve_cpu"].as<float>();
            }

            if (doc["pve_host"]["temp_k10temp_c"].is<float>()) {
                pve_temp_c = doc["pve_host"]["temp_k10temp_c"].as<float>();
            } else if (doc["pve_host"]["temp_nvme_c"].is<float>()) {
                pve_temp_c = doc["pve_host"]["temp_nvme_c"].as<float>();
            } else if (doc["pve_temp"].is<float>()) {
                pve_temp_c = doc["pve_temp"].as<float>();
            }

            if (doc["pve_host"]["ram_gb"].is<float>()) {
                pve_ram_gb = doc["pve_host"]["ram_gb"].as<float>();
                pve_ram_total_gb = doc["pve_host"]["ram_total_gb"] | 64.0f;
            } else if (doc["pve_ram"].is<float>()) {
                pve_ram_gb = doc["pve_ram"].as<float>();
            }

            if (doc["vms_running"].is<int>()) vms_running = doc["vms_running"].as<int>();
            if (doc["vms_total"].is<int>()) vms_total = doc["vms_total"].as<int>();

            if (doc["generated"].is<const char *>()) {
                serverGenerated = doc["generated"].as<String>();
            }

            hasServerData = true;
            displayNeedsUpdate = true;
            Serial.printf("[FETCH OK] Wetter: %.1f C (Code %d), Homelab: %.1f W, CPU: %.1f%%, RAM: %.1f/%.1f GB\n",
                          weather_temp_c, weather_code, homelab_w, pve_cpu_pct, pve_ram_gb, pve_ram_total_gb);
        } else {
            Serial.printf("[FETCH JSON ERROR] %s\n", err.c_str());
        }
    } else {
        Serial.printf("[FETCH HTTP ERROR] Code %d\n", httpCode);
    }
    http.end();
    lastStatusFetch = millis();
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println(" Waveshare ESP32-S3-RLCD WebRadio & Dash");
    Serial.println("========================================");

    // 1. Initialize Display
    lcd.begin(0, U8G2_R1); // Landscape 400x300
    u8g2 = lcd.getU8g2();
    u8g2->setFontMode(1);
    u8g2->setDrawColor(1);

    u8g2->clearBuffer();
    u8g2->setFont(u8g2_font_helvB14_tr);
    u8g2->drawStr(40, 100, "Waveshare RLCD-4.2");
    u8g2->setFont(u8g2_font_helvR10_tr);
    u8g2->drawStr(40, 140, "Initialisiere Hardware...");
    u8g2->sendBuffer();

    // 2. Buttons & Battery ADC
    pinMode(BTN_BOOT, INPUT_PULLUP);
    pinMode(BTN_KEY, INPUT_PULLUP);

    if (digitalRead(BTN_KEY) == LOW) {
        delay(150);
        if (digitalRead(BTN_KEY) == LOW) {
            Serial.println("[BOOT] KEY held at startup! Rebooting to MultiBoot Launcher...");
            u8g2->clearBuffer();
            u8g2->drawStr(40, 140, "Starte MultiBoot-Launcher...");
            u8g2->sendBuffer();
            const esp_partition_t *launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
            if (launcher) {
                esp_ota_set_boot_partition(launcher);
                delay(300);
                ESP.restart();
            }
        }
    }

    // Always arm the next reboot (RST button, power cycle, crash) to return directly to the MultiBoot Launcher!
    const esp_partition_t *launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (launcher) {
        esp_ota_set_boot_partition(launcher);
        Serial.printf("[BOOT] Next boot partition automatically set to Launcher (%s)\n", launcher->label);
    }

    Adc_PortInit();
    readBattery();
    Serial.printf("[BATTERY] Initial: %.2f V (%d%%)\n", batteryVoltage, batteryPercent);

    // 3. Audio & Codec Init
    Serial.println("Initialisiere ES8311 Codec...");
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, LOW); // Power Amplifier initial AUS

    if (!es.begin(I2C_SDA, I2C_SCL, 400000)) {
        Serial.println("[FEHLER] ES8311 I2C Initialisierung fehlgeschlagen!");
    } else {
        Serial.println("[OK] ES8311 initialisiert.");
    }
    es.setVolume(90);
    es.setBitsPerSample(16);

    // SHTC3 Room Sensor Init (I2C)
    shtc3Available = shtc3_init();
    if (shtc3Available) {
        shtc3_read(&roomTemp, &roomHumi);
        Serial.printf("[SHTC3] Raum: %.1f C, %.1f%% rF\n", roomTemp, roomHumi);
    }

    // PCF85063 Hardware-RTC Init (I2C 0x51)
    rtcAvailable = pcf85063_init();
    if (rtcAvailable) {
        struct tm rtc_ti;
        if (pcf85063_read_time(&rtc_ti)) {
            Serial.printf("[RTC] Hardware-Uhrzeit: %02d:%02d:%02d %02d.%02d.%04d\n",
                          rtc_ti.tm_hour, rtc_ti.tm_min, rtc_ti.tm_sec,
                          rtc_ti.tm_mday, rtc_ti.tm_mon + 1, rtc_ti.tm_year + 1900);
            if (rtc_ti.tm_year >= 124) {
                time_t t = mktime(&rtc_ti);
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                snprintf(timeStr, sizeof(timeStr), "%02d:%02d", rtc_ti.tm_hour, rtc_ti.tm_min);
                snprintf(dateStr, sizeof(dateStr), "%02d.%02d.", rtc_ti.tm_mday, rtc_ti.tm_mon + 1);
                Serial.println("[RTC] Systemzeit von Hardware-RTC uebernommen.");
            }
        }
    }

    Audio::audio_info_callback = onAudioInfo;
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
    audio.setVolume(currentVolume);
    audio.setAudioTaskCore(0); // Native Audio Task on Core 0!

    // 4. Klang, Psychoakustik, Presets & Wecker initialisieren
    loadSoundSettings();
    applySoundSettings();
    loadPresets();
    loadAlarmSettings();

    // 5. Load Saved WiFi credentials or use default
    prefs.begin("radio", false);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    if (ssid.length() == 0 || pass.length() == 0) {
        ssid = DEFAULT_WIFI_SSID;
        pass = DEFAULT_WIFI_PASSWORD;
    }

    Serial.printf("Verbinde mit WLAN: %s ...\n", ssid.c_str());
    u8g2->clearBuffer();
    u8g2->setFont(u8g2_font_helvB14_tr);
    u8g2->drawStr(30, 80, "Verbinde mit WLAN...");
    u8g2->setFont(u8g2_font_helvR10_tr);
    u8g2->drawStr(30, 120, ssid.c_str());
    u8g2->sendBuffer();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // Vor Verbindungsaufbau KEIN Sleep, um Handshake-Timeouts zu verhindern!
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 25) {
        delay(500);
        Serial.print(".");
        timeout++;
    }
    if (WiFi.status() != WL_CONNECTED && (ssid != DEFAULT_WIFI_SSID || pass != DEFAULT_WIFI_PASSWORD)) {
        Serial.println("\n[WLAN] Gespeicherte Daten schlugen fehl, versuche Default-WLAN...");
        WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
        while (WiFi.status() != WL_CONNECTED && timeout < 50) {
            delay(500);
            Serial.print(".");
            timeout++;
        }
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        onWifiConnected();
    } else {
        Serial.println("[WARNUNG] WLAN verbindet noch im Hintergrund...");
        es.standby();
    }

    displayNeedsUpdate = true;
    updateDisplay();
}

void onTimeSyncNotification(struct timeval *tv) {
    ntpSynced = true;
    time_t now = tv->tv_sec;
    struct tm ti;
    localtime_r(&now, &ti);
    Serial.printf("[NTP] Zeit synchronisiert: %02d:%02d:%02d %02d.%02d.%04d\n",
                  ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", ti.tm_hour, ti.tm_min);
    snprintf(dateStr, sizeof(dateStr), "%02d.%02d.", ti.tm_mday, ti.tm_mon + 1);
    displayNeedsUpdate = true;

    if (rtcAvailable) {
        pcf85063_set_time(&ti);
        Serial.println("[RTC] Hardware-Uhr mit NTP kalibriert.");
    }
}

void onWifiConnected() {
    if (wifiServicesInitialized) return;
    wifiServicesInitialized = true;
    Serial.printf("[OK] WLAN verbunden! IP: %s\n", WiFi.localIP().toString().c_str());

    // NTP Zeitsynchronisation mit Callback und multiplen Fallbacks (Berlin)
    sntp_set_time_sync_notification_cb(onTimeSyncNotification);
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com", "192.168.178.1");

    setupOTA();
    setupWebServer();
    fetchHomelabStatus();
    // Starte direkt die erste Station
    playStation(currentPresets[0].url, currentPresets[0].name);
    displayNeedsUpdate = true;
    updateDisplay();
}

void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String html;
        html.reserve(16384);
        html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>RLCD WebRadio & Dashboard</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>";
        html += "* {box-sizing:border-box;}";
        html += "body {font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#141416;color:#f4f4f5;margin:0;padding:20px 12px;display:flex;justify-content:center;}";
        html += ".container {width:100%;max-width:520px;display:flex;flex-direction:column;gap:16px;}";
        html += "h1 {font-size:22px;margin:4px 0;text-align:center;color:#fafafa;}";
        html += ".card {background:#202024;border:1px solid #323238;border-radius:12px;padding:18px 20px;width:100%;box-shadow:0 4px 10px rgba(0,0,0,0.3);text-align:left;}";
        html += ".card h2 {font-size:18px;margin:0 0 10px 0;color:#38bdf8;border-bottom:1px solid #2e2e34;padding-bottom:8px;}";
        html += ".card h3 {font-size:16px;margin:0 0 10px 0;color:#f4f4f5;border-bottom:1px solid #2e2e34;padding-bottom:8px;}";
        html += "p {margin:7px 0;font-size:14px;line-height:1.45;}";
        html += ".btn {display:inline-block;padding:9px 14px;background:#3f3f46;color:#fff;border:none;border-radius:8px;cursor:pointer;font-size:14px;text-decoration:none;font-weight:600;text-align:center;transition:background 0.15s ease;}";
        html += ".btn:hover {background:#52525b;}";
        html += ".btn-primary {background:#2563eb;} .btn-primary:hover {background:#1d4ed8;}";
        html += ".btn-danger {background:#dc2626;} .btn-danger:hover {background:#b91c1c;}";
        html += ".btn-success {background:#10b981;} .btn-success:hover {background:#059669;}";
        html += ".btn-subtle {background:#27272a;border:1px solid #3f3f46;} .btn-subtle:hover {background:#3f3f46;}";
        html += ".badge-btn {display:inline-block;background:#27272a;color:#a1a1aa;border:1px solid #3f3f46;border-radius:6px;padding:4px 9px;font-size:12px;font-weight:600;cursor:pointer;text-decoration:none;transition:all 0.15s;}";
        html += ".badge-btn:hover {background:#3f3f46;color:#fff;}";
        html += ".result-item {background:#18181b;border:1px solid #2e2e34;border-radius:8px;padding:10px 12px;display:flex;flex-direction:column;gap:6px;}";
        html += ".result-title {font-weight:600;color:#f4f4f5;font-size:14px;}";
        html += ".result-meta {font-size:12px;color:#a1a1aa;}";
        html += ".tag {display:inline-block;padding:1px 5px;background:#27272a;border-radius:4px;font-size:11px;color:#38bdf8;margin-right:4px;}";
        html += ".slot-btn {background:#27272a;border:1px solid #3b82f6;color:#38bdf8;padding:5px 8px;border-radius:6px;font-size:12px;font-weight:bold;cursor:pointer;margin:2px;}";
        html += ".slot-btn:hover {background:#3b82f6;color:#fff;}";
        html += ".slot-picker {display:flex;gap:4px;align-items:center;margin-top:6px;background:#202024;padding:6px;border-radius:6px;flex-wrap:wrap;}";
        html += ".station-grid {display:grid;grid-template-columns:repeat(2, 1fr);gap:8px;margin-top:10px;}";
        html += ".vol-box {display:flex;align-items:center;gap:12px;margin:12px 0;background:#18181b;padding:8px 12px;border-radius:8px;border:1px solid #2e2e34;}";
        html += ".slider {-webkit-appearance:none;flex:1;height:8px;border-radius:4px;background:#3f3f46;outline:none;cursor:pointer;accent-color:#38bdf8;}";
        html += "input[type='time'], input[type='number'], select {background:#18181b;border:1px solid #3f3f46;color:#f4f4f5;border-radius:6px;padding:6px 10px;font-size:14px;}";
        html += "select {width:100%;box-sizing:border-box;}";
        html += ".info-row {display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #27272a;font-size:14px;}";
        html += ".info-row:last-child {border-bottom:none;}";
        html += "</style></head><body>";
        html += "<div class='container'>";
        html += "<h1>\xF0\x9F\x93\xBB Waveshare RLCD Dashboard</h1>";

        // Radio Card
        html += "<div class='card'>";
        html += "<h2>\xF0\x9F\x8E\xB5 Radio: <span id='stationName'>" + currentStation + "</span></h2>";
        html += "<p><strong>Titel:</strong> <span id='stationTitle'>" + currentTitle + "</span></p>";
        html += "<p><strong>Status:</strong> <span id='streamStat'>" + streamStatus + "</span> &nbsp;|&nbsp; <strong>Lautst&auml;rke:</strong> <span id='volVal'>" + String(currentVolume) + "</span>/21</p>";

        html += "<div class='vol-box'>";
        html += "<button type='button' onclick='changeVol(-1)' class='btn' style='font-size:18px;padding:4px 14px;'>&minus;</button>";
        html += "<input type='range' id='volSlider' min='0' max='21' value='" + String(currentVolume) + "' class='slider' oninput='setVol(this.value)'>";
        html += "<button type='button' onclick='changeVol(1)' class='btn' style='font-size:18px;padding:4px 14px;'>+</button>";
        html += "</div>";

        html += "<p style='margin-bottom:12px;'><button type='button' onclick='stopRadio()' class='btn btn-danger' style='width:100%;padding:10px 0;'>&#9632; Wiedergabe stoppen</button></p>";

        html += "<div style='display:flex;justify-content:space-between;align-items:center;margin:12px 0 6px 0;'>";
        html += "<span style='font-weight:600;color:#a1a1aa;'>Favoriten (Presets 1-5):</span>";
        html += "<button type='button' onclick='resetPresets()' class='badge-btn' title='Auf Werkssender zur&uuml;cksetzen'>&#8634; Standard</button>";
        html += "</div>";
        html += "<div class='station-grid'>";
        for (int i = 0; i < NUM_PRESETS; i++) {
            bool isCur = (currentPresetIdx == i);
            html += "<button type='button' onclick='playRadio(" + String(i) + ")' class='btn " + String(isCur ? "btn-primary" : "btn-subtle") + "' id='pBtn" + String(i) + "'>";
            html += String(i + 1) + ". " + currentPresets[i].name;
            html += "</button>";
        }
        html += "</div>";
        html += "</div>";

        // Sendersuche Card (Radio-Browser)
        html += "<div class='card'>";
        html += "<h3>\xF0\x9F\x94\x8D Radio-Browser Sendersuche</h3>";
        html += "<div style='display:flex;gap:6px;margin-bottom:8px;'>";
        html += "<input type='text' id='searchInput' placeholder='Sendername, Genre oder Land...' style='flex:1;background:#18181b;border:1px solid #3f3f46;color:#fff;border-radius:6px;padding:8px 10px;font-size:14px;' onkeyup='if(event.key===\"Enter\")searchRadio()'>";
        html += "<button type='button' onclick='searchRadio()' class='btn btn-primary' style='margin:0;padding:8px 14px;'>Suchen</button>";
        html += "</div>";

        html += "<div style='display:flex;flex-wrap:wrap;gap:4px;margin-bottom:12px;align-items:center;'>";
        html += "<span style='font-size:12px;color:#a1a1aa;margin-right:2px;'>Schnellwahl:</span>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"Rock\")'>Rock</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"Pop\")'>Pop</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"80s\")'>80s</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"90s\")'>90s</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"Metal\")'>Metal</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"Electro\")'>Electro</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"Jazz\")'>Jazz</button>";
        html += "<button type='button' class='badge-btn' onclick='quickSearch(\"News\")'>News</button>";
        html += "</div>";

        html += "<div id='searchResults' style='max-height:360px;overflow-y:auto;display:flex;flex-direction:column;gap:8px;'>";
        html += "<p style='color:#71717a;font-size:13px;text-align:center;margin:12px 0;'>Gib einen Suchbegriff ein oder w&auml;hle eine Schnellwahl.</p>";
        html += "</div>";
        html += "</div>";

        // Klang & Psychoakustik Card
        html += "<div class='card'>";
        html += "<h3>\xF0\x9F\x8E\x9B Klang & Psychoakustik</h3>";
        html += "<form method='POST' action='/sound/save'>";
        html += "<p><label><strong>Klangprofil:</strong></label><br>";
        html += "<select name='profile' style='width:100%;margin:6px 0 10px 0;'>";
        for (int i = 0; i < NUM_SOUND_PROFILES; i++) {
            html += "<option value='" + String(i) + "' " + String(currentSoundProfile == i ? "selected" : "") + ">" + String(SOUND_PROFILES[i].name) + " - " + String(SOUND_PROFILES[i].desc) + "</option>";
        }
        html += "</select></p>";

        html += "<p><label style='display:flex;align-items:center;cursor:pointer;gap:8px;'>";
        html += "<input type='checkbox' name='drc' value='1' " + String(drcEnabled ? "checked" : "") + " style='width:18px;height:18px;'> ";
        html += "<span><strong>Hardware-DRC</strong> <small style='color:#a1a1aa;'>(Dynamik-Kompression & Peak-Limiter)</small></span>";
        html += "</label></p>";

        html += "<p><label style='display:flex;align-items:center;cursor:pointer;gap:8px;'>";
        html += "<input type='checkbox' name='mono' value='1' " + String(forceMonoEnabled ? "checked" : "") + " style='width:18px;height:18px;'> ";
        html += "<span><strong>Mono-Phasenoptimierung</strong> <small style='color:#a1a1aa;'>(kein Ausl&ouml;schen)</small></span>";
        html += "</label></p>";

        html += "<button type='submit' class='btn btn-primary' style='width:100%;padding:10px;margin-top:6px;'>Klangprofil aktivieren</button>";
        html += "</form>";
        html += "</div>";

        // Wecker Card
        html += "<div class='card'>";
        html += "<h3>\xE2\x8F\xB0 Radio-Wecker</h3>";

        time_t curNow = time(nullptr);
        struct tm curTi;
        localtime_r(&curNow, &curTi);
        char clockBuf[16] = "--:--:--";
        if (curTi.tm_year >= 124) {
            snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d", curTi.tm_hour, curTi.tm_min, curTi.tm_sec);
        }

        html += "<div class='info-row' style='margin-bottom:12px;background:#18181b;padding:8px 10px;border-radius:6px;'>";
        html += "<span>Board-Uhrzeit:</span>";
        html += "<span><strong id='boardClock' style='color:#38bdf8;font-size:16px;'>" + String(clockBuf) + "</strong> ";
        html += "<small id='syncBadge' style='color:" + String(ntpSynced ? "#22c55e" : (curTi.tm_year >= 124 ? "#38bdf8" : "#ef4444")) + ";'>";
        html += ntpSynced ? "(NTP)" : (curTi.tm_year >= 124 ? "(RTC)" : "(nicht synchronisiert)");
        html += "</small></span></div>";

        html += "<form method='POST' action='/alarm/save'>";
        char curAlarmTime[8];
        snprintf(curAlarmTime, sizeof(curAlarmTime), "%02d:%02d", alarmHour, alarmMinute);
        html += "<p style='display:flex;align-items:center;justify-content:space-between;margin:10px 0;'>";
        html += "<label><strong>Weckzeit:</strong></label><input type='time' name='time' value='" + String(curAlarmTime) + "' required style='font-size:16px;padding:4px 8px;'></p>";

        html += "<p><label><strong>Weck-Sender:</strong></label><br>";
        html += "<select name='station' id='alarmStationSelect' style='width:100%;margin-top:6px;'>";
        for (int i = 0; i < NUM_PRESETS; i++) {
            html += "<option value='" + String(i) + "' " + String(alarmStation == i ? "selected" : "") + ">" + String(currentPresets[i].name) + "</option>";
        }
        html += "</select></p>";

        html += "<p style='display:flex;align-items:center;justify-content:space-between;margin:10px 0;'>";
        html += "<label><strong>Ziel-Lautst&auml;rke:</strong></label>";
        html += "<span><input type='number' name='vol' min='5' max='21' value='" + String(alarmVolume) + "' style='width:55px;'> / 21</span></p>";
        html += "<p style='margin:0 0 12px 0;'><small style='color:#a1a1aa;'>Sanfter Anstieg ab Lautst&auml;rke 5 beim Wecken</small></p>";

        html += "<div style='display:flex;gap:8px;'>";
        html += "<button type='submit' name='action' value='enable' class='btn btn-success' style='flex:1;padding:11px;font-size:14px;'>";
        html += alarmEnabled ? "&#128276; Gespeichert (AKTIV um " + String(curAlarmTime) + ")" : "&#128276; Wecker AKTIVIEREN";
        html += "</button>";
        if (alarmEnabled) {
            html += "<button type='submit' name='action' value='disable' class='btn btn-danger' style='padding:11px 14px;'>&#128277; Wecker AUS</button>";
        }
        html += "</div>";
        html += "</form>";

        if (isAlarmActive) {
            html += "<div style='margin-top:14px;padding:12px;background:#7f1d1d;border-radius:8px;text-align:center;'>";
            html += "<p style='font-weight:bold;color:#fca5a5;margin:4px 0;'>\xE2\x8F\xB0 WECKER KLINGELT AKTUELL!</p>";
            html += "<a href='/alarm/dismiss' class='btn btn-danger' style='margin:4px;'>Wecker ausschalten</a> ";
            html += "<a href='/alarm/snooze' class='btn' style='background:#f59e0b;margin:4px;'>\xF0\x9F\x92\xA4 Schlummern (9 Min)</a>";
            html += "</div>";
        } else if (snoozeActive) {
            char snzHdr[32];
            snprintf(snzHdr, sizeof(snzHdr), "%02d:%02d", snoozeHour, snoozeMinute);
            html += "<p style='color:#f59e0b;font-weight:bold;margin-top:10px;'>\xF0\x9F\x92\xA4 Schlummern aktiv bis " + String(snzHdr) + " Uhr &nbsp; <a href='/alarm/dismiss' class='btn' style='background:#b22;padding:4px 10px;font-size:12px;'>Abbrechen</a></p>";
        }
        html += "</div>";

        // Homelab & Wetter Card
        html += "<div class='card'>";
        html += "<h3>\xF0\x9F\x93\x8A Homelab & Wetter</h3>";
        html += "<div class='info-row'><span>Wetter:</span><span><strong>" + String(weather_temp_c, 1) + " &deg;C</strong> (" + String(getWmoText(weather_code)) + ")</span></div>";
        html += "<div class='info-row'><span>Regenwahrscheinlichkeit:</span><span>" + String(weather_rain_pct) + "%</span></div>";
        html += "<div class='info-row'><span>Min / Max Temperatur:</span><span>" + String(weather_t_min, 1) + " / " + String(weather_t_max, 1) + " &deg;C</span></div>";
        if (shtc3Available) {
            html += "<div class='info-row'><span>Raumklima (SHTC3):</span><span>" + String(roomTemp, 1) + " &deg;C | " + String(roomHumi, 0) + "% rF</span></div>";
        }
        html += "<div class='info-row'><span>Akku:</span><span>" + String(batteryPercent) + "% (" + String(batteryVoltage, 2) + " V)</span></div>";
        html += "<div class='info-row'><span>Homelab Verbrauch:</span><span>" + String(homelab_w, 1) + " W</span></div>";
        html += "<div class='info-row'><span>PVE CPU &amp; RAM:</span><span>" + String(pve_cpu_pct, 1) + "% | " + String(pve_ram_gb, 1) + " / " + String(pve_ram_total_gb, 1) + " GB</span></div>";
        html += "<p style='margin-top:12px;margin-bottom:0;'><small style='color:#71717a;'>Stand: " + serverGenerated + "</small></p>";
        html += "</div>";

        // System & Launcher Card
        html += "<div class='card' style='text-align:center;'>";
        html += "<div style='display:flex;gap:10px;justify-content:center;flex-wrap:wrap;'>";
        html += "<a href='/launcher' class='btn' style='background:#7c3aed;flex:1 1 180px;'>\xF0\x9F\x9A\x80 Zum MultiBoot Launcher</a>";
        html += "<a href='/update' class='btn btn-subtle' style='flex:1 1 180px;'>\xE2\x9A\x99 OTA Firmware Update</a>";
        html += "</div></div>";
        html += "</div>"; // Container end

        html += "<script>";
        html += "let curVol = " + String(currentVolume) + ";";
        html += "let debounceTimer = null;";
        html += "let foundStations = [];";
        html += "function setVol(val) {";
        html += "  curVol = parseInt(val);";
        html += "  document.getElementById('volVal').innerText = curVol;";
        html += "  document.getElementById('volSlider').value = curVol;";
        html += "  clearTimeout(debounceTimer);";
        html += "  debounceTimer = setTimeout(() => {";
        html += "    fetch('/vol?val=' + curVol + '&ajax=1').catch(e=>console.log(e));";
        html += "  }, 120);";
        html += "}";
        html += "function changeVol(delta) {";
        html += "  let nv = Math.max(0, Math.min(21, curVol + delta));";
        html += "  setVol(nv);";
        html += "}";
        html += "function playRadio(idx) {";
        html += "  fetch('/play?station=' + idx + '&ajax=1').then(()=>{setTimeout(()=>location.reload(), 600);}).catch(e=>console.log(e));";
        html += "}";
        html += "function stopRadio() {";
        html += "  fetch('/stop?ajax=1').then(()=>{setTimeout(()=>location.reload(), 400);}).catch(e=>console.log(e));";
        html += "}";
        html += "function escapeHtml(str) {";
        html += "  return (str || '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');";
        html += "}";
        html += "function quickSearch(tag) {";
        html += "  let el = document.getElementById('searchInput');";
        html += "  if (el) el.value = tag;";
        html += "  searchRadio();";
        html += "}";
        html += "function searchRadio() {";
        html += "  let q = document.getElementById('searchInput').value.trim();";
        html += "  if (!q) return;";
        html += "  let resDiv = document.getElementById('searchResults');";
        html += "  resDiv.innerHTML = '<p style=\"color:#a1a1aa;text-align:center;padding:12px;\">\xE2\x8F\xB3 Suche nach &quot;' + escapeHtml(q) + '&quot;...</p>';";
        html += "  let url = 'https://de1.api.radio-browser.info/json/stations/search?name=' + encodeURIComponent(q) + '&limit=12&order=votes&reverse=true';";
        html += "  fetch(url).then(r => r.json()).then(data => renderResults(data)).catch(err => {";
        html += "    fetch('https://all.api.radio-browser.info/json/stations/search?name=' + encodeURIComponent(q) + '&limit=12&order=votes&reverse=true')";
        html += "      .then(r => r.json()).then(data => renderResults(data))";
        html += "      .catch(e => { resDiv.innerHTML = '<p style=\"color:#ef4444;text-align:center;padding:12px;\">Fehler bei der Suche.</p>'; });";
        html += "  });";
        html += "}";
        html += "function renderResults(stations) {";
        html += "  foundStations = stations || [];";
        html += "  let resDiv = document.getElementById('searchResults');";
        html += "  if (!foundStations.length) {";
        html += "    resDiv.innerHTML = '<p style=\"color:#a1a1aa;text-align:center;padding:12px;\">Keine Sender gefunden.</p>';";
        html += "    return;";
        html += "  }";
        html += "  let h = '';";
        html += "  foundStations.forEach((s, idx) => {";
        html += "    let codec = (s.codec || 'MP3').toUpperCase();";
        html += "    let br = s.bitrate ? s.bitrate + 'k' : '';";
        html += "    let country = s.countrycode || s.country || '';";
        html += "    let name = (s.name || 'Unbekannt').trim();";
        html += "    h += '<div class=\"result-item\">';";
        html += "    h += '<div style=\"display:flex;justify-content:space-between;align-items:flex-start;gap:8px;\">';";
        html += "    h += '<div style=\"flex:1;min-width:160px;\">';";
        html += "    h += '<div class=\"result-title\">' + escapeHtml(name) + '</div>';";
        html += "    h += '<div class=\"result-meta\"><span class=\"tag\">' + escapeHtml(codec) + '</span>';";
        html += "    if (br) h += ' <span class=\"tag\">' + escapeHtml(br) + '</span>';";
        html += "    if (country) h += ' <span class=\"tag\">' + escapeHtml(country) + '</span>';";
        html += "    h += '</div></div>';";
        html += "    h += '<div style=\"display:flex;gap:6px;flex-shrink:0;\">';";
        html += "    h += '<button class=\"btn btn-success badge-btn\" onclick=\"playCustomIdx(' + idx + ')\">&#9654; Abspielen</button>';";
        html += "    h += '<button class=\"btn btn-subtle badge-btn\" onclick=\"showSlotPicker(' + idx + ')\">&#11088; Preset...</button>';";
        html += "    h += '</div></div>';";
        html += "    h += '<div id=\"slotPicker_' + idx + '\" class=\"slot-picker\" style=\"display:none;\">';";
        html += "    h += '<span style=\"font-size:12px;color:#a1a1aa;margin-right:4px;\">Auf Taste:</span>';";
        html += "    for (let slot = 0; slot < 5; slot++) {";
        html += "      h += '<button class=\"slot-btn\" onclick=\"assignPreset(' + idx + ',' + slot + ')\">P' + (slot + 1) + '</button>';";
        html += "    }";
        html += "    h += '<button class=\"slot-btn\" style=\"border-color:#71717a;color:#a1a1aa;\" onclick=\"hideSlotPicker(' + idx + ')\">&times;</button>';";
        html += "    h += '</div></div>';";
        html += "  });";
        html += "  resDiv.innerHTML = h;";
        html += "}";
        html += "function showSlotPicker(idx) {";
        html += "  let el = document.getElementById('slotPicker_' + idx);";
        html += "  if (el) el.style.display = 'flex';";
        html += "}";
        html += "function hideSlotPicker(idx) {";
        html += "  let el = document.getElementById('slotPicker_' + idx);";
        html += "  if (el) el.style.display = 'none';";
        html += "}";
        html += "function playCustomIdx(idx) {";
        html += "  let s = foundStations[idx];";
        html += "  if (!s) return;";
        html += "  let streamUrl = s.url_resolved || s.url;";
        html += "  let name = (s.name || 'Custom Stream').trim();";
        html += "  fetch('/play?url=' + encodeURIComponent(streamUrl) + '&name=' + encodeURIComponent(name) + '&ajax=1')";
        html += "    .then(() => { setTimeout(pollStatus, 800); })";
        html += "    .catch(e => console.log(e));";
        html += "}";
        html += "function assignPreset(idx, slot) {";
        html += "  let s = foundStations[idx];";
        html += "  if (!s) return;";
        html += "  let streamUrl = s.url_resolved || s.url;";
        html += "  let name = (s.name || 'Station ' + (slot + 1)).trim();";
        html += "  hideSlotPicker(idx);";
        html += "  fetch('/preset/save?slot=' + slot + '&name=' + encodeURIComponent(name) + '&url=' + encodeURIComponent(streamUrl))";
        html += "    .then(r => r.json())";
        html += "    .then(d => {";
        html += "      if (d.result === 'ok') {";
        html += "        let btn = document.getElementById('pBtn' + slot);";
        html += "        if (btn) btn.innerText = (slot + 1) + '. ' + name;";
        html += "        let sel = document.getElementById('alarmStationSelect');";
        html += "        if (sel && sel.options[slot]) sel.options[slot].text = name;";
        html += "      }";
        html += "    }).catch(e => console.log(e));";
        html += "}";
        html += "function resetPresets() {";
        html += "  if (!confirm('Alle Presets (1-5) auf Standard-Sender zur\\u00fccksetzen?')) return;";
        html += "  fetch('/preset/reset').then(r => r.json()).then(d => {";
        html += "    if (d.result === 'ok') location.reload();";
        html += "  }).catch(e => console.log(e));";
        html += "}";
        html += "function pollStatus() {";
        html += "  fetch('/status').then(r=>r.json()).then(d=>{";
        html += "    if (d.title && document.getElementById('stationTitle')) document.getElementById('stationTitle').innerText = d.title;";
        html += "    if (d.station && document.getElementById('stationName')) document.getElementById('stationName').innerText = d.station;";
        html += "    if (d.status && document.getElementById('streamStat')) document.getElementById('streamStat').innerText = d.status;";
        html += "    let slider = document.getElementById('volSlider');";
        html += "    if (d.volume !== undefined && slider && document.activeElement !== slider) {";
        html += "      curVol = d.volume;";
        html += "      document.getElementById('volVal').innerText = curVol;";
        html += "      slider.value = curVol;";
        html += "    }";
        html += "    if (d.clock && document.getElementById('boardClock')) {";
        html += "      document.getElementById('boardClock').innerText = d.clock;";
        html += "    }";
        html += "    if (d.time_synced !== undefined && document.getElementById('syncBadge')) {";
        html += "      document.getElementById('syncBadge').innerText = d.time_synced ? (d.ntp_synced ? '(NTP)' : '(RTC)') : '(nicht synchronisiert)';";
        html += "      document.getElementById('syncBadge').style.color = d.time_synced ? '#22c55e' : '#ef4444';";
        html += "    }";
        html += "    if (d.presets) {";
        html += "      d.presets.forEach((p, i) => {";
        html += "        let btn = document.getElementById('pBtn' + i);";
        html += "        if (btn) btn.innerText = (i + 1) + '. ' + p.name;";
        html += "        let sel = document.getElementById('alarmStationSelect');";
        html += "        if (sel && sel.options[i]) sel.options[i].text = p.name;";
        html += "      });";
        html += "    }";
        html += "  }).catch(()=>{});";
        html += "}";
        html += "setInterval(pollStatus, 4000);";
        html += "</script></body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, []() {
        JsonDocument doc;
        doc["status"] = streamStatus;
        doc["station"] = currentStation;
        doc["title"] = currentTitle;
        doc["volume"] = currentVolume;
        doc["ip"] = WiFi.localIP().toString();

        time_t now = time(nullptr);
        struct tm ti;
        localtime_r(&now, &ti);
        if (ti.tm_year >= 124) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
            doc["clock"] = tbuf;
            doc["time_synced"] = true;
            doc["rtc_available"] = rtcAvailable;
            doc["ntp_synced"] = ntpSynced;
        } else {
            doc["clock"] = "--:--:--";
            doc["time_synced"] = false;
            doc["rtc_available"] = rtcAvailable;
            doc["ntp_synced"] = false;
        }

        JsonObject bat = doc["battery"].to<JsonObject>();
        bat["voltage"] = batteryVoltage;
        bat["percent"] = batteryPercent;

        JsonObject room = doc["room"].to<JsonObject>();
        room["temp_c"] = roomTemp;
        room["humi_pct"] = roomHumi;
        room["available"] = shtc3Available;
        room["chip_temp_c"] = temperatureRead();

        JsonObject w = doc["wetter"].to<JsonObject>();
        w["temp_c"] = weather_temp_c;
        w["t_min"] = weather_t_min;
        w["t_max"] = weather_t_max;
        w["rain_pct"] = weather_rain_pct;
        w["code"] = weather_code;
        w["text"] = getWmoText(weather_code);

        JsonObject h = doc["homelab"].to<JsonObject>();
        h["watts"] = homelab_w;
        h["cpu_pct"] = pve_cpu_pct;
        h["temp_c"] = pve_temp_c;
        h["ram_gb"] = pve_ram_gb;
        h["ram_total"] = pve_ram_total_gb;
        h["vms_running"] = vms_running;

        doc["generated"] = serverGenerated;

        JsonObject alm = doc["alarm"].to<JsonObject>();
        alm["enabled"] = alarmEnabled;
        alm["hour"] = alarmHour;
        alm["minute"] = alarmMinute;
        alm["station"] = alarmStation;
        alm["station_name"] = currentPresets[alarmStation].name;
        alm["volume"] = alarmVolume;
        alm["is_active"] = isAlarmActive;
        alm["snooze_active"] = snoozeActive;

        JsonArray pa = doc["presets"].to<JsonArray>();
        for (int i = 0; i < NUM_PRESETS; i++) {
            JsonObject p = pa.add<JsonObject>();
            p["name"] = currentPresets[i].name;
            p["url"] = currentPresets[i].url;
        }

        JsonObject snd = doc["sound"].to<JsonObject>();
        snd["profile"] = currentSoundProfile;
        snd["profile_name"] = SOUND_PROFILES[currentSoundProfile].name;
        snd["drc"] = drcEnabled;
        snd["force_mono"] = forceMonoEnabled;

        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/alarm/save", HTTP_ANY, []() {
        if (server.hasArg("action")) {
            String act = server.arg("action");
            if (act == "enable") {
                alarmEnabled = true;
            } else if (act == "disable") {
                alarmEnabled = false;
            }
        } else if (server.hasArg("enabled")) {
            alarmEnabled = (server.arg("enabled") == "1" || server.arg("enabled") == "on");
        }

        if (server.hasArg("time")) {
            String t = server.arg("time");
            int colon = t.indexOf(':');
            if (colon > 0) {
                alarmHour = t.substring(0, colon).toInt();
                alarmMinute = t.substring(colon + 1).toInt();
            }
        }
        if (server.hasArg("station")) {
            alarmStation = constrain(server.arg("station").toInt(), 0, NUM_PRESETS - 1);
        }
        if (server.hasArg("vol")) {
            alarmVolume = constrain(server.arg("vol").toInt(), 5, 21);
        }
        alarmTriggeredThisMinute = false;
        saveAlarmSettings();
        displayNeedsUpdate = true;
        Serial.printf("[ALARM] Gespeichert: %s um %02d:%02d, Sender %d, Vol %d\n",
                      alarmEnabled ? "EIN" : "AUS", alarmHour, alarmMinute, alarmStation + 1, alarmVolume);
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/alarm/dismiss", HTTP_GET, []() {
        dismissAlarm();
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/alarm/snooze", HTTP_GET, []() {
        snoozeAlarm();
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/sound/save", HTTP_ANY, []() {
        if (server.hasArg("profile")) {
            currentSoundProfile = constrain(server.arg("profile").toInt(), 0, NUM_SOUND_PROFILES - 1);
        }
        drcEnabled = server.hasArg("drc") && (server.arg("drc") == "1" || server.arg("drc") == "on");
        forceMonoEnabled = server.hasArg("mono") && (server.arg("mono") == "1" || server.arg("mono") == "on");

        saveSoundSettings();
        applySoundSettings();

        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/play", HTTP_GET, []() {
        if (server.hasArg("station")) {
            int idx = server.arg("station").toInt();
            if (idx >= 0 && idx < NUM_PRESETS) {
                currentPresetIdx = idx;
                playStation(currentPresets[idx].url, currentPresets[idx].name);
            }
        } else if (server.hasArg("url")) {
            String url = server.arg("url");
            String name = server.hasArg("name") ? server.arg("name") : "Custom Stream";
            playStation(url, name);
        }
        if (server.hasArg("ajax") || server.hasHeader("X-Requested-With")) {
            server.send(200, "application/json", "{\"result\":\"ok\"}");
        } else {
            server.sendHeader("Location", "/");
            server.send(303);
        }
    });

    server.on("/stop", HTTP_GET, []() {
        stopStation();
        if (server.hasArg("ajax") || server.hasHeader("X-Requested-With")) {
            server.send(200, "application/json", "{\"result\":\"stopped\"}");
        } else {
            server.sendHeader("Location", "/");
            server.send(303);
        }
    });

    server.on("/vol", HTTP_GET, []() {
        if (server.hasArg("val")) {
            setVolume(server.arg("val").toInt());
        }
        if (server.hasArg("ajax") || server.hasHeader("X-Requested-With")) {
            server.send(200, "application/json", "{\"volume\":" + String(currentVolume) + "}");
        } else {
            server.sendHeader("Location", "/");
            server.send(303);
        }
    });

    server.on("/fetch", HTTP_GET, []() {
        fetchHomelabStatus();
        server.send(200, "application/json", "{\"result\":\"fetched\"}");
    });

    server.on("/preset/save", HTTP_ANY, []() {
        if (server.hasArg("slot") && server.hasArg("name") && server.hasArg("url")) {
            int slot = server.arg("slot").toInt();
            String name = server.arg("name");
            String url = server.arg("url");
            if (slot >= 0 && slot < NUM_PRESETS) {
                savePreset(slot, name, url);
                displayNeedsUpdate = true;
                server.send(200, "application/json", "{\"result\":\"ok\",\"slot\":" + String(slot) + "}");
                return;
            }
        }
        server.send(400, "application/json", "{\"result\":\"error\",\"message\":\"invalid params\"}");
    });

    server.on("/preset/reset", HTTP_ANY, []() {
        resetPresets();
        displayNeedsUpdate = true;
        server.send(200, "application/json", "{\"result\":\"ok\"}");
    });

    // Web OTA Update
    server.on("/launcher", HTTP_GET, []() {
        server.send(200, "text/html", "<meta http-equiv='refresh' content='4;url=/'><div style='padding:30px;background:#222;color:#8b5cf6;font-family:sans-serif;'><h2>Starte MultiBoot-Launcher...</h2><p>ESP32 startet neu...</p></div>");
        delay(600);
        const esp_partition_t *launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (launcher) {
            esp_ota_set_boot_partition(launcher);
            ESP.restart();
        }
    });

    server.on("/update", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>OTA Firmware Update</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;background:#222;color:#eee;text-align:center;padding:30px;}";
        html += ".card{background:#333;padding:25px;border-radius:10px;display:inline-block;max-width:450px;width:100%;}";
        html += "input[type=file]{margin:20px 0;color:#ccc;font-size:15px;}";
        html += ".btn{padding:12px 24px;background:#0088cc;color:#fff;border:none;border-radius:6px;font-size:16px;cursor:pointer;font-weight:bold;}";
        html += ".btn:hover{background:#00aaff;}</style></head><body>";
        html += "<div class='card'><h2>RLCD Firmware OTA Update</h2>";
        html += "<p style='color:#bbb;'>W&auml;hle die kompilierte <code>rlcd_webradio.ino.bin</code> Datei aus:</p>";
        html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
        html += "<input type='file' name='update' accept='.bin' required><br>";
        html += "<input type='submit' class='btn' value='Firmware flashen' onclick=\"this.value='Flashe... Bitte warten...';\">";
        html += "</form><br><br><a href='/' style='color:#888;text-decoration:none;'>&larr; Zur&uuml;ck zum Dashboard</a></div></body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(200, "text/html", "<div style='background:#222;color:#f55;padding:40px;text-align:center;font-family:sans-serif;'><h2>Update FEHLGESCHLAGEN!</h2><p><a href='/update' style='color:#fff;'>Erneut versuchen</a></p></div>");
        } else {
            server.send(200, "text/html", "<meta http-equiv='refresh' content='10;url=/'><div style='background:#222;color:#5f5;padding:40px;text-align:center;font-family:sans-serif;'><h2>Update erfolgreich!</h2><p>ESP32 startet neu und leitet gleich weiter...</p></div>");
            delay(1000);
            ESP.restart();
        }
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            digitalWrite(PA_ENABLE, LOW);
            audio.stopSong();
            isPlaying = false;
            es.standby();
            Serial.printf("[OTA Web] Start: %s\n", upload.filename.c_str());
            if (u8g2) {
                u8g2->clearBuffer();
                u8g2->setFont(u8g2_font_helvB14_tr);
                u8g2->drawStr(40, 100, "OTA WEB UPDATE");
                u8g2->setFont(u8g2_font_helvR10_tr);
                u8g2->drawStr(40, 140, "Empfange Firmware...");
                u8g2->sendBuffer();
            }
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA Web] Erfolgreich: %u Bytes\n", upload.totalSize);
                if (u8g2) {
                    u8g2->clearBuffer();
                    u8g2->setFont(u8g2_font_helvB14_tr);
                    u8g2->drawStr(40, 120, "Update Erfolgreich!");
                    u8g2->setFont(u8g2_font_helvR10_tr);
                    u8g2->drawStr(40, 160, "Neustart...");
                    u8g2->sendBuffer();
                }
            } else {
                Update.printError(Serial);
            }
        }
    });

    server.begin();
    Serial.println("[OK] Webserver gestartet!");
}

void setupOTA() {
    ArduinoOTA.setHostname("rlcd-dashboard");

    ArduinoOTA.onStart([]() {
        digitalWrite(PA_ENABLE, LOW);
        audio.stopSong();
        isPlaying = false;
        es.standby();
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {
            type = "filesystem";
        }
        Serial.println("[OTA Arduino] Start updating " + type);
        if (u8g2) {
            u8g2->clearBuffer();
            u8g2->setFont(u8g2_font_helvB14_tr);
            u8g2->drawStr(40, 100, "ARDUINO OTA");
            u8g2->setFont(u8g2_font_helvR10_tr);
            u8g2->drawStr(40, 140, "Flashe Firmware...");
            u8g2->sendBuffer();
        }
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA Arduino] End");
        if (u8g2) {
            u8g2->clearBuffer();
            u8g2->setFont(u8g2_font_helvB14_tr);
            u8g2->drawStr(40, 120, "OTA Fertig!");
            u8g2->setFont(u8g2_font_helvR10_tr);
            u8g2->drawStr(40, 160, "Neustart...");
            u8g2->sendBuffer();
        }
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static unsigned int lastPct = 0;
        unsigned int pct = progress / (total / 100);
        if (pct >= lastPct + 10 || pct == 100) {
            lastPct = pct;
            Serial.printf("[OTA] %u%%\n", pct);
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA Fehler] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
        if (u8g2) {
            u8g2->clearBuffer();
            u8g2->setFont(u8g2_font_helvB14_tr);
            u8g2->drawStr(40, 120, "OTA FEHLER!");
            u8g2->sendBuffer();
        }
    });

    ArduinoOTA.begin();
    Serial.println("[OK] ArduinoOTA initialisiert (Hostname: rlcd-dashboard)");
}

void playStation(const String &url, const String &name) {
    es.resume();                   // Codec wecken
    digitalWrite(PA_ENABLE, HIGH); // Audio-Endstufe einschalten
    currentStation = name;
    currentTitle = "Puffere Stream...";
    streamStatus = "PLAYING";
    isPlaying = true;
    displayNeedsUpdate = true;
    Serial.printf("Starte Stream: %s (%s), PA an\n", name.c_str(), url.c_str());
    audio.connecttohost(url.c_str());
}

void stopStation() {
    digitalWrite(PA_ENABLE, LOW); // Endstufe sofort abschalten (knackfrei)
    audio.stopSong();
    es.standby();                 // Codec in Standby
    isPlaying = false;
    streamStatus = "STOPPED";
    currentTitle = "Gestoppt";
    displayNeedsUpdate = true;
    Serial.println("Stream gestoppt. PA AUS, Codec Standby.");
}

void setVolume(int vol) {
    currentVolume = constrain(vol, 0, 21);
    audio.setVolume(currentVolume);
    displayNeedsUpdate = true;
    Serial.printf("Lautstaerke: %d/21\n", currentVolume);
}

void applySoundSettings() {
    audio.forceMono(forceMonoEnabled);
    const SoundProfile &p = SOUND_PROFILES[currentSoundProfile];
    audio.setTone(p.gainLow, p.gainMid, p.gainHigh);
    es.setDRC(drcEnabled);
    Serial.printf("[AUDIO DSP] Profil [%d]: %s (B:%.1f M:%.1f H:%.1f), DRC: %s, Mono: %s\n",
                  currentSoundProfile, p.name, p.gainLow, p.gainMid, p.gainHigh,
                  drcEnabled ? "EIN" : "AUS", forceMonoEnabled ? "EIN" : "AUS");
}

void saveSoundSettings() {
    Preferences soundPrefs;
    soundPrefs.begin("radio_sound", false);
    soundPrefs.putInt("profile", currentSoundProfile);
    soundPrefs.putBool("drc", drcEnabled);
    soundPrefs.putBool("mono", forceMonoEnabled);
    soundPrefs.end();
}

void loadSoundSettings() {
    Preferences soundPrefs;
    soundPrefs.begin("radio_sound", true);
    currentSoundProfile = constrain(soundPrefs.getInt("profile", 0), 0, NUM_SOUND_PROFILES - 1);
    drcEnabled = soundPrefs.getBool("drc", true);
    forceMonoEnabled = soundPrefs.getBool("mono", true);
    soundPrefs.end();
}

void loadPresets() {
    Preferences presetPrefs;
    presetPrefs.begin("presets", true);
    for (int i = 0; i < NUM_PRESETS; i++) {
        String nKey = "n" + String(i);
        String uKey = "u" + String(i);
        String sName = presetPrefs.getString(nKey.c_str(), "");
        String sUrl  = presetPrefs.getString(uKey.c_str(), "");
        if (sName.length() > 0 && sUrl.length() > 0) {
            currentPresets[i].name = sName;
            currentPresets[i].url  = sUrl;
        } else {
            currentPresets[i].name = PRESET_STATIONS[i].name;
            currentPresets[i].url  = PRESET_STATIONS[i].url;
        }
    }
    presetPrefs.end();
}

void savePreset(int slot, const String &name, const String &url) {
    if (slot < 0 || slot >= NUM_PRESETS) return;
    currentPresets[slot].name = name;
    currentPresets[slot].url  = url;

    Preferences presetPrefs;
    presetPrefs.begin("presets", false);
    String nKey = "n" + String(slot);
    String uKey = "u" + String(slot);
    presetPrefs.putString(nKey.c_str(), name);
    presetPrefs.putString(uKey.c_str(), url);
    presetPrefs.end();
    Serial.printf("[PRESET] Slot %d belegt mit '%s' (%s)\n", slot + 1, name.c_str(), url.c_str());
}

void resetPresets() {
    Preferences presetPrefs;
    presetPrefs.begin("presets", false);
    presetPrefs.clear();
    presetPrefs.end();
    for (int i = 0; i < NUM_PRESETS; i++) {
        currentPresets[i].name = PRESET_STATIONS[i].name;
        currentPresets[i].url  = PRESET_STATIONS[i].url;
    }
    Serial.println("[PRESET] Alle Presets auf Werkssender zurueckgesetzt.");
}

void saveAlarmSettings() {
    Preferences alarmPrefs;
    alarmPrefs.begin("radio_alarm", false);
    alarmPrefs.putBool("enabled", alarmEnabled);
    alarmPrefs.putInt("hour", alarmHour);
    alarmPrefs.putInt("min", alarmMinute);
    alarmPrefs.putInt("station", alarmStation);
    alarmPrefs.putInt("vol", alarmVolume);
    alarmPrefs.end();
}

void loadAlarmSettings() {
    Preferences alarmPrefs;
    alarmPrefs.begin("radio_alarm", true);
    alarmEnabled = alarmPrefs.getBool("enabled", false);
    alarmHour = alarmPrefs.getInt("hour", 7);
    alarmMinute = alarmPrefs.getInt("min", 0);
    alarmStation = constrain(alarmPrefs.getInt("station", 0), 0, NUM_PRESETS - 1);
    alarmVolume = constrain(alarmPrefs.getInt("vol", 14), 5, 21);
    alarmPrefs.end();
}

void triggerAlarm() {
    isAlarmActive = true;
    isRamping = true;
    lastRampTime = millis();
    snoozeActive = false;
    currentVolume = 5; // Sanfter Start
    setVolume(currentVolume);
    currentPresetIdx = alarmStation;
    playStation(currentPresets[alarmStation].url, currentPresets[alarmStation].name);
    displayNeedsUpdate = true;
    Serial.printf("[WECKER] *** WECKER AUSGELOEST! *** Sender: %s, Ziel-Lautstaerke: %d\n",
                  currentPresets[alarmStation].name.c_str(), alarmVolume);
}

void dismissAlarm() {
    isAlarmActive = false;
    isRamping = false;
    snoozeActive = false;
    stopStation();
    displayNeedsUpdate = true;
    Serial.println("[WECKER] Wecker ausgeschaltet (Dismiss).");
}

void snoozeAlarm() {
    isAlarmActive = false;
    isRamping = false;
    stopStation();

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        int m = timeinfo.tm_min + 9;
        int h = timeinfo.tm_hour;
        if (m >= 60) {
            m -= 60;
            h = (h + 1) % 24;
        }
        snoozeHour = h;
        snoozeMinute = m;
        snoozeActive = true;
        Serial.printf("[WECKER] Schlummerfunktion aktiv bis %02d:%02d\n", snoozeHour, snoozeMinute);
    }
    displayNeedsUpdate = true;
}

void checkAlarm(int curHour, int curMin) {
    if (curMin != lastAlarmCheckMinute) {
        lastAlarmCheckMinute = curMin;
        alarmTriggeredThisMinute = false;
    }

    if (!alarmTriggeredThisMinute && !isAlarmActive) {
        if (snoozeActive && curHour == snoozeHour && curMin == snoozeMinute) {
            alarmTriggeredThisMinute = true;
            snoozeActive = false;
            triggerAlarm();
        } else if (alarmEnabled && curHour == alarmHour && curMin == alarmMinute) {
            alarmTriggeredThisMinute = true;
            triggerAlarm();
        }
    }
}

void updateTime() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    bool validTime = false;

    if (timeinfo.tm_year >= 124) { // Jahr >= 2024
        validTime = true;
    } else if (rtcAvailable && pcf85063_read_time(&timeinfo)) {
        if (timeinfo.tm_year >= 124) {
            validTime = true;
            time_t t = mktime(&timeinfo);
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
    }

    if (validTime) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        snprintf(dateStr, sizeof(dateStr), "%02d.%02d.", timeinfo.tm_mday, timeinfo.tm_mon + 1);

        if (timeinfo.tm_min != lastMinute) {
            lastMinute = timeinfo.tm_min;
            displayNeedsUpdate = true;
        }

        checkAlarm(timeinfo.tm_hour, timeinfo.tm_min);
    }
}


void updateDisplay() {
    u8g2->clearBuffer();
    u8g2->setDrawColor(1);

    // Outer Border
    u8g2->drawFrame(2, 2, 396, 296);

    // --- Header Bar ---
    u8g2->setFont(u8g2_font_helvB10_tr);
    u8g2->drawStr(8, 20, "DASHBOARD");

    u8g2->setFont(u8g2_font_6x10_tr);
    // Zeitstempel ohne Sekunden formatieren (z. B. "20:58" oder von Homelab)
    String curTimeStr = (timeStr[0] != '-') ? String(timeStr) : serverGenerated;
    if (curTimeStr.length() >= 14 && curTimeStr.charAt(curTimeStr.length() - 3) == ':') {
        curTimeStr = curTimeStr.substring(0, curTimeStr.length() - 3);
    }
    if (curTimeStr.length() > 0 && curTimeStr != "--:--") {
        u8g2->drawStr(98, 20, curTimeStr.c_str());
    }

    // Wecker-Status im Header
    if (alarmEnabled) {
        char almHdr[16];
        snprintf(almHdr, sizeof(almHdr), "ALM %02d:%02d", alarmHour, alarmMinute);
        u8g2->drawStr(138, 20, almHdr);
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ipStr = WiFi.localIP().toString();
        u8g2->drawStr(212, 20, ipStr.c_str());
    } else {
        u8g2->drawStr(212, 20, "Kein WLAN");
    }

    // Battery Icon & Status (Top-Right)
    int batX = 314;
    int batY = 11;
    u8g2->drawFrame(batX, batY, 18, 10);
    u8g2->drawBox(batX + 18, batY + 3, 2, 4);
    int fillW = map(constrain(batteryPercent, 0, 100), 0, 100, 0, 14);
    if (fillW > 0) {
        u8g2->drawBox(batX + 2, batY + 2, fillW, 6);
    }
    char batStr[24];
    snprintf(batStr, sizeof(batStr), "%d%% %.1fV", batteryPercent, batteryVoltage);
    u8g2->drawStr(batX + 22, 20, batStr);

    u8g2->drawHLine(2, 25, 396);

    // --- Radio Section (Top half) ---
    // Station Name or Alarm Banner
    u8g2->setFont(u8g2_font_helvB14_tr);
    if (isAlarmActive) {
        u8g2->drawStr(12, 45, "*** WECKER AKTIV ***");
    } else {
        u8g2->drawStr(12, 45, currentStation.c_str());
    }

    // Status Badge & Vol
    u8g2->setFont(u8g2_font_6x10_tr);
    String statBadge = "[" + streamStatus + "] Vol: " + String(currentVolume) + "/21";
    u8g2->drawStr(250, 45, statBadge.c_str());

    // Volume bar (mini)
    int barW = map(currentVolume, 0, 21, 0, 50);
    u8g2->drawFrame(340, 36, 52, 9);
    u8g2->drawBox(341, 37, barW, 7);

    // Current Song Title (1 or 2 lines)
    u8g2->setFont(u8g2_font_helvR12_tr);
    if (currentTitle.length() > 38) {
        String l1 = currentTitle.substring(0, 36);
        String l2 = currentTitle.substring(36);
        u8g2->drawStr(12, 68, l1.c_str());
        u8g2->drawStr(12, 88, l2.c_str());
    } else {
        u8g2->drawStr(12, 74, currentTitle.c_str());
    }

    // Mid Divider Line
    u8g2->drawHLine(2, 100, 396);
    // Vertical Split Line between Wetter & Homelab
    u8g2->drawVLine(200, 100, 172);

    // --- Left Column: WETTER & RAUM ---
    u8g2->setFont(u8g2_font_helvB10_tr);
    u8g2->drawStr(10, 118, "AUSSENWETTER");

    if (hasServerData) {
        // Temperature Large
        char tempBuf[32];
        snprintf(tempBuf, sizeof(tempBuf), "%.1f \xb0\x43", weather_temp_c);
        u8g2->setFont(u8g2_font_helvB18_tr);
        u8g2->drawStr(10, 145, tempBuf);

        // Min / Max
        char minMaxBuf[32];
        snprintf(minMaxBuf, sizeof(minMaxBuf), "Min: %.1f / Max: %.1f", weather_t_min, weather_t_max);
        u8g2->setFont(u8g2_font_6x10_tr);
        u8g2->drawStr(10, 165, minMaxBuf);

        // Condition Text & Rain
        u8g2->setFont(u8g2_font_helvR10_tr);
        u8g2->drawStr(10, 187, getWmoText(weather_code));

        char rainBuf[32];
        snprintf(rainBuf, sizeof(rainBuf), "Regenrisiko: %d%%", weather_rain_pct);
        u8g2->setFont(u8g2_font_6x10_tr);
        u8g2->drawStr(10, 205, rainBuf);
    } else {
        u8g2->setFont(u8g2_font_6x10_tr);
        u8g2->drawStr(10, 160, "Lade Wetterdaten...");
    }

    // Divider Line above Room Climate
    u8g2->drawHLine(6, 218, 188);

    // Room Climate (SHTC3)
    u8g2->setFont(u8g2_font_helvB10_tr);
    u8g2->drawStr(10, 236, "RAUMKLIMA (SHTC3)");
    u8g2->setFont(u8g2_font_helvR10_tr);
    if (shtc3Available) {
        char roomBuf[36];
        snprintf(roomBuf, sizeof(roomBuf), "%.1f \xb0\x43   |   %.0f%% rF", roomTemp, roomHumi);
        u8g2->drawStr(10, 258, roomBuf);
    } else {
        u8g2->drawStr(10, 258, "Sensor n.v.");
    }

    // --- Right Column: HOMELAB & PVE ---
    u8g2->setFont(u8g2_font_helvB10_tr);
    u8g2->drawStr(210, 118, "HOMELAB & PVE");

    if (hasServerData) {
        // Homelab Power in large font
        char wBuf[32];
        snprintf(wBuf, sizeof(wBuf), "%.1f W", homelab_w);
        u8g2->setFont(u8g2_font_helvB18_tr);
        u8g2->drawStr(210, 150, wBuf);

        // PVE CPU & Temp
        char pveCpuBuf[40];
        snprintf(pveCpuBuf, sizeof(pveCpuBuf), "CPU: %.1f%%  |  %.1f \xb0\x43", pve_cpu_pct, pve_temp_c);
        u8g2->setFont(u8g2_font_6x10_tr);
        u8g2->drawStr(210, 175, pveCpuBuf);

        // RAM Usage
        char ramBuf[40];
        snprintf(ramBuf, sizeof(ramBuf), "RAM: %.1f / %.1f GB", pve_ram_gb, pve_ram_total_gb);
        u8g2->drawStr(210, 200, ramBuf);

        // VMs Running
        char vmBuf[40];
        snprintf(vmBuf, sizeof(vmBuf), "VMs: %d aktiv / %d gesamt", vms_running, vms_total);
        u8g2->drawStr(210, 225, vmBuf);
    } else {
        u8g2->setFont(u8g2_font_6x10_tr);
        u8g2->drawStr(210, 160, "Lade Homelab...");
    }

    // --- Bottom Bar: Button Guide ---
    u8g2->drawHLine(2, 272, 396);
    u8g2->setFont(u8g2_font_6x10_tr);
    if (isAlarmActive) {
        u8g2->drawStr(10, 288, "[BOOT] Schlummern (9 Min)    [KEY] Wecker ausschalten");
    } else if (snoozeActive) {
        char snzBuf[64];
        snprintf(snzBuf, sizeof(snzBuf), "Schlummern bis %02d:%02d | [KEY] Aus", snoozeHour, snoozeMinute);
        u8g2->drawStr(10, 288, snzBuf);
    } else {
        char footerStr[64];
        snprintf(footerStr, sizeof(footerStr), "[BOOT] Sender/Vol-  [KEY] Play/Vol+  | DSP: %s", SOUND_PROFILES[currentSoundProfile].name);
        u8g2->drawStr(10, 288, footerStr);
    }

    u8g2->sendBuffer();
    displayNeedsUpdate = false;
    lastDisplayUpdate = millis();
}

void handleSerialCommands() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return;

        Serial.printf("[COMMAND] %s\n", line.c_str());

        if (line.startsWith("PLAY ")) {
            String url = line.substring(5);
            playStation(url, "Custom URL");
        } else if (line.startsWith("STATION ")) {
            int idx = line.substring(8).toInt();
            if (idx >= 0 && idx < NUM_PRESETS) {
                currentPresetIdx = idx;
                playStation(currentPresets[idx].url, currentPresets[idx].name);
            }
        } else if (line == "STOP") {
            stopStation();
        } else if (line.startsWith("VOL ")) {
            int v = line.substring(4).toInt();
            setVolume(v);
        } else if (line == "FETCH") {
            fetchHomelabStatus();
        } else if (line.startsWith("WIFI ")) {
            int spaceIdx = line.indexOf(' ', 5);
            if (spaceIdx > 0) {
                String newSsid = line.substring(5, spaceIdx);
                String newPass = line.substring(spaceIdx + 1);
                prefs.putString("ssid", newSsid);
                prefs.putString("pass", newPass);
                Serial.printf("[WIFI] Gespeichert: %s. Starte neu...\n", newSsid.c_str());
                ESP.restart();
            }
        } else if (line == "BAT" || line == "BATTERY") {
            readBattery();
            Serial.printf("Akku: %.2f V (%d%%)\n", batteryVoltage, batteryPercent);
        } else if (line == "STATUS") {
            readBattery();
            Serial.printf("Status: %s, Station: %s, Title: %s, Vol: %d, IP: %s\n",
                          streamStatus.c_str(), currentStation.c_str(), currentTitle.c_str(),
                          currentVolume, WiFi.localIP().toString().c_str());
            Serial.printf("Wetter: %.1f C, Regen: %d%%, Homelab: %.1f W, CPU: %.1f%%\n",
                          weather_temp_c, weather_rain_pct, homelab_w, pve_cpu_pct);
            Serial.printf("Raum: %.1f C, %.1f%% rF | Akku: %.2f V (%d%%)\n",
                          roomTemp, roomHumi, batteryVoltage, batteryPercent);
        } else if (line == "ROOM" || line == "SHTC3") {
            if (shtc3Available) {
                float t, h;
                if (shtc3_read(&t, &h)) { roomTemp = t; roomHumi = h; }
                Serial.printf("SHTC3: %.1f C, %.1f%% rF\n", roomTemp, roomHumi);
            } else {
                Serial.println("SHTC3: Nicht verfuegbar");
            }
        }
    }
}

void loop() {
    // Falls WLAN spaeter im Hintergrund verbunden wurde:
    if (WiFi.status() == WL_CONNECTED && !wifiServicesInitialized) {
        onWifiConnected();
    }

    audio.loop();
    if (wifiServicesInitialized) {
        server.handleClient();
        ArduinoOTA.handle();
    }
    handleSerialCommands();

    // Batterie alle 30 Sekunden messen (statt 5s)
    if (millis() - lastBatteryRead > 30000) {
        lastBatteryRead = millis();
        readBattery();
    }

    // SHTC3 Raumklima alle 30 Sekunden messen (statt 10s)
    if (shtc3Available && (millis() - lastShtc3Read > 30000)) {
        lastShtc3Read = millis();
        float t, h;
        if (shtc3_read(&t, &h)) {
            if (abs(t - roomTemp) >= 0.2f || abs(h - roomHumi) >= 1.0f) {
                roomTemp = t;
                roomHumi = h;
                displayNeedsUpdate = true;
            }
        }
    }

    // Zyklischer Status-Fetch alle 2 Minuten
    if (millis() - lastStatusFetch > STATUS_POLL_INTERVAL) {
        fetchHomelabStatus();
    }

    // Uhrzeit aktualisieren & Wecker prüfen
    updateTime();

    // Sanfter Weckstart (Lautstärke schrittweise alle 4s anheben)
    if (isAlarmActive && isRamping) {
        if (millis() - lastRampTime >= 4000) {
            lastRampTime = millis();
            if (currentVolume < alarmVolume) {
                setVolume(currentVolume + 1);
            } else {
                isRamping = false;
            }
        }
    }

    uint32_t now = millis();

    // 1. BOOT button:
    // Wenn Wecker aktiv: Schlummern (9 Min)
    // Sonst: Short -> Next Preset, Long -> Volume DOWN
    bool bootNow = digitalRead(BTN_BOOT);
    if (bootNow == LOW && lastBootState == HIGH) {
        bootPressTime = now;
        bootHandledLong = false;
        lastBootRepeat = now;
    } else if (bootNow == LOW && lastBootState == LOW) {
        if (now - bootPressTime >= 450) {
            if (now - lastBootRepeat >= 180) {
                lastBootRepeat = now;
                bootHandledLong = true;
                if (!isAlarmActive) {
                    setVolume(currentVolume - 1);
                }
            }
        }
    } else if (bootNow == HIGH && lastBootState == LOW) {
        if (!bootHandledLong && (now - bootPressTime > 40)) {
            if (isAlarmActive) {
                snoozeAlarm();
            } else {
                // Short press: Next station
                currentPresetIdx = (currentPresetIdx + 1) % NUM_PRESETS;
                playStation(currentPresets[currentPresetIdx].url, currentPresets[currentPresetIdx].name);
            }
        }
    }
    lastBootState = bootNow;

    // 2. KEY button:
    // Wenn Wecker aktiv: Wecker ausschalten (Dismiss)
    // Sonst: Short -> Play/Pause, Long -> Volume UP
    bool keyNow = digitalRead(BTN_KEY);
    if (keyNow == LOW && lastKeyState == HIGH) {
        keyPressTime = now;
        keyHandledLong = false;
        lastKeyRepeat = now;
    } else if (keyNow == LOW && lastKeyState == LOW) {
        if (now - keyPressTime >= 450) {
            if (now - lastKeyRepeat >= 180) {
                lastKeyRepeat = now;
                keyHandledLong = true;
                if (!isAlarmActive) {
                    setVolume(currentVolume + 1);
                }
            }
        }
    } else if (keyNow == HIGH && lastKeyState == LOW) {
        if (!keyHandledLong && (now - keyPressTime > 40)) {
            if (isAlarmActive) {
                dismissAlarm();
            } else {
                // Short press: Toggle Play / Stop
                if (isPlaying) {
                    stopStation();
                } else {
                    playStation(currentPresets[currentPresetIdx].url, currentPresets[currentPresetIdx].name);
                }
            }
        }
    }
    lastKeyState = keyNow;

    // Refresh Display if requested or fallback every 60s (RLCD ist statisch sparsam)
    if (displayNeedsUpdate || (millis() - lastDisplayUpdate > 60000)) {
        updateDisplay();
    }

    // Bei aktiver Wiedergabe 2ms fuer stabilen I2S-Stream,
    // im Dashboard-Modus (Standby) 25ms fuer FreeRTOS CPU-Idle-Sleep
    if (isPlaying) {
        delay(2);
    } else {
        delay(25);
    }
}
