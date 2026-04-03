/*
 * ESP32 + PMS5003 / PMS7003 Air Quality Monitor
 *
 * Reads all PM / particle-count values from a Plantower PMS5003 or PMS7003
 * sensor and POSTs them to a configurable backend server at a user-defined
 * interval.  The sampling interval can be changed live from the web dashboard.
 *
 * Both sensors share the same 32-byte UART frame format.
 * The only difference: PMS5003 bytes 26-27 (>10 µm count) are reserved/0;
 * set SENSOR_MODEL in config.h accordingly.
 *
 * Wiring — PMS5003 (10-pin 1.27 mm JST) → ESP32
 *   Pin 1 (VCC)   → 5 V
 *   Pin 2 (VCC)   → 5 V
 *   Pin 3 (GND)   → GND
 *   Pin 4 (GND)   → GND
 *   Pin 5 (RESET) → 3.3 V  (active-low; tie HIGH = no reset)
 *   Pin 6 (NC)    → —
 *   Pin 7 (RXD)   → GPIO 17  (PMS_TX_PIN)
 *   Pin 8 (NC)    → —
 *   Pin 9 (TXD)   → GPIO 16  (PMS_RX_PIN)
 *   Pin 10 (SET)  → 3.3 V  (active-high; tie HIGH = continuous / active mode)
 *
 * Wiring — PMS7003 (8-pin 1.25 mm JST) → ESP32
 *   VCC  → 5 V
 *   GND  → GND
 *   TX   → GPIO 16  (PMS_RX_PIN)
 *   RX   → GPIO 17  (PMS_TX_PIN)
 *   SET  → 3.3 V    (keep awake / active mode)
 *   RST  → 3.3 V    (no reset)
 *
 * Copy config.h.example → config.h and fill in your credentials before
 * compiling.
 *
 * Required libraries (install via Arduino Library Manager or PlatformIO):
 *   - ArduinoJson  (Benoit Blanchon)
 *   - WiFi         (bundled with esp32 core)
 *   - HTTPClient   (bundled with esp32 core)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ─── PMS5003 / PMS7003 protocol constants ────────────────────────────────────
static constexpr uint8_t  PMS_START1    = 0x42;
static constexpr uint8_t  PMS_START2    = 0x4D;
static constexpr uint16_t PMS_FRAME_LEN = 32;   // total bytes per frame

// ─── Runtime state ────────────────────────────────────────────────────────────
static uint32_t g_intervalSec  = DEFAULT_INTERVAL_SEC;
static uint32_t g_lastSampleMs = 0;   // millis() timestamp of last successful send
static uint32_t g_lastConfigMs = 0;   // millis() timestamp of last config poll

static constexpr uint32_t CONFIG_POLL_INTERVAL_MS = 60UL * 1000UL; // poll config every 60 s

// ─── Sensor capability flags ──────────────────────────────────────────────────
// PMS5003: bytes 26-27 in the frame are reserved; >10 µm count is not available.
// PMS7003: bytes 26-27 carry a valid >10 µm particle count.
#if SENSOR_MODEL == 7003
  static constexpr bool HAS_CNT_10UM = true;
#else
  static constexpr bool HAS_CNT_10UM = false;
#endif

// ─── Parsed sensor data ───────────────────────────────────────────────────────
struct PmsData {
    // Concentration in µg/m³ — "CF=1" (standard particle)
    uint16_t pm1_0_std;
    uint16_t pm2_5_std;
    uint16_t pm10_std;
    // Concentration in µg/m³ — atmospheric environment
    uint16_t pm1_0_atm;
    uint16_t pm2_5_atm;
    uint16_t pm10_atm;
    // Particle count per 0.1 L of air
    uint16_t cnt_0_3um;
    uint16_t cnt_0_5um;
    uint16_t cnt_1_0um;
    uint16_t cnt_2_5um;
    uint16_t cnt_5_0um;
    uint16_t cnt_10um;
};

// ─── Helper: read a 32-byte PMS5003/PMS7003 frame ────────────────────────────
// Returns true if a valid, checksummed frame was received within timeoutMs.
bool readPmsFrame(PmsData &out, uint32_t timeoutMs = 5000) {
    uint8_t buf[PMS_FRAME_LEN];
    uint32_t deadline = millis() + timeoutMs;

    while (millis() < deadline) {
        // Wait for first start byte
        if (!Serial2.available()) { delay(1); continue; }
        if (Serial2.read() != PMS_START1) continue;

        // Wait for second start byte
        uint32_t t = millis() + 100;
        while (millis() < t && !Serial2.available()) {}
        if (!Serial2.available() || Serial2.read() != PMS_START2) continue;

        buf[0] = PMS_START1;
        buf[1] = PMS_START2;

        // Read the remaining 30 bytes
        uint8_t bytesRead = 2;
        uint32_t inner = millis() + 200;
        while (bytesRead < PMS_FRAME_LEN && millis() < inner) {
            if (Serial2.available()) {
                buf[bytesRead++] = (uint8_t)Serial2.read();
            }
        }
        if (bytesRead < PMS_FRAME_LEN) continue;

        // Verify checksum (sum of bytes 0..29)
        uint16_t checksum = 0;
        for (uint8_t i = 0; i < 30; i++) checksum += buf[i];
        uint16_t rxCheck = ((uint16_t)buf[30] << 8) | buf[31];
        if (checksum != rxCheck) {
            Serial.printf("[PMS] Checksum mismatch: calc=%04X rx=%04X\n", checksum, rxCheck);
            continue;
        }

        // Parse data (big-endian 16-bit words starting at byte 4)
        auto word = [&](uint8_t idx) -> uint16_t {
            return ((uint16_t)buf[4 + idx * 2] << 8) | buf[5 + idx * 2];
        };

        out.pm1_0_std  = word(0);
        out.pm2_5_std  = word(1);
        out.pm10_std   = word(2);
        out.pm1_0_atm  = word(3);
        out.pm2_5_atm  = word(4);
        out.pm10_atm   = word(5);
        out.cnt_0_3um  = word(6);
        out.cnt_0_5um  = word(7);
        out.cnt_1_0um  = word(8);
        out.cnt_2_5um  = word(9);
        out.cnt_5_0um  = word(10);
        // Bytes 26-27 are reserved on PMS5003 (always 0); valid on PMS7003.
        out.cnt_10um   = HAS_CNT_10UM ? word(11) : 0;
        return true;
    }
    return false;  // timeout
}

// ─── WiFi helpers ─────────────────────────────────────────────────────────────
void wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Serial.print('.');
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] Failed — will retry next cycle");
    }
}

// ─── Post sensor data ─────────────────────────────────────────────────────────
bool postData(const PmsData &d) {
    wifiConnect();
    if (WiFi.status() != WL_CONNECTED) return false;

    StaticJsonDocument<512> doc;
    // Standard
    doc["pm1_0_std"]  = d.pm1_0_std;
    doc["pm2_5_std"]  = d.pm2_5_std;
    doc["pm10_std"]   = d.pm10_std;
    // Atmospheric
    doc["pm1_0_atm"]  = d.pm1_0_atm;
    doc["pm2_5_atm"]  = d.pm2_5_atm;
    doc["pm10_atm"]   = d.pm10_atm;
    // Particle counts
    doc["cnt_0_3um"]  = d.cnt_0_3um;
    doc["cnt_0_5um"]  = d.cnt_0_5um;
    doc["cnt_1_0um"]  = d.cnt_1_0um;
    doc["cnt_2_5um"]  = d.cnt_2_5um;
    doc["cnt_5_0um"]  = d.cnt_5_0um;
    // Omit cnt_10um entirely for PMS5003 (reserved field); backend treats
    // its absence as null so the dashboard can show "N/A".
    if (HAS_CNT_10UM) {
        doc["cnt_10um"] = d.cnt_10um;
    }

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/data");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);
    int code = http.POST(body);
    bool ok = (code == 200 || code == 201);
    Serial.printf("[HTTP] POST /api/data → %d\n", code);
    http.end();
    return ok;
}

// ─── Poll server for config ───────────────────────────────────────────────────
void fetchConfig() {
    wifiConnect();
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/config");
    http.addHeader("X-API-Key", API_KEY);
    int code = http.GET();
    if (code == 200) {
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        if (!err && doc.containsKey("interval_sec")) {
            uint32_t newInterval = doc["interval_sec"].as<uint32_t>();
            if (newInterval != g_intervalSec && newInterval >= 10) {
                Serial.printf("[Config] Interval updated: %u → %u s\n", g_intervalSec, newInterval);
                g_intervalSec = newInterval;
            }
        }
    }
    Serial.printf("[HTTP] GET /api/config → %d (interval=%u s)\n", code, g_intervalSec);
    http.end();
}

// ─── Arduino lifecycle ────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Boot] ESP32 PM Monitor starting...");

    // PMS5003 / PMS7003 both use 9600 baud, 8N1
    Serial2.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
    Serial.printf("[PMS] Model PMS%d — UART on RX=%d TX=%d\n",
                  SENSOR_MODEL, PMS_RX_PIN, PMS_TX_PIN);

    WiFi.mode(WIFI_STA);
    wifiConnect();
    fetchConfig();

    // Force an immediate first reading
    g_lastSampleMs = millis() - (g_intervalSec * 1000UL);
}

void loop() {
    uint32_t now = millis();

    // ── Config poll ──────────────────────────────────────────────────────────
    if (now - g_lastConfigMs >= CONFIG_POLL_INTERVAL_MS) {
        g_lastConfigMs = now;
        fetchConfig();
    }

    // ── Sample & send ────────────────────────────────────────────────────────
    if (now - g_lastSampleMs >= g_intervalSec * 1000UL) {
        Serial.printf("[Sample] Reading PMS%d...\n", SENSOR_MODEL);
        PmsData data;

        // Flush stale bytes first
        while (Serial2.available()) Serial2.read();

        if (readPmsFrame(data)) {
            Serial.printf(
                "[PMS] std  PM1.0=%u PM2.5=%u PM10=%u µg/m³\n"
                "[PMS] atm  PM1.0=%u PM2.5=%u PM10=%u µg/m³\n",
                data.pm1_0_std, data.pm2_5_std, data.pm10_std,
                data.pm1_0_atm, data.pm2_5_atm, data.pm10_atm
            );
            if (HAS_CNT_10UM) {
                Serial.printf(
                    "[PMS] cnt  0.3=%u 0.5=%u 1.0=%u 2.5=%u 5.0=%u 10=%u /0.1L\n",
                    data.cnt_0_3um, data.cnt_0_5um, data.cnt_1_0um,
                    data.cnt_2_5um, data.cnt_5_0um, data.cnt_10um);
            } else {
                Serial.printf(
                    "[PMS] cnt  0.3=%u 0.5=%u 1.0=%u 2.5=%u 5.0=%u /0.1L  (>10µm N/A on PMS5003)\n",
                    data.cnt_0_3um, data.cnt_0_5um, data.cnt_1_0um,
                    data.cnt_2_5um, data.cnt_5_0um);
            }
            if (postData(data)) {
                g_lastSampleMs = millis();
            } else {
                Serial.println("[HTTP] POST failed — will retry in 30 s");
                g_lastSampleMs = millis() - (g_intervalSec * 1000UL) + 30000UL;
            }
        } else {
            Serial.println("[PMS] Timeout reading frame — will retry in 30 s");
            g_lastSampleMs = millis() - (g_intervalSec * 1000UL) + 30000UL;
        }
    }

    delay(100);
}
