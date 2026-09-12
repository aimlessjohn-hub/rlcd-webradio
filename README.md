# rlcd-webradio

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Arduino: 2.x](https://img.shields.io/badge/Arduino%20IDE-2.x-teal.svg)](https://www.arduino.cc/)
[![ESP32 Core: 3.3.x](https://img.shields.io/badge/esp32--core-3.3.x-orange.svg)](https://github.com/espressif/arduino-esp32)
[![Display: ST7305 RLCD](https://img.shields.io/badge/Display-4.2%22%20ST7305%20RLCD-green.svg)](#hardware)

Ein energieeffizientes, tageslichttaugliches **WebRadio** und **Homelab/Wetter-Dashboard** für das **Waveshare ESP32-S3 4.2" RLCD Development Board**.

Das Projekt vereint modernes Webradio-Streaming mit On-Chip-Dekodierung und ein durchdachtes Info-Display. Dank der reflektierenden RLCD-Technologie bleibt die Anzeige selbst bei direkter Sonneneinstrahlung glasklar ablesbar, ohne Strom für eine Hintergrundbeleuchtung zu verbrauchen.

---

## Inhaltsverzeichnis

- [Features](#features)
- [Hardware](#hardware)
  - [Übersicht](#übersicht)
  - [Pinbelegung (Referenz)](#pinbelegung-referenz)
- [Installation](#installation)
  - [Entwicklungsumgebung](#entwicklungsumgebung)
  - [Bibliotheken](#bibliotheken)
  - [Board-Konfiguration](#board-konfiguration)
- [Konfiguration](#konfiguration)
- [REST-API](#rest-api)
- [MultiBoot-Unterstützung](#multiboot-unterstützung)
- [Web-Dashboard & OTA](#web-dashboard--ota)
- [Lizenz](#lizenz)

---

## Features

- **Audio & WebRadio:**
  - On-Chip-Audiodekodierung (MP3 & AAC) via `ESP32-audioI2S` (v4.x) in Kombination mit dem **ES8311** I2S-Codec.
  - 7 frei konfigurierbare Sender-Presets direkt in der Firmware verankert (`config.h`).
  - Dynamisches Auslesen und Darstellen von **ICY-Metadaten** (Sendername, Interpret, Titel).
  - Feingranulare Lautstärkeregelung (Stufen 0 bis 21).
  - Abspielen benutzerdefinierter Streams zur Laufzeit per REST-Aufruf.

- **4.2" RLCD-Display (ST7305, 400x300):**
  - Reflektierendes Display: Höchster Kontrast bei Raum- und Sonnenlicht.
  - Ultra-niedriger Stromverbrauch (Memory-in-Pixel/RLCD-Technologie, keine Hintergrundbeleuchtung erforderlich).
  - Schnelle Bildschirmaktualisierungen und segmentierte Renderzonen mit optimierter U8g2-Grafikausgabe.

- **Sensoren & Energieverwaltung:**
  - Lokale Raumklima-Erfassung via Onboard **Sensirion SHTC3** (Temperatur & relative Luftfeuchtigkeit über I2C).
  - LiPo-/Akku-Spannungsüberwachung über internen ADC an **GPIO 4**.
  - Dynamisches Batterie-Icon mit prozentualer Füllstandsanzeige und Ladezustandserkennung.

- **Wetter- & Homelab-Dashboard:**
  - **Open-Meteo Wetter:** Abruf von Temperatur, Vorhersagen und Wettersymbolen über ein schlankes, selbstgehostetes Backend-Script (JSON-Aggregator).
  - **Homelab-Telemetrie:** Überwachung von Proxmox-Servern (CPU-Auslastung, RAM-Verbrauch, Anzahl aktiver VMs) sowie Echtzeit-Leistungsaufnahme des Homelab-Netzteils (Watt).

- **Netzwerk & Management:**
  - **Web-Dashboard (Port 80):** Mobile-optimierte Oberfläche zur Wiedergabesteuerung, Senderwahl und Datenansicht im lokalen Netzwerk.
  - **Vollwertige REST-API:** Nahtlose Integration in Home Assistant, Node-RED oder Skripte.
  - **Duales OTA-Update:** Drahtlose Firmware-Aktualisierung sowohl über das Web-Frontend (`/update`) als auch über standardmäßiges `ArduinoOTA`.

- **MultiBoot-kompatibel:**
  - Entwickelt für Setups mit Boot-Launcher. Schneller Rücksprung in die Launcher-Partition (`app0`) via `esp_ota_set_boot_partition()`.

---

## Hardware

### Übersicht

Das Projekt ist maßgeschneidert für das **Waveshare ESP32-S3 4.2" RLCD Dev Board**:

| Komponente | Spezifikation | Anbindung |
| :--- | :--- | :--- |
| **SoC** | ESP32-S3-WROOM-1 (N16R8) | 16 MB Flash, 8 MB Octal PSRAM |
| **Display** | 4.2" Reflective LCD (400 × 300) | ST7305 Controller via SPI |
| **Audio-Codec** | Everest Semiconductor ES8311 | I2C (Control) + I2S (Digital Audio) |
| **Audio-Output** | Onboard-Verstärker & Speaker-Header | Mono / Stereo-Downmix |
| **Umweltsensor** | Sensirion SHTC3 | I2C (Temperatur & Luftfeuchtigkeit) |
| **Batterie-ADC** | Spannungsteiler an LiPo-Anschluss | ADC1 Channel / GPIO 4 |

### Pinbelegung (Referenz)

| Funktionsgruppe | Signal | ESP32-S3 GPIO |
| :--- | :--- | :--- |
| **I2C Bus** (Codec, SHTC3) | SDA / SCL | GPIO 13 / GPIO 14 |
| **I2S Audio** (ES8311) | DSDIN / SCLK / LRCK / MCLK | GPIO 8 / GPIO 9 / GPIO 45 / GPIO 16 |
| **RLCD Display** (ST7305) | SCK / MOSI / DC / CS / RST | GPIO 11 / GPIO 12 / GPIO 5 / GPIO 40 / GPIO 41 |
| **Sensor / ADC** | Battery Sense | GPIO 4 |

---

## Installation

### Entwicklungsumgebung

1. Installiere die [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. Öffne die Voreinstellungen (`Datei` → `Voreinstellungen` bzw. `Preferences`) und füge die offizielle ESP32-Boardverwalter-URL hinzu:
   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. Öffne den **Boardverwalter** (`Werkzeuge` → `Board` → `Boardverwalter...`), suche nach `esp32` von **Espressif Systems** und installiere die Version **3.3.x**.

### Bibliotheken

Installiere folgende Bibliotheken über den Arduino Bibliotheksverwalter oder via GitHub:

- **ESP32-audioI2S** (Version **4.x** von Wolle / *schreibfaul1*)
- **U8g2** (von *oliver*) für die Ansteuerung und Schriftart-Darstellung des ST7305
- **ArduinoJson** (Version **6.x** oder **7.x** von *Benoît Blanchon*)
- **ArduinoOTA** (im ESP32-Core enthalten)

### Board-Konfiguration

Wähle unter `Werkzeuge` in der Arduino IDE folgende Einstellungen:

- **Board:** `ESP32S3 Dev Module`
- **USB CDC On Boot:** `Enabled`
- **Flash Size:** `16MB (128Mb)`
- **Partition Scheme:** `16MB Flash (3MB APP/OTA/16MB SPIFFS)` oder deine MultiBoot-spezifische Partitionstabelle
- **PSRAM:** `OPI PSRAM` (wichtig für den 8 MB Octal PSRAM)
- **CPU Frequency:** `240MHz (WiFi)`
- **Upload Speed:** `921600`

---

## Konfiguration

Kopiere vor dem Kompilieren die Konfigurationsvorlage (falls vorhanden) oder passe die `config.h` direkt im Projektverzeichnis an:

```cpp
// =================================================================
// WLAN-Zugangsdaten
// =================================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =================================================================
// Backend & API Endpunkte (Wetter & Homelab)
// =================================================================
// URL des selbstgehosteten Backend-Skripts (aggregiert Open-Meteo & Proxmox)
#define HOMELAB_BACKEND_URL "http://backend.homelab.internal/api/dashboard.json"

// =================================================================
// 7 Sender-Presets (Name und Stream-URL)
// =================================================================
struct RadioStation {
  const char* name;
  const char* url;
};

const RadioStation PRESETS[7] = {
  { "Radio 1",       "http://stream.example.com/radio1.mp3" },
  { "Ambient Chill", "http://stream.example.com/ambient.aac" },
  { "Jazz Lounge",   "http://stream.example.com/jazz.mp3" },
  { "Rock Classics", "http://stream.example.com/rock.mp3" },
  { "Electronic FM", "http://stream.example.com/electro.aac" },
  { "News & Info",   "http://stream.example.com/news.mp3" },
  { "Classical",     "http://stream.example.com/classical.mp3" }
};
```

> [!IMPORTANT]
> Achte darauf, `YOUR_WIFI_SSID` und `YOUR_WIFI_PASSWORD` mit deinen tatsächlichen WLAN-Parametern zu belegen. Verbinde den ESP32-S3 vorzugsweise mit einem stabilen 2,4-GHz-Netzwerk.

---

## REST-API

Das Gerät stellt auf Port 80 eine strukturierte HTTP-REST-Schnittstelle zur Verfügung. Alle Endpunkte können einfach via Browser, `curl` oder Home Assistant angesprochen werden.

| Methode | Endpunkt | Parameter | Beschreibung | Rückgabe (Beispiel) |
| :--- | :--- | :--- | :--- | :--- |
| `GET` | `/status` | *keine* | Gibt den aktuellen Gesamtstatus (Playback, Metadaten, Sensoren, Homelab, Akku) zurück | `{"status":"playing","station":"Radio 1","title":"Artist - Song","vol":14,"temp":21.8,"hum":48,"vbat":4.12,"batt_pct":92,"proxmox":{"cpu":18,"ram":42,"vms":9},"power_w":84.5}` |
| `GET` | `/play` | `station=1..7` | Startet die Wiedergabe eines vordefinierten Presets (Index 1–7) | `{"success":true,"message":"Playing preset 1"}` |
| `GET` | `/play` | `url=STREAM&name=NAME` | Startet die Wiedergabe eines beliebigen Streams mit temporärem Namen | `{"success":true,"message":"Playing custom stream"}` |
| `GET` | `/stop` | *keine* | Stoppt den laufenden Audio-Stream | `{"success":true,"message":"Playback stopped"}` |
| `GET` | `/vol` | `val=0..21` | Setzt die Lautstärke im Bereich von 0 (stumm) bis 21 (Maximum) | `{"success":true,"volume":15}` |
| `GET` | `/fetch` | *keine* | Erzwingt die sofortige Hintergrundabfrage von Open-Meteo & Homelab-Daten | `{"success":true,"message":"Sync triggered"}` |

### Beispiele

```bash
# Aktuellen Status abfragen
curl http://192.168.x.x/status

# Sender 3 starten
curl "http://192.168.x.x/play?station=3"

# Lautstärke auf 12 setzen
curl "http://192.168.x.x/vol?val=12"

# Eigenen Stream abspielen
curl "http://192.168.x.x/play?url=http://stream.example.com/live.mp3&name=CustomRadio"

# Telemetrie-Sync manuell anstoßen
curl http://192.168.x.x/fetch
```

---

## MultiBoot-Unterstützung

Die Firmware ist vollständig **MultiBoot-kompatibel**. In einem Multi-App-Partitionslayout (z. B. Factory-Launcher auf `app0` und Anwendungen auf `app1`, `app2` etc.) kann das WebRadio die Boot-Partition dynamisch zurücksetzen:

```cpp
#include "esp_ota_ops.h"

void returnToLauncher() {
  const esp_partition_t* launcher_partition = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,
    ESP_PARTITION_SUBTYPE_APP_OTA_0,
    NULL
  );

  if (launcher_partition != nullptr) {
    esp_ota_set_boot_partition(launcher_partition);
    esp_restart();
  }
}
```

- Ein Wechsel zurück ins Hauptmenü bzw. in den Launcher kann entweder per Hardware-Taster oder über einen API-Aufruf getriggert werden.
- Die Ausführung von `esp_ota_set_boot_partition(app0)` stellt sicher, dass beim anschließenden `esp_restart()` sauber in das Auswahlmenü gebootet wird, ohne den Flash-Inhalt neu schreiben zu müssen.

---

## Web-Dashboard & OTA

- **Web-Dashboard:** Rufe im Browser `http://<GERAETE_IP>/` auf, um die Weboberfläche zu öffnen. Dort stehen Buttons für Sender 1–7, Play/Pause, ein Lautstärkeregler sowie Echtzeitanzeigen für das Raumklima und das Homelab bereit.
- **Web-OTA:** Unter `http://<GERAETE_IP>/update` steht ein Formular bereit, über das kompilierte Binärdateien (`.bin`) direkt aus dem Browser geflasht werden können.
- **ArduinoOTA:** Das Gerät kündigt sich im lokalen Netzwerk via mDNS an. In der Arduino IDE taucht das Board unter `Werkzeuge` → `Port` als Netzwerk-Port auf und kann kabellos geflasht werden.

---

## Lizenz

Dieses Projekt ist unter der **GNU General Public License v3.0 (GPL-3.0)** lizenziert. Weitere Details findest du in der Datei [LICENSE](LICENSE).
