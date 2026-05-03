#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <Update.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <time.h>
#include "wmbus_gama350.h"
#include "secrets.h"

#ifndef SECRET_WIFI_SSID
#error "SECRET_WIFI_SSID is not defined. Create include/secrets.h based on include/secrets.h.example"
#endif

#ifndef SECRET_WIFI_PASSWORD
#error "SECRET_WIFI_PASSWORD is not defined. Create include/secrets.h based on include/secrets.h.example"
#endif

#ifndef SECRET_GAMA350_AES_KEY_HEX
#error "SECRET_GAMA350_AES_KEY_HEX is not defined. Create include/secrets.h based on include/secrets.h.example"
#endif

// =========================
// WiFi / NTP
// =========================

const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* FALLBACK_AP_SSID = "ESP32-WMBUS";
const char* FALLBACK_AP_PASS = "esp32wmbus";
const char* NTP_SERVER1   = "pool.ntp.org";
const char* NTP_SERVER2   = "time.nist.gov";
const char* GAMA350_AES_KEY_HEX = SECRET_GAMA350_AES_KEY_HEX;
const uint32_t GAMA350_METER_SERIAL = 0x31676464;
const uint8_t GAMA350_SERIAL_BCD_LE[4] = {0x64, 0x64, 0x67, 0x31};
const uint8_t GAMA350_SERIAL_BCD_BE[4] = {0x31, 0x67, 0x64, 0x64};
const uint8_t GAMA350_MFG_EGM_LE[2] = {0xED, 0x14};

const float WMBUS_FREQ_DEFAULT_MHZ = 868.95f;
const char* LOG_PATH = "/logcc1101.ndjson";
const char* LOG_PATH_ROTATED = "/logcc1101_prev.ndjson";
const char* LOG_PATH_METER = "/logcc1101_meter.ndjson";
const size_t LOG_MAX_BYTES = 384 * 1024;
const unsigned long DIAG_WINDOW_MS = 30UL * 60UL * 1000UL;
const byte MAX_REASONABLE_WMBUS_FRAME_LEN = 192;

struct OmsScanProfile {
  const char* name;
  float freq;
  uint8_t syncH;
  uint8_t syncL;
  uint8_t syncMode;
  uint8_t manchester;
  uint16_t rxBw;
  bool modeS;
};

static const OmsScanProfile OMS_SCAN_PROFILES[] = {
    {"S1-868.300", 868.300f, 0x76, 0x96, 2, 0, 325, true},
    {"T1-868.950", 868.950f, 0x54, 0x3D, 2, 1, 325, false},
    {"C1-868.950", 868.950f, 0x54, 0x3D, 2, 0, 325, false},
    {"C1w-868.950", 868.950f, 0x54, 0x3D, 2, 0, 541, false},
};
const int OMS_SCAN_PROFILE_COUNT = sizeof(OMS_SCAN_PROFILES) / sizeof(OMS_SCAN_PROFILES[0]);

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
const int STATUS_LED_PIN = LED_BUILTIN;
const bool STATUS_LED_ACTIVE_LOW = true;

WebServer g_server(80);
String g_resetTime = "";
uint32_t g_foundIdCount = 0;
unsigned long g_resetMillis = 0;
unsigned long g_diagStartMs = 0;
bool g_diagActive = false;
uint32_t g_diagFrameCount = 0;
uint32_t g_diagMatchCount = 0;
unsigned long g_diagLastMatchMs = 0;
unsigned long g_diagPrevMatchMs = 0;
uint64_t g_diagMatchIntervalSumMs = 0;
uint32_t g_diagMatchIntervalCount = 0;
bool g_radioWideMode = false;
float g_radioFreqMhz = WMBUS_FREQ_DEFAULT_MHZ;
bool g_radioModeS = false;
bool g_autoScanMode = false;
int g_autoScanIdx = 0;
unsigned long g_autoScanLastHopMs = 0;
uint32_t g_rejectedFrameCount = 0;
String g_activeRadioProfile = "Mode T";

String formatElapsedSinceReset() {
  unsigned long elapsedSec = (millis() - g_resetMillis) / 1000UL;
  unsigned long days = elapsedSec / 86400UL;
  elapsedSec %= 86400UL;
  unsigned long hours = elapsedSec / 3600UL;
  elapsedSec %= 3600UL;
  unsigned long minutes = elapsedSec / 60UL;
  unsigned long seconds = elapsedSec % 60UL;

  char buf[48];
  snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  return String(buf);
}

void resetDiagWindow() {
  g_diagStartMs = millis();
  g_diagActive = true;
  g_diagFrameCount = 0;
  g_diagMatchCount = 0;
  g_diagLastMatchMs = 0;
  g_diagPrevMatchMs = 0;
  g_diagMatchIntervalSumMs = 0;
  g_diagMatchIntervalCount = 0;
}

String diagStatusJson() {
  unsigned long nowMs = millis();
  if (g_diagActive && (nowMs - g_diagStartMs >= DIAG_WINDOW_MS)) {
    g_diagActive = false;
  }

  unsigned long elapsedSec = (nowMs - g_diagStartMs) / 1000UL;
  unsigned long remainingSec = 0;
  if (g_diagActive && elapsedSec < (DIAG_WINDOW_MS / 1000UL)) {
    remainingSec = (DIAG_WINDOW_MS / 1000UL) - elapsedSec;
  }

  unsigned long avgIntervalSec = 0;
  if (g_diagMatchIntervalCount > 0) {
    avgIntervalSec = (unsigned long)((g_diagMatchIntervalSumMs / g_diagMatchIntervalCount) / 1000ULL);
  }

  String out = "{";
  out += "\"active\":" + String(g_diagActive ? "true" : "false") + ",";
  out += "\"elapsed_sec\":" + String(elapsedSec) + ",";
  out += "\"remaining_sec\":" + String(remainingSec) + ",";
  out += "\"total_frames\":" + String(g_diagFrameCount) + ",";
  out += "\"meter_matches\":" + String(g_diagMatchCount) + ",";
  out += "\"avg_match_interval_sec\":" + String(avgIntervalSec) + ",";
  out += "\"radio_mode\":\"" + String(g_radioWideMode ? "wide" : "normal") + "\",";
  out += "\"profile\":\"" + g_activeRadioProfile + "\",";
  out += "\"channel_mhz\":" + String(g_radioFreqMhz, 3) + ",";
  out += "\"rejected_frames\":" + String(g_rejectedFrameCount) + ",";
  out += "\"last_match_ago_sec\":";
  if (g_diagLastMatchMs > 0) {
    out += String((nowMs - g_diagLastMatchMs) / 1000UL);
  } else {
    out += "-1";
  }
  out += "}";
  return out;
}

const char* wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void logWifiScanResults() {
  Serial.println("Skan WiFi...");
  int networkCount = WiFi.scanNetworks();
  if (networkCount <= 0) {
    Serial.println("Brak widocznych sieci WiFi");
    return;
  }

  Serial.print("Widoczne sieci: ");
  Serial.println(networkCount);
  for (int i = 0; i < networkCount; i++) {
    Serial.print(" - ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" | RSSI ");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm | ");
    Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  }
}

void setStatusLed(bool on) {
  bool pinState = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STATUS_LED_PIN, pinState ? HIGH : LOW);
}

void setupStatusLed() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  setStatusLed(false);

  Serial.print("Laczenie z WiFi ");
  Serial.print(WIFI_SSID);
  Serial.print(" ... ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  unsigned long lastBlink = 0;
  bool ledOn = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    if (millis() - lastBlink >= 250) {
      ledOn = !ledOn;
      setStatusLed(ledOn);
      lastBlink = millis();
    }
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(true);
    Serial.println("WiFi polaczone");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    setStatusLed(false);
    Serial.println("Brak polaczenia WiFi");
    Serial.print("Status WiFi: ");
    Serial.println(wifiStatusText(WiFi.status()));
    logWifiScanResults();

    WiFi.mode(WIFI_AP_STA);
    bool apOk = WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS);
    if (apOk) {
      Serial.print("Tryb awaryjny AP: ");
      Serial.println(FALLBACK_AP_SSID);
      Serial.print("Haslo AP: ");
      Serial.println(FALLBACK_AP_PASS);
      Serial.print("IP AP: ");
      Serial.println(WiFi.softAPIP());
    } else {
      Serial.println("BLAD uruchomienia AP awaryjnego");
    }
  }
}

void disablePowerSave() {
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.println("Tryb oszczedzania energii: WYLACZONY");
}

bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isValidAes128HexKey(const char* keyHex) {
  if (keyHex == nullptr) return false;
  for (int i = 0; i < 32; i++) {
    if (keyHex[i] == '\0' || !isHexChar(keyHex[i])) return false;
  }
  return keyHex[32] == '\0';
}

bool containsPattern(const uint8_t* data, int len, const uint8_t* pattern, int patternLen) {
  if (data == nullptr || pattern == nullptr || len <= 0 || patternLen <= 0 || len < patternLen) return false;

  for (int i = 0; i <= len - patternLen; i++) {
    bool same = true;
    for (int j = 0; j < patternLen; j++) {
      if (data[i + j] != pattern[j]) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

bool isLikelyCorruptedFrame(const uint8_t* data, byte len) {
  if (data == nullptr || len == 0) return true;

  // In this setup, lengths near the 255-byte CC1101 packet limit have proven to be
  // repeated FIFO garbage rather than real Wireless M-Bus telegrams.
  return len > MAX_REASONABLE_WMBUS_FRAME_LEN;
}

String timestamp();
String timestampIso();
String hexLine(const uint8_t *data, int len);
void applyRadioProfile(bool wideMode);
void applyOmsScanProfile(int idx);

unsigned long g_lastHeartbeatMs = 0;
unsigned long g_lastFrameMs = 0;
uint32_t g_frameCount = 0;

void setupStorage() {
  if (SPIFFS.begin(true)) {
    Serial.println("SPIFFS: OK");
  } else {
    Serial.println("SPIFFS: BLAD MONTOWANIA");
  }
}

void rotateLogIfNeeded() {
  if (!SPIFFS.exists(LOG_PATH)) return;

  File f = SPIFFS.open(LOG_PATH, "r");
  if (!f) return;

  size_t sz = f.size();
  f.close();

  if (sz < LOG_MAX_BYTES) return;

  if (SPIFFS.exists(LOG_PATH_ROTATED)) {
    SPIFFS.remove(LOG_PATH_ROTATED);
  }
  SPIFFS.rename(LOG_PATH, LOG_PATH_ROTATED);
  Serial.println("Log SPIFFS: rotacja");
}

void appendFrameLogNdjson(
    byte len,
    int rssi,
    int lqi,
    bool encrypted,
    bool hasHeaderId,
    uint32_t id,
    bool meterIdMatch,
    bool meterIdMatchRaw,
    const Gama350Data& meter,
    const uint8_t* buf,
    uint8_t marcState = 0x0D) {
  if (!buf) return;

  rotateLogIfNeeded();

  File logFile = SPIFFS.open(LOG_PATH, "a");
  if (!logFile) return;

  String dataHex = hexLine(buf, len);
  dataHex.trim();

  String line = "{";
  line += "\"ts\":\"" + timestampIso() + "\",";
  line += "\"epoch\":" + String((uint32_t)time(nullptr)) + ",";
  line += "\"rssi\":" + String(rssi) + ",";
  line += "\"lqi\":" + String(lqi) + ",";
  line += "\"len\":" + String(len) + ",";
  line += "\"meter_match\":" + String(meterIdMatch ? "true" : "false") + ",";
  line += "\"meter_raw_match\":" + String(meterIdMatchRaw ? "true" : "false") + ",";
  line += "\"has_header_id\":" + String(hasHeaderId ? "true" : "false") + ",";
  line += "\"header_id\":\"";
  if (hasHeaderId) {
    line += String(id, HEX);
  } else {
    line += "";
  }
  line += "\",";
  line += "\"encrypted\":" + String(encrypted ? "true" : "false") + ",";
  line += "\"channel_mhz\":" + String(g_radioFreqMhz, 3) + ",";
  line += "\"energy\":" + String(meter.valid ? meter.energy : 0) + ",";
  line += "\"power\":" + String(meter.valid ? meter.power : 0) + ",";
  line += "\"meter_decoded\":" + String(meter.valid ? "true" : "false");
  char marcBuf[8];
  snprintf(marcBuf, sizeof(marcBuf), "0x%02X", marcState);
  line += ",\"marc_state\":\"" + String(marcBuf) + "\"";
  line += ",\"data_hex\":\"" + dataHex + "\"";
  line += "}";

  logFile.println(line);
  logFile.close();

  if (meterIdMatch) {
    File meterFile = SPIFFS.open(LOG_PATH_METER, "a");
    if (meterFile) {
      meterFile.println(line);
      meterFile.close();
    }
  }
}

void setupWebServer() {
  g_server.on("/", HTTP_GET, []() {
    String nowTime = timestampIso();
    String elapsedFromReset = formatElapsedSinceReset();
    String rfMode = g_radioWideMode ? "WIDE" : (g_radioModeS ? "Mode S" : "Mode T");
    String btnModeT = (!g_radioWideMode && !g_radioModeS) ? "btn-blue" : "btn-gray";
    String btnModeS = (!g_radioWideMode && g_radioModeS) ? "btn-blue" : "btn-gray";
    String btnWide  = g_radioWideMode ? "btn-blue" : "btn-gray";
    String btnScan  = g_autoScanMode  ? "btn-blue" : "btn-gray";
    String body =
        "<!doctype html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='300'><title>WMBus logger</title>"
        "<style>"
        "body{font-family:Verdana,sans-serif;background:#f3f4f6;color:#1f2937;margin:0;padding:24px;}"
        ".card{max-width:720px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;"
        "box-shadow:0 8px 24px rgba(0,0,0,.08);}"
        "h2{margin:0 0 16px 0;}"
        "h3{margin:14px 0 6px 0;font-size:13px;text-transform:uppercase;color:#6b7280;letter-spacing:.05em;}"
        ".row{display:flex;gap:8px;flex-wrap:wrap;}"
        "a.btn{display:inline-block;padding:9px 13px;border:0;border-radius:8px;"
        "text-decoration:none;cursor:pointer;font-size:13px;font-weight:600;}"
        ".btn-teal{background:#0f766e;color:#fff;} .btn-teal:hover{background:#115e59;}"
        ".btn-blue{background:#1d4ed8;color:#fff;} .btn-blue:hover{background:#1e40af;}"
        ".btn-amber{background:#b45309;color:#fff;} .btn-amber:hover{background:#92400e;}"
        ".btn-red{background:#dc2626;color:#fff;} .btn-red:hover{background:#b91c1c;}"
        ".btn-gray{background:#6b7280;color:#fff;} .btn-gray:hover{background:#4b5563;}"
        "code{background:#e5e7eb;padding:2px 6px;border-radius:5px;}"
        ".info{font-size:13px;line-height:1.7;margin-top:14px;border-top:1px solid #e5e7eb;padding-top:12px;}"
        "</style></head><body>"
        "<div class='card'>"
        "<h2>WMBus logger online</h2>"

        "<h3>Logi</h3>"
        "<div class='row'>"
        "<a class='btn btn-teal' href='/log'>Biezacy log</a>"
        "<a class='btn btn-teal' href='/log_prev'>Poprzedni log</a>"
        "<a class='btn btn-teal' href='/log_meter'>Log licznika</a>"
        "<a class='btn btn-red'  href='/log_clear'>Wyczysc logi</a>"
        "</div>"

        "<h3>Radio CC1101</h3>"
        "<div class='row'>"
        "<a class='btn " + btnModeT + "' href='/mode_t'>Mode T (868.95)</a>"
        "<a class='btn " + btnModeS + "' href='/mode_s'>Mode S (868.30)</a>"
        "<a class='btn " + btnWide  + "' href='/radio_wide'>WIDE</a>"
        "</div>"
        "<div class='row' style='margin-top:8px;'>"
        "<a class='btn " + String((g_radioFreqMhz > 868.29f && g_radioFreqMhz < 868.31f) ? "btn-blue" : "btn-gray") + "' href='/freq_868300'>868.300</a>"
        "<a class='btn " + String((g_radioFreqMhz > 868.94f && g_radioFreqMhz < 868.96f) ? "btn-blue" : "btn-gray") + "' href='/freq_868950'>868.950</a>"
        "<a class='btn " + String((g_radioFreqMhz > 869.51f && g_radioFreqMhz < 869.54f) ? "btn-blue" : "btn-gray") + "' href='/freq_869525'>869.525</a>"
        "</div>"
        "<div class='row' style='margin-top:8px;'>"
        "<a class='btn " + btnScan + "' href='/scan_start'>AUTO-SCAN wl</a>"
        "<a class='btn btn-gray' href='/scan_stop'>AUTO-SCAN wyl</a>"
        "</div>"

        "<h3>Diagnostyka</h3>"
        "<div class='row'>"
        "<a class='btn btn-amber' href='/diag'>Status DIAG</a>"
        "<a class='btn btn-amber' href='/diag_reset'>Reset okna DIAG</a>"
        "</div>"

        "<h3>System</h3>"
        "<div class='row'>"
        "<a class='btn btn-teal' href='/update'>OTA update</a>"
        "<a class='btn btn-red'  href='/restart'>Restart ESP32</a>"
        "</div>"

        "<div class='info'>"
        "Aktualny czas: <strong>" + nowTime + "</strong><br>"
        "Ostatni reset: <strong>" + g_resetTime + "</strong><br>"
        "Uplynelo od resetu: <strong>" + elapsedFromReset + "</strong><br>"
        "Tryb RF: <strong>" + rfMode + "</strong><br>"
        "Profil RX: <strong>" + g_activeRadioProfile + "</strong><br>"
        "Kanal MHz: <strong>" + String(g_radioFreqMhz, 3) + "</strong><br>"
        "Znalezione ID GAMA350: <strong>" + String(g_foundIdCount) + "</strong><br>"
        "Ilosc komunikatow: <strong>" + String(g_frameCount) + "</strong><br>"
        "Odrzucone ramki: <strong>" + String(g_rejectedFrameCount) + "</strong><br>"
        "Auto-scan: <strong>" + String(g_autoScanMode ? "TAK" : "NIE") + "</strong>"
        "</div>"
        "</div></body></html>";
    g_server.send(200, "text/html", body);
  });

  g_server.on("/log", HTTP_GET, []() {
    if (!SPIFFS.exists(LOG_PATH)) {
      g_server.send(404, "text/plain", "Brak pliku logu");
      return;
    }
    File f = SPIFFS.open(LOG_PATH, "r");
    g_server.streamFile(f, "text/plain");
    f.close();
  });

  g_server.on("/log_prev", HTTP_GET, []() {
    if (!SPIFFS.exists(LOG_PATH_ROTATED)) {
      g_server.send(404, "text/plain", "Brak poprzedniego pliku logu");
      return;
    }
    File f = SPIFFS.open(LOG_PATH_ROTATED, "r");
    g_server.streamFile(f, "text/plain");
    f.close();
  });

  g_server.on("/log_meter", HTTP_GET, []() {
    if (!SPIFFS.exists(LOG_PATH_METER)) {
      g_server.send(404, "text/plain", "Brak logu ramek licznika");
      return;
    }
    File f = SPIFFS.open(LOG_PATH_METER, "r");
    g_server.streamFile(f, "text/plain");
    f.close();
  });

  g_server.on("/diag", HTTP_GET, []() {
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/diag_reset", HTTP_GET, []() {
    resetDiagWindow();
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/radio_toggle", HTTP_GET, []() {
    g_radioWideMode = !g_radioWideMode;
    applyRadioProfile(g_radioWideMode);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/radio_normal", HTTP_GET, []() {
    g_radioWideMode = false;
    applyRadioProfile(false);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/radio_wide", HTTP_GET, []() {
    g_radioWideMode = true;
    applyRadioProfile(true);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/freq_868300", HTTP_GET, []() {
    g_radioFreqMhz = 868.300f;
    applyRadioProfile(g_radioWideMode);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/freq_868950", HTTP_GET, []() {
    g_radioFreqMhz = 868.950f;
    applyRadioProfile(g_radioWideMode);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/freq_869525", HTTP_GET, []() {
    g_radioFreqMhz = 869.525f;
    applyRadioProfile(g_radioWideMode);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/mode_t", HTTP_GET, []() {
    g_autoScanMode = false;
    g_radioModeS = false;
    g_radioFreqMhz = 868.950f;
    applyRadioProfile(false);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/mode_s", HTTP_GET, []() {
    g_autoScanMode = false;
    g_radioModeS = true;
    g_radioFreqMhz = 868.300f;
    applyRadioProfile(false);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/scan_start", HTTP_GET, []() {
    g_autoScanMode = true;
    g_autoScanIdx = 0;
    g_autoScanLastHopMs = millis();
    applyOmsScanProfile(g_autoScanIdx);
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/scan_stop", HTTP_GET, []() {
    g_autoScanMode = false;
    g_server.send(200, "application/json", diagStatusJson());
  });

  g_server.on("/log_clear", HTTP_POST, []() {
    if (SPIFFS.exists(LOG_PATH)) SPIFFS.remove(LOG_PATH);
    if (SPIFFS.exists(LOG_PATH_METER)) SPIFFS.remove(LOG_PATH_METER);
    g_server.send(200, "text/plain", "OK");
  });

  g_server.on("/log_clear", HTTP_GET, []() {
    if (SPIFFS.exists(LOG_PATH)) SPIFFS.remove(LOG_PATH);
    if (SPIFFS.exists(LOG_PATH_METER)) SPIFFS.remove(LOG_PATH_METER);
    g_server.send(200, "text/plain", "OK - log wyczyszczony");
  });

  g_server.on("/restart", HTTP_POST, []() {
    g_server.send(200, "text/plain", "Restart ESP32...");
    Serial.println("Restart z HTTP /restart");
    delay(250);
    ESP.restart();
  });

  g_server.on("/restart", HTTP_GET, []() {
    g_server.send(200, "text/plain", "Restart ESP32...");
    Serial.println("Restart z HTTP /restart");
    delay(250);
    ESP.restart();
  });

  g_server.on("/update", HTTP_GET, []() {
    String html =
        "<!doctype html><html><head><meta charset='utf-8'><title>ESP32 OTA</title></head><body>"
        "<h2>ESP32 OTA Update</h2>"
        "<form method='POST' action='/update' enctype='multipart/form-data'>"
        "<input type='file' name='firmware' accept='.bin' required>"
        "<button type='submit'>Upload</button>"
        "</form>"
        "<p>Wybierz plik firmware.bin z .pio/build/esp32dev/</p>"
        "</body></html>";
    g_server.send(200, "text/html", html);
  });

  g_server.on(
      "/update",
      HTTP_POST,
      []() {
        bool success = !Update.hasError();
        g_server.sendHeader("Connection", "close");
        g_server.send(200, "text/plain", success ? "OTA OK - restart" : "OTA ERROR");
        if (success) {
          Serial.println("OTA zakonczone, restart...");
          delay(250);
          ESP.restart();
        }
      },
      []() {
        HTTPUpload& upload = g_server.upload();

        if (upload.status == UPLOAD_FILE_START) {
          Serial.printf("OTA start: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("OTA koniec: %u B\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          Update.abort();
          Serial.println("OTA przerwane");
        }
      });

  g_server.begin();
  Serial.println("HTTP logger: /log /log_prev /log_meter /diag /diag_reset /radio_toggle /radio_normal /radio_wide /freq_868300 /freq_868950 /freq_869525 /mode_t /mode_s /scan_start /scan_stop /log_clear /update /restart");
}

void setupTime() {
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", NTP_SERVER1, NTP_SERVER2);
  Serial.println("Pobieranie czasu NTP...");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    char tzBuf[16];
    strftime(tzBuf, sizeof(tzBuf), "%Z", &timeinfo);
    Serial.print("Czas zaktualizowany: ");
    Serial.print(timestamp());
    Serial.print(" (strefa: ");
    Serial.print(tzBuf);
    Serial.println(")");
  } else {
    Serial.println("NTP nie odpowiedzialo, uzyj SETTIME do recznego ustawienia");
  }
}

// =========================
// Konfiguracja CC1101
// =========================

void setup_radio() {
  ELECHOUSE_cc1101.setGDO(4, 27);  // GDO0 = GPIO4, GDO2 = GPIO27
  ELECHOUSE_cc1101.Init();
  applyRadioProfile(false);
}

void applyRadioProfile(bool wideMode) {
  g_radioWideMode = wideMode;

  ELECHOUSE_cc1101.setMHZ(g_radioFreqMhz);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setDRate(32768);
  ELECHOUSE_cc1101.setRxBW(wideMode ? 812 : 325);
  ELECHOUSE_cc1101.setDeviation(50);

  if (wideMode) {
    ELECHOUSE_cc1101.setSyncWord(0x54, 0x3D);
    ELECHOUSE_cc1101.setSyncMode(0);
    ELECHOUSE_cc1101.setManchester(0);
    g_activeRadioProfile = "WIDE-868";
  } else if (g_radioModeS) {
    ELECHOUSE_cc1101.setSyncWord(0x76, 0x96);
    ELECHOUSE_cc1101.setSyncMode(2);
    ELECHOUSE_cc1101.setManchester(0);
    g_activeRadioProfile = "S1-868.300";
  } else {
    ELECHOUSE_cc1101.setSyncWord(0x54, 0x3D);
    ELECHOUSE_cc1101.setSyncMode(2);
    ELECHOUSE_cc1101.setManchester(1);
    g_activeRadioProfile = "T1-868.950";
  }
  ELECHOUSE_cc1101.setPktFormat(0);
  ELECHOUSE_cc1101.setPacketLength(255);
  ELECHOUSE_cc1101.setLengthConfig(1);
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setWhiteData(0);

  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x03);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x40);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x91);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FIFOTHR, wideMode ? 0x07 : 0x47);

  ELECHOUSE_cc1101.SetRx();

  Serial.print("Nasluch WMBus na stale (");
  if (wideMode) Serial.print("WIDE");
  else if (g_radioModeS) Serial.print("Mode S");
  else Serial.print("Mode T");
  Serial.print(") kanal ");
  Serial.print(g_radioFreqMhz, 3);
  Serial.println(" MHz");
}

void applyOmsScanProfile(int idx) {
  if (idx < 0 || idx >= OMS_SCAN_PROFILE_COUNT) {
    idx = 0;
  }

  const OmsScanProfile& p = OMS_SCAN_PROFILES[idx];
  g_autoScanIdx = idx;
  g_radioWideMode = false;
  g_radioModeS = p.modeS;
  g_radioFreqMhz = p.freq;
  g_activeRadioProfile = String(p.name);

  ELECHOUSE_cc1101.setMHZ(p.freq);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setDRate(32768);
  ELECHOUSE_cc1101.setRxBW(p.rxBw);
  ELECHOUSE_cc1101.setDeviation(50);
  ELECHOUSE_cc1101.setSyncWord(p.syncH, p.syncL);
  ELECHOUSE_cc1101.setSyncMode(p.syncMode);
  ELECHOUSE_cc1101.setManchester(p.manchester);
  ELECHOUSE_cc1101.setPktFormat(0);
  ELECHOUSE_cc1101.setPacketLength(255);
  ELECHOUSE_cc1101.setLengthConfig(1);
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setWhiteData(0);

  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x03);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x40);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x91);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FIFOTHR, 0x47);
  ELECHOUSE_cc1101.SetRx();
}

// =========================
// Pomocnicze funkcje
// =========================

String hexLine(const uint8_t *data, int len) {
  String out = "";
  for (int i = 0; i < len; i++) {
    if (data[i] < 16) out += "0";
    out += String(data[i], HEX) + " ";
  }
  return out;
}

String timestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "[NO TIME] ";
  char buf[32];
  strftime(buf, sizeof(buf), "[%H:%M:%S] ", &t);
  return String(buf);
}

String timestampIso() {
  struct tm t;
  if (!getLocalTime(&t)) return "1970-01-01 00:00:00";
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// =========================
// Ustawianie czasu z komputera
// =========================

void handleSetTime(String cmd) {
  // Format: SETTIME YYYY-MM-DD HH:MM:SS
  if (!cmd.startsWith("SETTIME ")) return;

  int y, M, d, h, m, s;
  if (sscanf(cmd.c_str(), "SETTIME %d-%d-%d %d:%d:%d", &y, &M, &d, &h, &m, &s) != 6) {
    Serial.println("BLEDNY FORMAT! Uzyj:");
    Serial.println("SETTIME 2026-04-11 12:34:56");
    return;
  }

  struct tm t;
  t.tm_year = y - 1900;
  t.tm_mon  = M - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = m;
  t.tm_sec  = s;

  time_t tt = mktime(&t);
  struct timeval now = { .tv_sec = tt };
  settimeofday(&now, NULL);

  Serial.print("Czas ustawiony na: ");
  Serial.println(timestamp());
}

// =========================
// Setup
// =========================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=== WMBus Sniffer – AUTO TIME ===");

  setupStatusLed();
  connectWifi();
  disablePowerSave();
  setupTime();
  g_resetMillis = millis();
  g_resetTime = timestampIso();
  resetDiagWindow();
  setupStorage();
  setupWebServer();

  Serial.print("Klucz AES licznika: ");
  Serial.println(isValidAes128HexKey(GAMA350_AES_KEY_HEX) ? "zaladowany" : "BLEDNY FORMAT");
  Serial.print("Numer licznika GAMA350: ");
  Serial.println(GAMA350_METER_SERIAL);

  setup_radio();
  Serial.println("Radio CC1101 gotowe");
}

// =========================
// Loop
// =========================

void loop() {
  g_server.handleClient();

  // Obsługa komend z terminala
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    handleSetTime(cmd);
  }

  unsigned long nowMs = millis();

  // Auto-scan: automatyczne przeskakiwanie miedzy trybami/kanalami
  if (g_autoScanMode) {
    const unsigned long SCAN_DWELL_MS = 2UL * 60UL * 1000UL;
    if (nowMs - g_autoScanLastHopMs >= SCAN_DWELL_MS) {
      g_autoScanLastHopMs = nowMs;
      g_autoScanIdx = (g_autoScanIdx + 1) % OMS_SCAN_PROFILE_COUNT;
      applyOmsScanProfile(g_autoScanIdx);
      Serial.print(timestamp());
      Serial.print("AUTO-SCAN: profil ");
      Serial.print(g_activeRadioProfile);
      Serial.print(", kanal ");
      Serial.print(g_radioFreqMhz, 3);
      Serial.println(" MHz");
    }
  }

  if (g_diagActive && (nowMs - g_diagStartMs >= DIAG_WINDOW_MS)) {
    g_diagActive = false;
    Serial.println("[DIAG] Okno 30 min zakonczone");
  }

  if (nowMs - g_lastHeartbeatMs >= 10000) {
    g_lastHeartbeatMs = nowMs;
    int bgRssi = ELECHOUSE_cc1101.getRssi();
    uint8_t marc = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1F;

    if (marc != 0x0D) {  // 0x0D = RX — jesli nie nasluchuje, wymuszamy powrot
      Serial.print(timestamp());
      Serial.print("UWAGA: MARCSTATE=0x");
      Serial.print(marc, HEX);
      if (marc == 0x11) {
        Serial.println(" = RXFIFO_OVERFLOW — recovery SIDLE→SFRX→SRX");
        ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
        delay(1);
        ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
        delay(1);
        ELECHOUSE_cc1101.SpiStrobe(CC1101_SRX);
      } else {
        Serial.println(" (nie jest RX=0x0D) — wymuszam SetRx()");
        ELECHOUSE_cc1101.SetRx();
      }
    }

    Serial.print(timestamp());
    Serial.print("Nasluch aktywny, brak nowych ramek. Odebrane: ");
    Serial.print(g_frameCount);
    Serial.print(", kanal: ");
    Serial.print(g_radioFreqMhz, 3);
    Serial.print(" MHz, MARC=0x");
    Serial.print(marc, HEX);
    Serial.print(", RSSI tla: ");
    Serial.print(bgRssi);
    Serial.print(" dBm");
    if (g_lastFrameMs > 0) {
      Serial.print(", cisza od: ");
      Serial.print((nowMs - g_lastFrameMs) / 1000);
      Serial.print(" s");
    }
    Serial.println();
  }

  // Odbiór ramek
  if (!ELECHOUSE_cc1101.CheckRxFifo(1)) return;

  uint8_t buf[256];
  byte len = ELECHOUSE_cc1101.ReceiveData(buf);
  uint8_t marcAtRecv = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1F;
  ELECHOUSE_cc1101.SetRx();  // zawsze restart RX po odczycie (CC1101 wraca do IDLE po odebraniu ramki)
  if (len == 0) return;
  if (isLikelyCorruptedFrame(buf, len)) {
    g_rejectedFrameCount++;
    Serial.println("=== ODRZUCONA RAMKA ===");
    Serial.print(timestamp());
    Serial.print("Powod: podejrzana dlugosc ");
    Serial.print(len);
    Serial.print(" B, MARC=0x");
    Serial.println(marcAtRecv, HEX);
    return;
  }

  int rssi = ELECHOUSE_cc1101.getRssi();
  int lqi  = ELECHOUSE_cc1101.getLqi();

  uint32_t id = 0;
  bool hasHeaderId = (len >= 5);
  if (hasHeaderId) {
    id =
        (uint32_t)buf[1] |
        ((uint32_t)buf[2] << 8) |
        ((uint32_t)buf[3] << 16) |
        ((uint32_t)buf[4] << 24);
  }

  bool meterIdMatchHeader = hasHeaderId && (id == GAMA350_METER_SERIAL);
  bool meterIdMatchRaw = containsPattern(buf, len, GAMA350_SERIAL_BCD_LE, 4) || containsPattern(buf, len, GAMA350_SERIAL_BCD_BE, 4);
  bool meterIdMatch = meterIdMatchHeader || meterIdMatchRaw;
  g_lastFrameMs = nowMs;
  g_frameCount++;

  bool hasEgmSignature = containsPattern(buf, len, GAMA350_MFG_EGM_LE, 2);
  bool hasTypicalT1Start = (len >= 2 && buf[0] == 0xBE && buf[1] == 0x44);
  // Soft candidate filter: allow ID/BCD match OR EGM signature OR typical T1 start.
  // This reduces the chance of dropping valid GAMA frames with slightly different layouts.
  bool frameLooksLikeGama = meterIdMatch || hasEgmSignature || hasTypicalT1Start;

  if (!frameLooksLikeGama) {
    g_rejectedFrameCount++;
    Serial.println("=== ODRZUCONA RAMKA ===");
    Serial.print(timestamp());
    Serial.print("Powod: obcy telegram (brak ID/BCD GAMA, sygnatury EGM i startu T1)");
    if (hasHeaderId) {
      Serial.print(", header_id=0x");
      Serial.print(id, HEX);
    }
    Serial.print(", len=");
    Serial.print(len);
    Serial.print(", RSSI=");
    Serial.print(rssi);
    Serial.println(" dBm");
    return;
  }

  if (meterIdMatch) g_foundIdCount++;

  bool encrypted = (len >= 3 && buf[1] == 0x2F && buf[2] == 0x2F);
  Gama350Data meter = decodeGama350(buf, len, GAMA350_AES_KEY_HEX);

  if (g_diagActive) {
    g_diagFrameCount++;
    if (meterIdMatch) {
      g_diagMatchCount++;
      if (g_diagLastMatchMs > 0) {
        g_diagMatchIntervalSumMs += (uint64_t)(nowMs - g_diagLastMatchMs);
        g_diagMatchIntervalCount++;
      }
      g_diagPrevMatchMs = g_diagLastMatchMs;
      g_diagLastMatchMs = nowMs;
    }
  }

  Serial.println("=== RAMKA WMBUS ===");
  Serial.print(timestamp());
  Serial.print("RSSI: ");
  Serial.print(rssi);
  Serial.print(" dBm, LQI: ");
  Serial.print(lqi);
  Serial.print(", MARC=0x");
  Serial.print(marcAtRecv, HEX);
  if (marcAtRecv != 0x0D && marcAtRecv != 0x01) {
    Serial.print(" [!BLAD STANU]");
  }
  Serial.println();

  Serial.print("ID (naglowek): ");
  if (hasHeaderId) {
    Serial.print("0x");
    Serial.println(id, HEX);
  } else {
    Serial.println("BRAK (ramka krotsza niz 6 bajtow)");
  }

  Serial.print("ID LICZNIKA (oczekiwany): ");
  Serial.println(GAMA350_METER_SERIAL);

  Serial.print("ZGODNOSC Z TWOIM LICZNIKIEM: ");
  Serial.println(meterIdMatch ? "TAK" : "NIE");

  Serial.print("DOPASOWANIE RAW BCD 31676464: ");
  Serial.println(meterIdMatchRaw ? "TAK" : "NIE");

  Serial.print("LICZNIK RAMEK: ");
  Serial.println(g_frameCount);

  Serial.print("KANAL MHz: ");
  Serial.println(g_radioFreqMhz, 3);

  Serial.print("SZYFROWANIE: ");
  Serial.println(encrypted ? "TAK" : "NIE");
  Serial.print("SYGNATURA EGM: ");
  Serial.println(hasEgmSignature ? "TAK" : "NIE");
  Serial.print("START T1 (BE44): ");
  Serial.println(hasTypicalT1Start ? "TAK" : "NIE");

  if (meter.valid) {
    Serial.print("ENERGIA: ");
    Serial.print(meter.energy);
    Serial.print(" Wh, MOC: ");
    Serial.print(meter.power);
    Serial.println(" W");
  }

  Serial.print("DANE: ");
  Serial.println(hexLine(buf, len));

  appendFrameLogNdjson(len, rssi, lqi, encrypted, hasHeaderId, id, meterIdMatch, meterIdMatchRaw, meter, buf, marcAtRecv);

  Serial.println("-----------------------------");
}
