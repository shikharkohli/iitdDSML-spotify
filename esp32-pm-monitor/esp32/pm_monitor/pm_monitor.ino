/*
 * ESP32 + PMS5003 / PMS7003 Air Quality Monitor
 *
 * Reads all PM / particle-count values from a Plantower PMS5003 or PMS7003
 * sensor.  Supports two modes set via config.h:
 *
 *   STANDALONE_MODE 1 — prints readings to USB serial only (no WiFi/cloud).
 *                       Use `pio device monitor` to view values on your laptop.
 *
 *   STANDALONE_MODE 0 — POSTs readings to the configured backend at the
 *                       interval set from the web dashboard.
 *
 * Both sensors use the same 32-byte UART frame at 9600 baud.
 * Only difference: PMS5003 bytes 26-27 (>10 µm count) are reserved/0.
 * Set SENSOR_MODEL in config.h to 5003 or 7003 accordingly.
 *
 * ── Wiring: PMS5003 (10-pin 1.27 mm JST) → ESP32 ──────────────────────────
 *   Pin 1  VCC   → 5 V
 *   Pin 2  VCC   → 5 V
 *   Pin 3  GND   → GND
 *   Pin 4  GND   → GND
 *   Pin 5  RESET → 3.3 V  (active-low; tie HIGH = no reset)
 *   Pin 6  NC    → —
 *   Pin 7  RXD   → GPIO 17  (PMS_TX_PIN — ESP32 transmits to sensor)
 *   Pin 8  NC    → —
 *   Pin 9  TXD   → GPIO 16  (PMS_RX_PIN — ESP32 receives from sensor)
 *   Pin 10 SET   → 3.3 V  (active-high; tie HIGH = continuous active mode)
 *
 * ── Wiring: PMS7003 (8-pin 1.25 mm JST) → ESP32 ───────────────────────────
 *   VCC → 5 V       GND → GND
 *   TX  → GPIO 16 (PMS_RX_PIN)    RX → GPIO 17 (PMS_TX_PIN)
 *   SET → 3.3 V (active mode)     RST → 3.3 V (no reset)
 *
 * ── Before compiling ────────────────────────────────────────────────────────
 *   cp config.h.example config.h   then edit config.h with your settings.
 *
 * ── Libraries (PlatformIO installs automatically via platformio.ini) ────────
 *   ArduinoJson >= 7.0  (Benoit Blanchon)
 *   WiFi / HTTPClient   bundled with ESP32 Arduino core
 */

#include <Arduino.h>
#include "config.h"

// STANDALONE_MODE defaults to 0 if not defined in config.h
#ifndef STANDALONE_MODE
  #define STANDALONE_MODE 0
#endif

// WiFi / HTTP / JSON are only needed in cloud mode
#if !STANDALONE_MODE
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <ArduinoJson.h>   // v7 API: JsonDocument (no size template)
#endif

// ─── Protocol ────────────────────────────────────────────────────────────────
static constexpr uint8_t  PMS_START1    = 0x42;
static constexpr uint8_t  PMS_START2    = 0x4D;
static constexpr uint8_t  PMS_FRAME_LEN = 32;

// ─── Sensor capability ───────────────────────────────────────────────────────
// PMS5003 bytes 26-27 are reserved (>10 µm not populated).
// PMS7003 bytes 26-27 carry a valid >10 µm count.
#if SENSOR_MODEL == 7003
  static constexpr bool HAS_CNT_10UM = true;
#else
  static constexpr bool HAS_CNT_10UM = false;
#endif

// PMS5003/7003 fan needs ~30 s after power-on to reach stable airflow.
static constexpr uint32_t SENSOR_WARMUP_MS        = 30000UL;
static constexpr uint32_t CONFIG_POLL_INTERVAL_MS = 60000UL;

// ─── Runtime state ───────────────────────────────────────────────────────────
static uint32_t g_intervalSec  = DEFAULT_INTERVAL_SEC;
static uint32_t g_lastSampleMs = 0;
#if !STANDALONE_MODE
static uint32_t g_lastConfigMs = 0;
#endif

// ─── Parsed sensor data ───────────────────────────────────────────────────────
struct PmsData {
    uint16_t pm1_0_std, pm2_5_std, pm10_std;  // µg/m³ — CF=1 (standard)
    uint16_t pm1_0_atm, pm2_5_atm, pm10_atm;  // µg/m³ — atmospheric
    uint16_t cnt_0_3um, cnt_0_5um, cnt_1_0um; // particle counts / 0.1 L
    uint16_t cnt_2_5um, cnt_5_0um, cnt_10um;
};

// ─── Frame reader ─────────────────────────────────────────────────────────────
// Blocks until a valid checksummed 32-byte frame arrives, or timeoutMs expires.
bool readPmsFrame(PmsData &out, uint32_t timeoutMs = 6000) {
    uint8_t  buf[PMS_FRAME_LEN];
    uint32_t deadline = millis() + timeoutMs;

    while ((int32_t)(deadline - millis()) > 0) {

        // Sync: wait for start byte 1 (0x42)
        if (!Serial2.available()) { delay(2); continue; }
        if (Serial2.read() != PMS_START1) continue;

        // Confirm start byte 2 (0x4D) — allow 100 ms
        uint32_t t2 = millis() + 100;
        while (!Serial2.available() && (int32_t)(t2 - millis()) > 0) delay(1);
        if (!Serial2.available() || Serial2.read() != PMS_START2) continue;

        buf[0] = PMS_START1;
        buf[1] = PMS_START2;

        // Read remaining 30 bytes — allow 250 ms (well above 30 bytes × 1 ms/byte at 9600)
        uint8_t  got   = 2;
        uint32_t tRest = millis() + 250;
        while (got < PMS_FRAME_LEN && (int32_t)(tRest - millis()) > 0) {
            if (Serial2.available()) buf[got++] = (uint8_t)Serial2.read();
        }
        if (got < PMS_FRAME_LEN) {
            Serial.println("[PMS] Incomplete frame — retrying");
            continue;
        }

        // Checksum: sum of bytes 0..29 must equal bytes 30-31
        uint16_t sum  = 0;
        for (uint8_t i = 0; i < 30; i++) sum += buf[i];
        uint16_t rxCk = ((uint16_t)buf[30] << 8) | buf[31];
        if (sum != rxCk) {
            Serial.printf("[PMS] Checksum FAIL  calc=0x%04X  rx=0x%04X\n", sum, rxCk);
            continue;
        }

        // Parse: big-endian 16-bit words, data starts at byte 4
        auto w = [&](uint8_t i) -> uint16_t {
            return ((uint16_t)buf[4 + i * 2] << 8) | buf[5 + i * 2];
        };

        out.pm1_0_std = w(0);  out.pm2_5_std = w(1);  out.pm10_std  = w(2);
        out.pm1_0_atm = w(3);  out.pm2_5_atm = w(4);  out.pm10_atm  = w(5);
        out.cnt_0_3um = w(6);  out.cnt_0_5um = w(7);  out.cnt_1_0um = w(8);
        out.cnt_2_5um = w(9);  out.cnt_5_0um = w(10);
        out.cnt_10um  = HAS_CNT_10UM ? w(11) : 0;

        return true;
    }

    Serial.println("[PMS] Frame timeout");
    return false;
}

// ─── WiFi / HTTP — compiled out when STANDALONE_MODE = 1 ─────────────────────
#if !STANDALONE_MODE

void wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("[WiFi] Connecting to \"%s\"", WIFI_SSID);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (uint8_t i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print('.');
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected  IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
        WiFi.disconnect(true);
        Serial.println("\n[WiFi] Failed — will retry next cycle");
    }
}

bool postData(const PmsData &d) {
    wifiConnect();
    if (WiFi.status() != WL_CONNECTED) return false;

    // ArduinoJson v7: JsonDocument (replaces StaticJsonDocument / DynamicJsonDocument)
    JsonDocument doc;
    doc["pm1_0_std"] = d.pm1_0_std;
    doc["pm2_5_std"] = d.pm2_5_std;
    doc["pm10_std"]  = d.pm10_std;
    doc["pm1_0_atm"] = d.pm1_0_atm;
    doc["pm2_5_atm"] = d.pm2_5_atm;
    doc["pm10_atm"]  = d.pm10_atm;
    doc["cnt_0_3um"] = d.cnt_0_3um;
    doc["cnt_0_5um"] = d.cnt_0_5um;
    doc["cnt_1_0um"] = d.cnt_1_0um;
    doc["cnt_2_5um"] = d.cnt_2_5um;
    doc["cnt_5_0um"] = d.cnt_5_0um;
    // PMS5003: omit cnt_10um — backend stores NULL, dashboard shows N/A
    if (HAS_CNT_10UM) doc["cnt_10um"] = d.cnt_10um;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/data");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);
    int code = http.POST(body);
    http.end();

    Serial.printf("[HTTP] POST /api/data → %d\n", code);
    return (code == 200 || code == 201);
}

void fetchConfig() {
    wifiConnect();
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/config");
    http.addHeader("X-API-Key", API_KEY);
    int code = http.GET();

    if (code == 200) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        if (!err) {
            uint32_t newSec = doc["interval_sec"] | g_intervalSec;
            if (newSec >= 10 && newSec != g_intervalSec) {
                Serial.printf("[Config] Interval %u → %u s\n", g_intervalSec, newSec);
                g_intervalSec = newSec;
            }
        }
    }

    http.end();
    Serial.printf("[HTTP] GET /api/config → %d  interval=%u s\n", code, g_intervalSec);
}

#endif  // !STANDALONE_MODE

// ─── setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);  // let USB-serial settle before printing
    Serial.printf("\n\n[Boot] ESP32 PMS%d Monitor\n", SENSOR_MODEL);

    // Both PMS5003 and PMS7003 use 9600 baud, 8N1
    Serial2.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
    Serial.printf("[PMS]  UART on GPIO RX=%d  TX=%d\n", PMS_RX_PIN, PMS_TX_PIN);

    // Fan warmup — datasheet requires ~30 s before readings are stable
    Serial.printf("[PMS]  Warming up (%lu s)", SENSOR_WARMUP_MS / 1000);
    for (uint32_t i = 0; i < SENSOR_WARMUP_MS; i += 2000) {
        delay(2000);
        Serial.print('.');
    }
    Serial.println(" ready");

#if STANDALONE_MODE
    Serial.println("[Mode] STANDALONE — Serial output only, no WiFi");
    Serial.printf("[Mode] Reading every %lu s  (change DEFAULT_INTERVAL_SEC in config.h)\n",
                  (unsigned long)g_intervalSec);
#else
    WiFi.mode(WIFI_STA);
    wifiConnect();
    fetchConfig();
    g_lastConfigMs = millis();  // prevent immediate re-poll on first loop()
#endif

    g_lastSampleMs = millis();
}

// ─── loop ────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

#if !STANDALONE_MODE
    // Poll server for updated interval every CONFIG_POLL_INTERVAL_MS
    if (now - g_lastConfigMs >= CONFIG_POLL_INTERVAL_MS) {
        g_lastConfigMs = now;
        fetchConfig();
    }
#endif

    if (now - g_lastSampleMs >= g_intervalSec * 1000UL) {
        Serial.printf("\n[Sample] Reading PMS%d...\n", SENSOR_MODEL);

        // Discard frames that built up while idle, then wait for a fresh one
        while (Serial2.available()) Serial2.read();
        delay(10);

        PmsData data;
        if (readPmsFrame(data)) {
            // Always print to serial — visible in both standalone and cloud modes
            Serial.printf("[PMS] std  PM1.0=%-4u  PM2.5=%-4u  PM10=%-4u  µg/m³\n",
                          data.pm1_0_std, data.pm2_5_std, data.pm10_std);
            Serial.printf("[PMS] atm  PM1.0=%-4u  PM2.5=%-4u  PM10=%-4u  µg/m³\n",
                          data.pm1_0_atm, data.pm2_5_atm, data.pm10_atm);
            if (HAS_CNT_10UM) {
                Serial.printf("[PMS] cnt  >0.3=%-6u  >0.5=%-6u  >1.0=%-6u  >2.5=%-6u  >5.0=%-6u  >10=%-6u  /0.1L\n",
                              data.cnt_0_3um, data.cnt_0_5um, data.cnt_1_0um,
                              data.cnt_2_5um, data.cnt_5_0um, data.cnt_10um);
            } else {
                Serial.printf("[PMS] cnt  >0.3=%-6u  >0.5=%-6u  >1.0=%-6u  >2.5=%-6u  >5.0=%-6u  /0.1L\n",
                              data.cnt_0_3um, data.cnt_0_5um, data.cnt_1_0um,
                              data.cnt_2_5um, data.cnt_5_0um);
            }

#if STANDALONE_MODE
            g_lastSampleMs = millis();
#else
            if (postData(data)) {
                g_lastSampleMs = millis();
            } else {
                Serial.println("[HTTP] POST failed — retrying in 30 s");
                g_lastSampleMs = millis() - (g_intervalSec * 1000UL) + 30000UL;
            }
#endif
        } else {
            Serial.println("[PMS] Read failed — retrying in 30 s");
            g_lastSampleMs = millis() - (g_intervalSec * 1000UL) + 30000UL;
        }
    }

    delay(100);
}
