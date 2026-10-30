// PM2200 - ref5: 1 power meter 3 phase, baca BORONGAN 2 grup
//
// Grup 1: start 0x0BB7 (reg 3000), count 38 -> arus + tegangan     (respons 81 byte)
//         frame: 01 03 0B B7 00 26 76 12
// Grup 2: start 0x0BED (reg 3054), count 32 -> P, Q, S, PF per fase (respons 69 byte)
//         frame: 01 03 0B ED 00 20 D6 03
//
// Perbaikan dari ref3/ref4:
//  - CRC request dihitung otomatis (ref3/ref4 gagal karena CRC hardcode salah)
//  - CRC respons dicek, byte korup dari SoftwareSerial ditolak
//  - Grup 1 & 2 dibaca berturutan (jarak +-15 ms), publish HANYA jika keduanya sukses
//    -> tidak ada data basi / campuran waktu
//  - Buffer MQTT diperbesar (payload > 256 byte, default PubSubClient terlalu kecil)
//  - Serial Monitor tetap bisa dipakai untuk debug
//  - Wiring SAMA dengan ref2/ref3: SoftwareSerial RX=D7, TX=D6, DE/RE=D5

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <AsyncElegantOTA.h>
#include <PubSubClient.h>

// ---------------- USER CONFIG ----------------
// Salin secrets.example.h menjadi secrets.h dan sesuaikan secara lokal.
#include "secrets.h"

const uint8_t SLAVE_ADDR  = 0x01;
// ---------------------------------------------

#define DE_RE  D5
#define RX_PIN D7
#define TX_PIN D6

SoftwareSerial rs485(RX_PIN, TX_PIN);
AsyncWebServer server(80);
WiFiClient     espClient;
PubSubClient   mqttClient(espClient);

// ---------------- Timing ----------------
const unsigned long cycleMs               = 1000; // 1 siklus baca+publish per detik
const unsigned long groupGapMs            = 15;   // jeda antar grup
const unsigned long respTimeoutMs         = 250;
const unsigned long mqttReconnectInterval = 5000;
unsigned long lastCycle = 0;
unsigned long lastMqttReconnectAttempt = 0;

// ---------------- Definisi grup ----------------
const uint16_t G1_START = 0x0BB7;           // reg 3000 (Current A)
const uint16_t G1_COUNT = 38;               // s/d reg 3037 (Voltage L-N Avg)
const int      G1_LEN   = 5 + G1_COUNT * 2; // 81

const uint16_t G2_START = 0x0BED;           // reg 3054 (Active Power A)
const uint16_t G2_COUNT = 32;               // s/d reg 3085 (PF Total)
const int      G2_LEN   = 5 + G2_COUNT * 2; // 69

uint32_t seq      = 0; // nomor siklus sukses
uint32_t errCount = 0; // jumlah siklus gagal sejak boot

// ------------------------------------------------
// CRC16 Modbus
// ------------------------------------------------
uint16_t crc16Modbus(const uint8_t* buf, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

// ------------------------------------------------
// RS485
// ------------------------------------------------
void preTransmission()  { digitalWrite(DE_RE, HIGH); }
void postTransmission() { delayMicroseconds(200); digitalWrite(DE_RE, LOW); delayMicroseconds(200); }

void sendRequest(uint16_t start, uint16_t count) {
    uint8_t f[8];
    f[0] = SLAVE_ADDR;
    f[1] = 0x03;
    f[2] = start >> 8;  f[3] = start & 0xFF;
    f[4] = count >> 8;  f[5] = count & 0xFF;
    uint16_t c = crc16Modbus(f, 6);
    f[6] = c & 0xFF;    f[7] = c >> 8;

    while (rs485.available()) rs485.read(); // buang sisa byte lama
    preTransmission();
    rs485.write(f, 8);
    rs485.flush();
    postTransmission();
}

bool readResponse(uint8_t* buf, int len) {
    int idx = 0;
    unsigned long t0 = millis();
    while (idx < len && (millis() - t0) < respTimeoutMs) {
        if (rs485.available()) {
            uint8_t b = rs485.read();
            if (idx == 0 && b != SLAVE_ADDR) continue;      // sinkron ke alamat slave
            if (idx == 1 && b != 0x03) {                    // 0x83 = exception dari meter
                Serial.printf("  fungsi tak terduga: 0x%02X\n", b);
                return false;
            }
            buf[idx++] = b;
        }
        yield();
    }
    if (idx < len) {
        Serial.printf("  timeout (%d/%d byte)\n", idx, len);
        return false;
    }
    if (buf[2] != len - 5) {
        Serial.printf("  byte count salah: %d\n", buf[2]);
        return false;
    }
    uint16_t c = crc16Modbus(buf, len - 2);
    if (buf[len - 2] != (c & 0xFF) || buf[len - 1] != (c >> 8)) {
        Serial.println("  CRC respons salah");
        return false;
    }
    return true;
}

// Baca satu grup, retry 1x kalau gagal
bool readGroup(const char* name, uint16_t start, uint16_t count, uint8_t* buf, int len) {
    for (int attempt = 1; attempt <= 2; attempt++) {
        sendRequest(start, count);
        if (readResponse(buf, len)) return true;
        Serial.printf("[%s] gagal percobaan %d\n", name, attempt);
        delay(20);
    }
    return false;
}

// Ambil float (big-endian) dari buffer berdasarkan alamat register 0-based
float regFloat(const uint8_t* buf, uint16_t groupStart, uint16_t regAddr) {
    int off = 3 + (regAddr - groupStart) * 2;
    uint32_t raw = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                   ((uint32_t)buf[off + 2] << 8) | buf[off + 3];
    float v;
    memcpy(&v, &raw, 4);
    return v;
}

// ------------------------------------------------
// JSON builder sederhana
// ------------------------------------------------
char   payload[1024];
size_t plen = 0;

void jsonBegin() { plen = snprintf(payload, sizeof(payload), "{"); }

void jsonAdd(const char* key, float v, int dec) {
    if (plen >= sizeof(payload)) return;
    if (isnan(v) || isinf(v))
        plen += snprintf(payload + plen, sizeof(payload) - plen, "\"%s\":null,", key);
    else
        plen += snprintf(payload + plen, sizeof(payload) - plen, "\"%s\":%.*f,", key, dec, v);
}

void jsonAddU(const char* key, uint32_t v) {
    if (plen >= sizeof(payload)) return;
    plen += snprintf(payload + plen, sizeof(payload) - plen, "\"%s\":%lu,", key, (unsigned long)v);
}

bool jsonEnd() {
    if (plen >= sizeof(payload) - 1) return false; // kepotong
    if (payload[plen - 1] == ',') plen--;
    payload[plen++] = '}';
    payload[plen] = '\0';
    return true;
}

// ------------------------------------------------
// Satu siklus: baca 2 grup -> publish
// ------------------------------------------------
void readAndPublish() {
    uint8_t b1[G1_LEN];
    uint8_t b2[G2_LEN];

    unsigned long t0 = millis();
    if (!readGroup("G1", G1_START, G1_COUNT, b1, G1_LEN)) {
        errCount++;
        Serial.println("Siklus dibatalkan (G1 gagal), tidak publish");
        return;
    }
    delay(groupGapMs);
    if (!readGroup("G2", G2_START, G2_COUNT, b2, G2_LEN)) {
        errCount++;
        Serial.println("Siklus dibatalkan (G2 gagal), tidak publish");
        return;
    }
    unsigned long spanMs = millis() - t0;
    seq++;

    jsonBegin();
    jsonAddU("seq", seq);
    jsonAddU("err", errCount);
    jsonAddU("read_ms", spanMs);

    // --- Grup 1: arus & tegangan ---
    jsonAdd("current_a",   regFloat(b1, G1_START, 0x0BB7), 3); // 3000
    jsonAdd("current_b",   regFloat(b1, G1_START, 0x0BB9), 3); // 3002
    jsonAdd("current_c",   regFloat(b1, G1_START, 0x0BBB), 3); // 3004
    jsonAdd("current_avg", regFloat(b1, G1_START, 0x0BC1), 3); // 3010
    jsonAdd("voltage_ab",  regFloat(b1, G1_START, 0x0BCB), 2); // 3020
    jsonAdd("voltage_bc",  regFloat(b1, G1_START, 0x0BCD), 2); // 3022
    jsonAdd("voltage_ca",  regFloat(b1, G1_START, 0x0BCF), 2); // 3024
    jsonAdd("voltage_ll_avg", regFloat(b1, G1_START, 0x0BD1), 2); // 3026
    jsonAdd("voltage_an",  regFloat(b1, G1_START, 0x0BD3), 2); // 3028
    jsonAdd("voltage_bn",  regFloat(b1, G1_START, 0x0BD5), 2); // 3030
    jsonAdd("voltage_cn",  regFloat(b1, G1_START, 0x0BD7), 2); // 3032
    jsonAdd("voltage_avg", regFloat(b1, G1_START, 0x0BDB), 2); // 3036 (L-N avg, sama dgn ref2)

    // --- Grup 2: daya & PF (satuan kW / kVAR / kVA sesuai meter) ---
    jsonAdd("active_power_a",       regFloat(b2, G2_START, 0x0BED), 3); // 3054
    jsonAdd("active_power_b",       regFloat(b2, G2_START, 0x0BEF), 3); // 3056
    jsonAdd("active_power_c",       regFloat(b2, G2_START, 0x0BF1), 3); // 3058
    jsonAdd("active_power",         regFloat(b2, G2_START, 0x0BF3), 3); // 3060 total (sama dgn ref2)
    jsonAdd("reactive_power_total", regFloat(b2, G2_START, 0x0BFB), 3); // 3068
    jsonAdd("apparent_power_total", regFloat(b2, G2_START, 0x0C03), 3); // 3076
    // CATATAN PF: PM2200 bisa memakai format 4-kuadran (nilai -2..2).
    // Cocokkan dengan display meter sebelum dipakai untuk perhitungan.
    jsonAdd("pf_a",     regFloat(b2, G2_START, 0x0C05), 4); // 3078
    jsonAdd("pf_b",     regFloat(b2, G2_START, 0x0C07), 4); // 3080
    jsonAdd("pf_c",     regFloat(b2, G2_START, 0x0C09), 4); // 3082
    jsonAdd("pf_total", regFloat(b2, G2_START, 0x0C0B), 4); // 3084

    if (!jsonEnd()) {
        Serial.println("Payload terlalu panjang, tidak publish");
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/telemetry", baseTopic);
    if (mqttClient.publish(topic, payload)) {
        Serial.printf("[#%lu] OK (%lu ms): %s\n", (unsigned long)seq, spanMs, payload);
    } else {
        Serial.println("Gagal publish (cek setBufferSize / koneksi)");
    }
}

// ------------------------------------------------
// WiFi / MQTT / OTA
// ------------------------------------------------
void connectWiFiAtBoot() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi_ssid, wifi_password);
    Serial.printf("Menghubungkan ke WiFi %s", wifi_ssid);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi terhubung, IP: "); Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi belum terhubung, akan reconnect otomatis");
    }
}

void connectMQTT() {
    if (mqttClient.connected()) return;
    Serial.print("Mencoba koneksi MQTT... ");
    String clientId = String(deviceName) + "-" + String(ESP.getChipId(), HEX);
    char lwtTopic[128];
    snprintf(lwtTopic, sizeof(lwtTopic), "%s/status", baseTopic);
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, lwtTopic, 1, true, "offline")) {
        Serial.println("berhasil.");
        mqttClient.publish(lwtTopic, "online", true);
        char ipTopic[128];
        snprintf(ipTopic, sizeof(ipTopic), "%s/ip", baseTopic);
        mqttClient.publish(ipTopic, WiFi.localIP().toString().c_str(), true);
    } else {
        Serial.print("gagal, rc="); Serial.println(mqttClient.state());
    }
}

void setupWEBOTA() {
    if (!LittleFS.begin()) Serial.println("LittleFS mount failed");
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/plain", "PM2200 ref5 (3 phase, 2 grup) - OTA/MQTT alive");
    });
    AsyncElegantOTA.begin(&server);
    server.begin();
    Serial.println("OTA ready: http://" + WiFi.localIP().toString() + "/update");
}

// ------------------------------------------------
// Setup & Loop
// ------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(50);
    pinMode(DE_RE, OUTPUT);
    digitalWrite(DE_RE, LOW);

    // rxBuffer 256 supaya respons 81 byte tidak overflow
    rs485.begin(19200, SWSERIAL_8N1, RX_PIN, TX_PIN, false, 256, 256);
    Serial.println("RS485 @19200 8N1, rxBuffer=256");

    connectWiFiAtBoot();
    setupWEBOTA();

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(1024); // WAJIB: payload ref5 > 256 byte (butuh PubSubClient >= 2.8)
}

void loop() {
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) return; // autoReconnect jalan di background

    if (!mqttClient.connected()) {
        if (now - lastMqttReconnectAttempt >= mqttReconnectInterval) {
            lastMqttReconnectAttempt = now;
            connectMQTT();
        }
    }
    mqttClient.loop();

    if (now - lastCycle >= cycleMs) {
        lastCycle = now;
        if (mqttClient.connected()) readAndPublish();
    }
}
