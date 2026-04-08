/*
  Paradise ESP32 Dummy Sender — v1.0
  Sends static dummy data in new schema to AWS DynamoDB every 60 seconds.
  Used by dashboard developer for testing while real site is being set up.

  SETUP BEFORE FLASHING:
  1. Change WIFI_SSID and WIFI_PASSWORD below to your network
  2. Board: ESP32 Dev Module (same settings as existing ESP)
  3. Baud rate: 115200

  Libraries required (install via Arduino Library Manager):
  - ArduinoJson (v6)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================
// ================== CONFIG ===========================
// =====================================================

// WiFi — CHANGE THESE to your home/office network
const char* WIFI_SSID     = "MUGETSU_EXT";
const char* WIFI_PASSWORD = "d8stracts";

// Client / site identity
const char* CLIENT_NAME  = "Paradise Resort";
const char* CLIENT_EMAIL = "example@paradise.com";
const char* CLIENT_ID    = "001";
const char* SITE_ID      = "1";
const char* GATEWAY_ID   = "ESP32-DUMMY-001";
const char* FW_VERSION   = "paradise-esp32-dummy-1.0";

// AWS — same endpoint as production system
const char* SERVER_URL = "https://0nriesk3fl.execute-api.ap-southeast-2.amazonaws.com/dev-esp32/solarv2handler";
const char* API_KEY    = "CcTVhmGC5FJStLooyNgH2fuHecM892Z6cpinehC2";

// Upload interval
const uint32_t UPLOAD_INTERVAL_MS = 60000; // 60 seconds

// =====================================================
// ================== GLOBALS ==========================
// =====================================================
uint32_t lastUpload = 0;

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
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// =====================================================
// ================== NTP ==============================
// =====================================================
bool syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing NTP");

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

  Serial.printf(" OK (%04d-%02d-%02d %02d:%02d:%02d UTC)\n",
    ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
    ti.tm_hour, ti.tm_min, ti.tm_sec);
  return true;
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
  return (code >= 200 && code < 300);
}

// =====================================================
// ================== BUILD PAYLOAD ====================
// =====================================================
String buildPayload() {
  DynamicJsonDocument doc(4096);

  // Identity
  doc["client_name"]  = CLIENT_NAME;
  doc["client_email"] = CLIENT_EMAIL;
  doc["client_ID"]    = CLIENT_ID;
  doc["site_id"]      = SITE_ID;
  doc["gateway_id"]   = GATEWAY_ID;
  doc["fw"]           = FW_VERSION;

  // Timestamp — real current time in milliseconds (UTC)
  time_t now = time(nullptr);
  doc["ts_epoch_ms"] = (uint64_t)now * 1000ULL;

  // ---- Meters ----
  // Meters 11, 14, 15: single tariff (kwh_total only)
  // Meters 12, 13:     dual tariff   (kwh_t1 + kwh_t2)
  JsonArray meters = doc.createNestedArray("meters");

  JsonObject m11 = meters.createNestedObject();
  m11["slave_id"]  = 11;
  m11["p_total_w"] = 4200.0;
  m11["kwh_total"] = 1800.0;

  JsonObject m12 = meters.createNestedObject();
  m12["slave_id"]  = 12;
  m12["p_total_w"] = 15000.0;
  m12["kwh_total"] = 9500.0;
  m12["kwh_t1"]    = 8000.0;
  m12["kwh_t2"]    = 1500.0;

  JsonObject m13 = meters.createNestedObject();
  m13["slave_id"]  = 13;
  m13["p_total_w"] = 12000.0;
  m13["kwh_total"] = 7200.0;
  m13["kwh_t1"]    = 6000.0;
  m13["kwh_t2"]    = 1200.0;

  JsonObject m14 = meters.createNestedObject();
  m14["slave_id"]  = 14;
  m14["p_total_w"] = 3800.0;
  m14["kwh_total"] = 2100.0;

  JsonObject m15 = meters.createNestedObject();
  m15["slave_id"]  = 15;
  m15["p_total_w"] = 2500.0;
  m15["kwh_total"] = 1400.0;

  // ---- Inverters ----
  // 3 x 50kW Deye inverters, 4 MPPT each
  // p_total_w = PV generation in watts; mppt_kw = per-string output in watts
  JsonArray inverters = doc.createNestedArray("inverters");

  JsonObject inv1 = inverters.createNestedObject();
  inv1["slave_id"]  = 1;
  inv1["status"]    = "running";
  inv1["p_total_w"] = 38000.0;
  JsonArray mppt1 = inv1.createNestedArray("mppt_kw");
  mppt1.add(9800.0); mppt1.add(9500.0); mppt1.add(9200.0); mppt1.add(9500.0);
  inv1["bat_soc"] = 72;

  JsonObject inv2 = inverters.createNestedObject();
  inv2["slave_id"]  = 2;
  inv2["status"]    = "running";
  inv2["p_total_w"] = 36000.0;
  JsonArray mppt2 = inv2.createNestedArray("mppt_kw");
  mppt2.add(9200.0); mppt2.add(9000.0); mppt2.add(8800.0); mppt2.add(9000.0);
  inv2["bat_soc"] = 68;

  JsonObject inv3 = inverters.createNestedObject();
  inv3["slave_id"]  = 3;
  inv3["status"]    = "running";
  inv3["p_total_w"] = 35000.0;
  JsonArray mppt3 = inv3.createNestedArray("mppt_kw");
  mppt3.add(8800.0); mppt3.add(8800.0); mppt3.add(8700.0); mppt3.add(8700.0);
  inv3["bat_soc"] = 65;

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

  Serial.println("=== Paradise ESP32 Dummy Sender v1.0 ===");
  Serial.println("-----------------------------------------");

  // Connect WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  // Force Google DNS — fixes DNS resolution on some routers
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(8, 8, 8, 8));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected — IP: ");
    Serial.println(WiFi.localIP());
    syncNTP();
  } else {
    Serial.println("WiFi FAILED — will retry before each upload");
  }

  // First upload ~10 seconds after boot (not immediately)
  lastUpload = millis() - UPLOAD_INTERVAL_MS + 10000;

  Serial.println("Setup complete. First upload in ~10 seconds.");
  Serial.println("-----------------------------------------");
}

void loop() {
  uint32_t now = millis();

  if (now - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = now;

    Serial.println("------------------------------------------------------");
    Serial.println("Building payload...");

    String payload = buildPayload();

    Serial.println("Payload:");
    Serial.println(payload);

    Serial.println("Posting to AWS...");
    bool ok = postToAWS(payload);
    Serial.println(ok ? "Upload OK" : "Upload FAILED");
  }

  delay(20);
}
