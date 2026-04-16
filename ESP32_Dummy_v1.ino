/*
  Paradise ESP32 Dummy Sender — v2.4
  Sends static dummy data matching the agreed schema to AWS DynamoDB every 60 seconds.
  Used by dashboard developer for testing while real site ESP is being repaired.

  NEW IN v2.3:
  - WiFi watchdog: checks connection every 10s, auto-reconnects if dropped
  - NTP retry: retries every 30s until time is synced
  - Remote OTA: checks GitHub every hour for new firmware, auto-downloads and flashes
  - Dual-site: Site 1 uploads at T+10s, Site 2 uploads at T+40s, then every 60s each (30s stagger)
  - Improved LED status patterns (see below)

  LED STATUS (built-in LED GPIO 2 — no extra wiring needed):
  - Fast blink 150ms (startup):   Connecting to WiFi
  - Short flash every 1s:         All good — WiFi connected, uploads OK
  - Double blink every 2s:        WiFi disconnected
  - Slow pulse every 2s:          WiFi OK but last upload failed
  - 3 quick blinks (event):       Upload just succeeded
  - 5 rapid blinks (event):       Upload just failed
  - Rapid blink 50ms:             OTA update flashing in progress

  OTA WORKFLOW:
  1. Edit code, bump FW_VERSION string below to a new value
  2. In VS Code: Ctrl+Alt+B to build
  3. Copy .pio/build/esp32dev/firmware.bin to firmware/paradise-dummy.bin in repo
  4. Update firmware/version.txt to match new FW_VERSION
  5. git add . && git commit && git push
  6. ESP detects new version within 1 hour and self-updates

  SETUP BEFORE FLASHING:
  1. Change WIFI_SSID and WIFI_PASSWORD below to your network
  2. Board: ESP32 Dev Module
  3. Baud rate: 115200

  Libraries required (install via Arduino Library Manager):
  - ArduinoJson (v6)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>

// =====================================================
// ================== CONFIG ===========================
// =====================================================

// WiFi — CHANGE THESE to your network
const char* WIFI_SSID     = "marinewifi";
const char* WIFI_PASSWORD = "roxybaby";
//const char* WIFI_SSID     = "Kishen";
//const char* WIFI_PASSWORD = "12345678";

// Client / site identity
const char* CLIENT_NAME  = "Paradise Resort";
const char* CLIENT_EMAIL = "example@paradise.com";
const char* CLIENT_ID    = "001";
const char* SITE_ID      = "1";
const char* GATEWAY_ID   = "ESP32-DUMMY-001";
const char* FW_VERSION   = "paradise-esp32-dummy-2.4";

// Site 2 identity (same client, different site + gateway)
const char* SITE_ID_S2    = "2";
const char* GATEWAY_ID_S2 = "ESP32-DUMMY-002";
const char* FW_VERSION_S2 = "paradise-esp32-dummy-site2-1.0";

// AWS
const char* SERVER_URL = "https://0nriesk3fl.execute-api.ap-southeast-2.amazonaws.com/dev-esp32/solarv2handler";
const char* API_KEY    = "CcTVhmGC5FJStLooyNgH2fuHecM892Z6cpinehC2";

// OTA — GitHub raw URLs (update branch to main after merging)
const char* OTA_VERSION_URL = "https://raw.githubusercontent.com/kishenkumar92/kk92/claude/refactor-lambda-sorting-UIpKm/firmware/version.txt";
const char* OTA_BIN_URL     = "https://raw.githubusercontent.com/kishenkumar92/kk92/claude/refactor-lambda-sorting-UIpKm/firmware/paradise-dummy.bin";

// Intervals
const uint32_t UPLOAD_INTERVAL_MS   = 60000;    // 60 seconds
const uint32_t WIFI_CHECK_MS        = 10000;    // 10 seconds
const uint32_t NTP_RETRY_MS         = 30000;    // 30 seconds
const uint32_t OTA_CHECK_MS         = 3600000;  // 1 hour

// =====================================================
// ================== LED PIN ==========================
// =====================================================
#define LED_BUILTIN_PIN 2   // WROOM-32 built-in blue LED

// =====================================================
// ================== GLOBALS ==========================
// =====================================================
uint32_t lastUpload       = 0;
uint32_t lastUploadS2     = 0;
uint32_t lastWifiCheck    = 0;
uint32_t lastNTPRetry     = 0;
uint32_t lastOTACheck     = 0;
bool     ntpSynced        = false;
bool     wifiWasConnected = false;
bool     lastUploadOK     = true;   // tracks last upload result for LED status

// =====================================================
// ================== LED HELPERS ======================
// =====================================================
void ledBlink(uint8_t times, uint32_t onMs, uint32_t offMs) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_BUILTIN_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_BUILTIN_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

void showUploadOK()   { ledBlink(3, 100, 100); }
void showUploadFail() { ledBlink(5,  80,  80); }

// Background LED pattern — call every loop() iteration
// Uses millis() modulo so patterns stay perfectly timed with no extra globals.
void updateStatusLED() {
  uint32_t t = millis();
  bool ledOn = false;

  if (WiFi.status() != WL_CONNECTED) {
    // Double blink every 2s: flash at 0ms and 250ms
    uint32_t pos = t % 2000;
    ledOn = (pos < 100) || (pos >= 250 && pos < 350);
  } else if (!lastUploadOK) {
    // Slow pulse every 2s: 200ms ON then long OFF
    ledOn = (t % 2000) < 200;
  } else {
    // All good: short heartbeat flash every 1s
    ledOn = (t % 1000) < 50;
  }

  digitalWrite(LED_BUILTIN_PIN, ledOn ? HIGH : LOW);
}

// =====================================================
// ================== WIFI =============================
// =====================================================
bool ensureWifi(uint32_t maxWaitMs = 12000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < maxWaitMs) {
    digitalWrite(LED_BUILTIN_PIN, HIGH); delay(150);
    digitalWrite(LED_BUILTIN_PIN, LOW);  delay(150);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// =====================================================
// ================== NTP ==============================
// =====================================================
bool syncNTP() {
  configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing");

  uint32_t t0 = millis();
  struct tm ti;
  while (!getLocalTime(&ti) || ti.tm_year < (2020 - 1900)) {
    if (millis() - t0 > 10000) {
      Serial.println(" FAILED");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  ntpSynced = true;
  Serial.printf(" OK (%04d-%02d-%02d %02d:%02d:%02d UTC)\n",
    ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
    ti.tm_hour, ti.tm_min, ti.tm_sec);
  return true;
}

// =====================================================
// ================== OTA ==============================
// =====================================================
void checkOTA() {
  if (!ensureWifi(8000)) {
    Serial.println("[OTA] No WiFi — skipping check");
    return;
  }

  Serial.println("[OTA] Checking for update...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Step 1: Fetch version.txt
  http.begin(client, OTA_VERSION_URL);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] Version check failed (HTTP %d)\n", code);
    http.end();
    return;
  }

  String latestVersion = http.getString();
  latestVersion.trim();
  http.end();

  Serial.printf("[OTA] Current: %s | Latest: %s\n", FW_VERSION, latestVersion.c_str());

  if (latestVersion == FW_VERSION) {
    Serial.println("[OTA] Already up to date");
    return;
  }

  // Step 2: Download and flash new firmware
  Serial.println("[OTA] New version found — downloading firmware...");

  http.begin(client, OTA_BIN_URL);
  http.setTimeout(60000); // 60s timeout for binary download
  code = http.GET();

  if (code != 200) {
    Serial.printf("[OTA] Download failed (HTTP %d)\n", code);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

  if (contentLength <= 0) {
    Serial.println("[OTA] Invalid content length — aborting");
    http.end();
    return;
  }

  if (!Update.begin(contentLength)) {
    Serial.printf("[OTA] Not enough space: %s\n", Update.errorString());
    http.end();
    return;
  }

  // Stream binary into flash
  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != (size_t)contentLength) {
    Serial.printf("[OTA] Write mismatch: %d / %d bytes\n", written, contentLength);
    http.end();
    return;
  }

  if (!Update.end()) {
    Serial.printf("[OTA] Flash failed: %s\n", Update.errorString());
    http.end();
    return;
  }

  http.end();
  Serial.println("[OTA] Update complete — rebooting in 2s...");
  ledBlink(10, 50, 50); // rapid blinks before reboot
  delay(2000);
  ESP.restart();
}

// =====================================================
// ================== AWS POST =========================
// =====================================================
bool postToAWS(const String &payload) {
  if (!ensureWifi()) {
    Serial.println("WiFi not connected — skipping upload");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", API_KEY);

  int code = http.POST(payload);
  Serial.print("HTTP Response: ");
  Serial.println(code);

  String resp = http.getString();
  if (resp.length()) {
    Serial.print("Server: ");
    Serial.println(resp);
  }

  http.end();

  bool ok = (code >= 200 && code < 300);
  if (ok) showUploadOK();
  else    showUploadFail();

  return ok;
}

// =====================================================
// ================== BUILD PAYLOAD ====================
// =====================================================
String buildPayload() {
  DynamicJsonDocument doc(8192);

  // ---- Client ----
  JsonObject client = doc.createNestedObject("client");
  client["client_name"]  = CLIENT_NAME;
  client["client_email"] = CLIENT_EMAIL;
  client["client_id"]    = CLIENT_ID;
  client["site_id"]      = SITE_ID;

  // ---- Gateway ----
  JsonObject gateway = doc.createNestedObject("gateway");
  gateway["gateway_id"] = GATEWAY_ID;
  gateway["firmware"]   = FW_VERSION;

  // ---- Timestamp — real UTC time in milliseconds ----
  JsonObject ts = doc.createNestedObject("timestamp");
  time_t now = time(nullptr);
  ts["ts_epoch_ms"] = (uint64_t)now * 1000ULL;

  // ---- Meters ----
  // 11, 14, 15 = single tariff  |  12, 13 = dual tariff
  // Each meter includes per-phase voltage (v1/v2/v3), current (i1/i2/i3), freq_hz
  JsonArray meters = doc.createNestedArray("meters");

  JsonObject m11 = meters.createNestedObject();
  m11["slave_id"]  = 11;
  m11["v1"] = 230.50; m11["v2"] = 231.20; m11["v3"] = 229.80;
  m11["i1"] = 6.123;  m11["i2"] = 5.987;  m11["i3"] = 6.234;
  m11["freq_hz"]   = 50.00;
  m11["p_total_w"] = 4200.0;
  m11["kwh_total"] = 1800.0;

  JsonObject m12 = meters.createNestedObject();
  m12["slave_id"]  = 12;
  m12["v1"] = 230.50; m12["v2"] = 231.20; m12["v3"] = 229.80;
  m12["i1"] = 24.150; m12["i2"] = 23.870; m12["i3"] = 24.530;
  m12["freq_hz"]   = 50.00;
  m12["p_total_w"] = 15000.0;
  m12["kwh_total"] = 9500.0;
  JsonObject t12 = m12.createNestedObject("tariff");
  t12["kwh_t1"] = 8000.0;
  t12["kwh_t2"] = 1500.0;

  JsonObject m13 = meters.createNestedObject();
  m13["slave_id"]  = 13;
  m13["v1"] = 230.50; m13["v2"] = 231.20; m13["v3"] = 229.80;
  m13["i1"] = 18.500; m13["i2"] = 18.200; m13["i3"] = 18.800;
  m13["freq_hz"]   = 50.00;
  m13["p_total_w"] = 12000.0;
  m13["kwh_total"] = 7200.0;
  JsonObject t13 = m13.createNestedObject("tariff");
  t13["kwh_t1"] = 6000.0;
  t13["kwh_t2"] = 1200.0;

  JsonObject m14 = meters.createNestedObject();
  m14["slave_id"]  = 14;
  m14["v1"] = 230.50; m14["v2"] = 231.20; m14["v3"] = 229.80;
  m14["i1"] = 5.500;  m14["i2"] = 5.300;  m14["i3"] = 5.700;
  m14["freq_hz"]   = 50.00;
  m14["p_total_w"] = 3800.0;
  m14["kwh_total"] = 2100.0;

  JsonObject m15 = meters.createNestedObject();
  m15["slave_id"]  = 15;
  m15["v1"] = 230.50; m15["v2"] = 231.20; m15["v3"] = 229.80;
  m15["i1"] = 3.600;  m15["i2"] = 3.500;  m15["i3"] = 3.700;
  m15["freq_hz"]   = 50.00;
  m15["p_total_w"] = 2500.0;
  m15["kwh_total"] = 1400.0;

  // ---- Inverters ----
  JsonArray inverters = doc.createNestedArray("inverters");

  JsonObject inv1 = inverters.createNestedObject();
  inv1["slave_id"]          = 1;
  inv1["status"]            = "running";
  inv1["p_total_kw"]        = 38.0;
  JsonArray mppt1 = inv1.createNestedArray("mppt_kw");
  mppt1.add(9.8); mppt1.add(9.5); mppt1.add(9.2); mppt1.add(9.5);
  inv1["battery_soc"]       = 72;
  inv1["battery_current_a"] = 15.5;
  inv1["battery_status"]    = "charging";

  JsonObject inv2 = inverters.createNestedObject();
  inv2["slave_id"]          = 2;
  inv2["status"]            = "running";
  inv2["p_total_kw"]        = 36.0;
  JsonArray mppt2 = inv2.createNestedArray("mppt_kw");
  mppt2.add(9.2); mppt2.add(9.0); mppt2.add(8.8); mppt2.add(9.0);
  inv2["battery_soc"]       = 68;
  inv2["battery_current_a"] = 12.3;
  inv2["battery_status"]    = "charging";

  JsonObject inv3 = inverters.createNestedObject();
  inv3["slave_id"]          = 3;
  inv3["status"]            = "running";
  inv3["p_total_kw"]        = 35.0;
  JsonArray mppt3 = inv3.createNestedArray("mppt_kw");
  mppt3.add(8.8); mppt3.add(8.8); mppt3.add(8.7); mppt3.add(8.7);
  inv3["battery_soc"]       = 65;
  inv3["battery_current_a"] = -5.2;
  inv3["battery_status"]    = "discharging";

  // ---- AC Units ----
  JsonArray ac_units = doc.createNestedArray("ac_units");
  JsonObject ac1 = ac_units.createNestedObject();
  ac1["slave_id"] = 21; ac1["status"] = "ok"; ac1["temperature_c"] = 20;
  JsonObject ac2 = ac_units.createNestedObject();
  ac2["slave_id"] = 22; ac2["status"] = "ok"; ac2["temperature_c"] = 21;

  // ---- Irradiance Meters ----
  JsonArray irradiance = doc.createNestedArray("irradiance_meters");
  JsonObject ir1 = irradiance.createNestedObject();
  ir1["slave_id"] = 30; ir1["irradiance_w_per_m2"] = 700;

  // ---- Generator ----
  JsonObject generator = doc.createNestedObject("generator");
  generator["status"]   = "off";
  generator["relay_on"] = false;

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================
// =========== BUILD PAYLOAD — SITE 2 =================
// =====================================================
String buildPayloadSite2() {
  DynamicJsonDocument doc(4096);

  // ---- Client (same client, different site) ----
  JsonObject client = doc.createNestedObject("client");
  client["client_name"]  = CLIENT_NAME;
  client["client_email"] = CLIENT_EMAIL;
  client["client_id"]    = CLIENT_ID;
  client["site_id"]      = SITE_ID_S2;

  // ---- Gateway ----
  JsonObject gateway = doc.createNestedObject("gateway");
  gateway["gateway_id"] = GATEWAY_ID_S2;
  gateway["firmware"]   = FW_VERSION_S2;

  // ---- Timestamp ----
  JsonObject ts = doc.createNestedObject("timestamp");
  time_t now = time(nullptr);
  ts["ts_epoch_ms"] = (uint64_t)now * 1000ULL;

  // ---- Meters (3 of 5: slave 11, 12, 13) ----
  JsonArray meters = doc.createNestedArray("meters");

  JsonObject m11 = meters.createNestedObject();
  m11["slave_id"]  = 11;
  m11["v1"] = 228.40; m11["v2"] = 229.10; m11["v3"] = 227.90;
  m11["i1"] = 3.210;  m11["i2"] = 3.105;  m11["i3"] = 3.320;
  m11["freq_hz"]   = 50.00;
  m11["p_total_w"] = 2100.0;
  m11["kwh_total"] = 920.0;

  JsonObject m12 = meters.createNestedObject();
  m12["slave_id"]  = 12;
  m12["v1"] = 228.40; m12["v2"] = 229.10; m12["v3"] = 227.90;
  m12["i1"] = 12.450; m12["i2"] = 12.110; m12["i3"] = 12.780;
  m12["freq_hz"]   = 50.00;
  m12["p_total_w"] = 7500.0;
  m12["kwh_total"] = 4800.0;
  JsonObject t12 = m12.createNestedObject("tariff");
  t12["kwh_t1"] = 4100.0;
  t12["kwh_t2"] = 700.0;

  JsonObject m13 = meters.createNestedObject();
  m13["slave_id"]  = 13;
  m13["v1"] = 228.40; m13["v2"] = 229.10; m13["v3"] = 227.90;
  m13["i1"] = 9.200;  m13["i2"] = 8.950;  m13["i3"] = 9.410;
  m13["freq_hz"]   = 50.00;
  m13["p_total_w"] = 6000.0;
  m13["kwh_total"] = 3600.0;

  // ---- Inverters (1 of 3: slave 1) ----
  JsonArray inverters = doc.createNestedArray("inverters");

  JsonObject inv1 = inverters.createNestedObject();
  inv1["slave_id"]          = 1;
  inv1["status"]            = "running";
  inv1["p_total_kw"]        = 18.5;
  JsonArray mppt1 = inv1.createNestedArray("mppt_kw");
  mppt1.add(4.8); mppt1.add(4.6); mppt1.add(4.7); mppt1.add(4.4);
  inv1["battery_soc"]       = 58;
  inv1["battery_current_a"] = 8.2;
  inv1["battery_status"]    = "charging";

  // ---- AC Units (1 of 2: slave 21) ----
  JsonArray ac_units = doc.createNestedArray("ac_units");
  JsonObject ac1 = ac_units.createNestedObject();
  ac1["slave_id"] = 21; ac1["status"] = "ok"; ac1["temperature_c"] = 22;

  // ---- Irradiance Meters ----
  JsonArray irradiance = doc.createNestedArray("irradiance_meters");
  JsonObject ir1 = irradiance.createNestedObject();
  ir1["slave_id"] = 30; ir1["irradiance_w_per_m2"] = 650;

  // ---- Generator ----
  JsonObject generator = doc.createNestedObject("generator");
  generator["status"]   = "off";
  generator["relay_on"] = false;

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================
// ================== SETUP/LOOP =======================
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, LOW);

  Serial.println("=== Paradise ESP32 Dummy Sender v2.4 ===");
  Serial.println("-----------------------------------------");

  Serial.printf("[WiFi] Connecting to: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(8, 8, 8, 8));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    digitalWrite(LED_BUILTIN_PIN, HIGH); delay(150);
    digitalWrite(LED_BUILTIN_PIN, LOW);  delay(150);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    syncNTP();
  } else {
    Serial.println("[WiFi] FAILED — will retry automatically");
  }

  // Site 1: first upload ~10s after boot
  lastUpload   = millis() - UPLOAD_INTERVAL_MS + 10000;
  // Site 2: first upload ~40s after boot (30s after Site 1)
  lastUploadS2 = millis() - UPLOAD_INTERVAL_MS + 40000;

  // First OTA check ~30 seconds after boot
  lastOTACheck = millis() - OTA_CHECK_MS + 30000;

  Serial.println("Setup complete.");
  Serial.println("-----------------------------------------");
}

void loop() {
  uint32_t now = millis();

  // --- Background LED status ---
  updateStatusLED();

  // --- WiFi Watchdog (every 10s) ---
  if (now - lastWifiCheck >= WIFI_CHECK_MS) {
    lastWifiCheck = now;
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (!connected && wifiWasConnected) {
      Serial.println("[WiFi] Connection lost — reconnecting...");
      wifiWasConnected = false;
    }

    if (!connected) {
      if (ensureWifi(8000)) {
        wifiWasConnected = true;
        Serial.printf("[WiFi] Reconnected — IP: %s\n", WiFi.localIP().toString().c_str());
        if (!ntpSynced) syncNTP();
      }
    } else {
      wifiWasConnected = true;
    }
  }

  // --- NTP Retry (every 30s until synced) ---
  if (!ntpSynced && WiFi.status() == WL_CONNECTED && now - lastNTPRetry >= NTP_RETRY_MS) {
    lastNTPRetry = now;
    syncNTP();
  }

  // --- OTA Check (every 1 hour) ---
  if (now - lastOTACheck >= OTA_CHECK_MS) {
    lastOTACheck = now;
    checkOTA();
  }

  // --- Site 1 Upload (every 60s) ---
  if (now - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = now;
    Serial.println("------------------------------------------------------");
    Serial.println("[Site 1] Building payload...");
    String payload = buildPayload();
    Serial.println("[Site 1] Payload:");
    Serial.println(payload);
    Serial.println("[Site 1] Posting to AWS...");
    bool ok = postToAWS(payload);
    lastUploadOK = ok;
    Serial.println(ok ? "[Site 1] Upload OK" : "[Site 1] Upload FAILED");
  }

  // --- Site 2 Upload (every 60s, staggered 30s after Site 1) ---
  if (now - lastUploadS2 >= UPLOAD_INTERVAL_MS) {
    lastUploadS2 = now;
    Serial.println("------------------------------------------------------");
    Serial.println("[Site 2] Building payload...");
    String payload2 = buildPayloadSite2();
    Serial.println("[Site 2] Payload:");
    Serial.println(payload2);
    Serial.println("[Site 2] Posting to AWS...");
    bool ok2 = postToAWS(payload2);
    if (!ok2) lastUploadOK = false;  // either site failing sets the warning LED
    Serial.println(ok2 ? "[Site 2] Upload OK" : "[Site 2] Upload FAILED");
  }

  delay(20);
}
