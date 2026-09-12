#pragma once

// --- Hardware Pins for Waveshare ESP32-S3-RLCD-4.2 ---
// Display (ST7305 SPI)
#define RLCD_SCK_PIN    11
#define RLCD_MOSI_PIN   12
#define RLCD_DC_PIN      5
#define RLCD_CS_PIN     40
#define RLCD_RST_PIN    41

// Display dimensions
#define LCD_WIDTH       400
#define LCD_HEIGHT      300

// Audio Codec (ES8311 I2C & I2S)
#define I2S_DOUT         8 // DSDIN on ES8311
#define I2S_BCLK         9 // SCLK on ES8311
#define I2S_LRC         45 // LRCK / WS on ES8311
#define I2S_MCLK        16 // MCLK on ES8311
#define I2C_SDA         13
#define I2C_SCL         14
#define PA_ENABLE       46 // Audio Power Amplifier enable

// Buttons
#define BTN_BOOT         0
#define BTN_KEY         18

// --- Wi-Fi Default Config ---
#define DEFAULT_WIFI_SSID     "YOUR_WIFI_SSID"
#define DEFAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// --- Preset Radio Stations ---
struct RadioStation {
    const char *name;
    const char *url;
};

const RadioStation PRESET_STATIONS[] = {
    {"Rock Antenne", "http://stream.rockantenne.de/rockantenne/stream/mp3"},
    {"Radio BOB!",   "http://streams.radiobob.de/bob-national/mp3"},
    {"1LIVE",        "http://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
    {"Sunshine Live","http://stream.sunshine-live.de/live/mp3-192"},
    {"Antenne Thueringen", "http://stream.antennethueringen.de/live/aac-64/stream.antennethueringen.de/"}
};

const int NUM_PRESETS = sizeof(PRESET_STATIONS) / sizeof(PRESET_STATIONS[0]);
