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

// --- Global Objects ---
Audio audio;
ES8311 es;
static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *u8g2 = nullptr;
WebServer server(80);
Preferences prefs;

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

    Audio::audio_info_callback = onAudioInfo;
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
    audio.setVolume(currentVolume);
    audio.setAudioTaskCore(0); // Native Audio Task on Core 0!

    // 4. Load Saved WiFi credentials or use default
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

void onWifiConnected() {
    if (wifiServicesInitialized) return;
    wifiServicesInitialized = true;
    Serial.printf("[OK] WLAN verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
    setupOTA();
    setupWebServer();
    fetchHomelabStatus();
    // Starte direkt die erste Station
    playStation(PRESET_STATIONS[0].url, PRESET_STATIONS[0].name);
    displayNeedsUpdate = true;
    updateDisplay();
}

void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>RLCD WebRadio & Dashboard</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;background:#222;color:#eee;text-align:center;padding:20px;}";
        html += ".btn{display:inline-block;padding:10px 20px;margin:6px;background:#444;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:15px;text-decoration:none;}";
        html += ".btn:hover{background:#666;}.card{background:#333;padding:20px;border-radius:10px;display:inline-block;max-width:520px;width:100%;margin-top:15px;text-align:left;}</style></head><body>";
        html += "<h1>Waveshare RLCD Dashboard</h1>";
        html += "<div class='card'>";
        html += "<h2>Radio: " + currentStation + "</h2>";
        html += "<p><strong>Titel:</strong> " + currentTitle + "</p>";
        html += "<p><strong>Status:</strong> " + streamStatus + " | <strong>Lautst&auml;rke:</strong> " + String(currentVolume) + "/21</p>";
        html += "<p><a href='/stop' class='btn' style='background:#b22;'>STOP</a>";
        html += "<a href='/vol?val=" + String(max(0, currentVolume - 2)) + "' class='btn'>Vol -</a>";
        html += "<a href='/vol?val=" + String(min(21, currentVolume + 2)) + "' class='btn'>Vol +</a></p>";
        html += "<h3>Stationen</h3>";
        for (int i = 0; i < NUM_PRESETS; i++) {
            html += "<a href='/play?station=" + String(i) + "' class='btn'>" + String(PRESET_STATIONS[i].name) + "</a> ";
        }
        html += "<hr style='border-color:#555;'>";
        html += "<h3>Homelab & Wetter</h3>";
        html += "<p>Wetter: " + String(weather_temp_c, 1) + " &deg;C (" + String(getWmoText(weather_code)) + "), Regen: " + String(weather_rain_pct) + "%</p>";
        html += "<p>Homelab: " + String(homelab_w, 1) + " W | PVE CPU: " + String(pve_cpu_pct, 1) + "% | RAM: " + String(pve_ram_gb, 1) + "/" + String(pve_ram_total_gb, 1) + " GB</p>";
        if (shtc3Available) {
            html += "<p><strong>Raumklima (SHTC3):</strong> " + String(roomTemp, 1) + " &deg;C | " + String(roomHumi, 0) + "% rF</p>";
        }
        html += "<p><strong>Akku:</strong> " + String(batteryPercent) + "% (" + String(batteryVoltage, 2) + " V)</p>";
        html += "<p><small>Stand: " + serverGenerated + "</small></p>";
        html += "<hr style='border-color:#555;'>";
        html += "<p style='text-align:center;'>";
        html += "<a href='/launcher' class='btn' style='font-size:13px;background:#8b5cf6;'>&#128640; Zum MultiBoot Launcher wechseln</a> &nbsp;";
        html += "<a href='/update' class='btn' style='font-size:13px;background:#555;'>&#9881; OTA Firmware Update</a>";
        html += "</p>";
        html += "</div></body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, []() {
        JsonDocument doc;
        doc["status"] = streamStatus;
        doc["station"] = currentStation;
        doc["title"] = currentTitle;
        doc["volume"] = currentVolume;
        doc["ip"] = WiFi.localIP().toString();

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

        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/play", HTTP_GET, []() {
        if (server.hasArg("station")) {
            int idx = server.arg("station").toInt();
            if (idx >= 0 && idx < NUM_PRESETS) {
                currentPresetIdx = idx;
                playStation(PRESET_STATIONS[idx].url, PRESET_STATIONS[idx].name);
            }
        } else if (server.hasArg("url")) {
            String url = server.arg("url");
            String name = server.hasArg("name") ? server.arg("name") : "Custom Stream";
            playStation(url, name);
        }
        server.send(200, "application/json", "{\"result\":\"ok\"}");
    });

    server.on("/stop", HTTP_GET, []() {
        stopStation();
        server.send(200, "application/json", "{\"result\":\"stopped\"}");
    });

    server.on("/vol", HTTP_GET, []() {
        if (server.hasArg("val")) {
            setVolume(server.arg("val").toInt());
        }
        server.send(200, "application/json", "{\"volume\":" + String(currentVolume) + "}");
    });

    server.on("/fetch", HTTP_GET, []() {
        fetchHomelabStatus();
        server.send(200, "application/json", "{\"result\":\"fetched\"}");
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

void updateDisplay() {
    u8g2->clearBuffer();
    u8g2->setDrawColor(1);

    // Outer Border
    u8g2->drawFrame(2, 2, 396, 296);

    // --- Header Bar ---
    u8g2->setFont(u8g2_font_helvB10_tr);
    u8g2->drawStr(8, 20, "DASHBOARD");

    u8g2->setFont(u8g2_font_6x10_tr);
    // Zeitstempel ohne Sekunden formatieren (z. B. "10.09. 20:58")
    String timeStr = serverGenerated;
    if (timeStr.length() >= 14 && timeStr.charAt(timeStr.length() - 3) == ':') {
        timeStr = timeStr.substring(0, timeStr.length() - 3);
    }
    if (timeStr.length() > 0 && timeStr != "--:--") {
        u8g2->drawStr(122, 20, timeStr.c_str());
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
    // Station Name
    u8g2->setFont(u8g2_font_helvB14_tr);
    u8g2->drawStr(12, 45, currentStation.c_str());

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
    u8g2->drawStr(10, 288, "[BOOT] Sender / Vol-    [KEY] Play/Stop / Vol+");

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
                playStation(PRESET_STATIONS[idx].url, PRESET_STATIONS[idx].name);
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

    uint32_t now = millis();

    // 1. BOOT button: Short -> Next Preset, Long -> Volume DOWN
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
                setVolume(currentVolume - 1);
            }
        }
    } else if (bootNow == HIGH && lastBootState == LOW) {
        if (!bootHandledLong && (now - bootPressTime > 40)) {
            // Short press: Next station
            currentPresetIdx = (currentPresetIdx + 1) % NUM_PRESETS;
            playStation(PRESET_STATIONS[currentPresetIdx].url, PRESET_STATIONS[currentPresetIdx].name);
        }
    }
    lastBootState = bootNow;

    // 2. KEY button: Short -> Play/Pause, Long -> Volume UP
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
                setVolume(currentVolume + 1);
            }
        }
    } else if (keyNow == HIGH && lastKeyState == LOW) {
        if (!keyHandledLong && (now - keyPressTime > 40)) {
            // Short press: Toggle Play / Stop
            if (isPlaying) {
                stopStation();
            } else {
                playStation(PRESET_STATIONS[currentPresetIdx].url, PRESET_STATIONS[currentPresetIdx].name);
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
