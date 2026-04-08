/*
  Paradise ESP32 Dummy Sender — v2.0
  Sends static dummy data matching agreed schema to AWS DynamoDB every 60 seconds.
  Used by dashboard developer for testing while real site ESP is being repaired.

  Schema v2 changes:
  - Nested structure: client / gateway / timestamp / meters / inverters / ac_units / irradiance_meters / generator
  - Meters now include per-phase voltage (v1/v2/v3), current (i1/i2/i3), freq_hz
  - Inverters include battery_current_a and battery_status (charging/discharging)
  - Generator status block added

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
#include <time.h>

// =====================================================
// ================== CONFIG ===========================
// =====================================================

// WiFi — CHANGE THESE to your network
const char* WIFI_SSID     = "MUGETSU_EXT";
const char* WIFI_PASSWORD = "d8stracts";

// Client / site identity
const char* CLIENT_NAME  = "Paradise Resort";
const char* CLIENT_EMAIL = "example@paradise.com";
const char* CLIENT_ID    = "001";
const char* SITE_ID      = "1";
const char* GATEWAY_ID   = "ESP32-DUMMY-001";
const char* FW_VERSION   = "paradise-esp32-dummy-2.0";

// AWS
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

  // Meter 11 — single tariff
  JsonObject m11 = meters.createNestedObject();
  m11["slave_id"]  = 11;
  m11["v1"] = 230.50; m11["v2"] = 231.20; m11["v3"] = 229.80;
  m11["i1"] = 6.123;  m11["i2"] = 5.987;  m11["i3"] = 6.234;
  m11["freq_hz"]   = 50.00;
  m11["p_total_w"] = 4200.0;
  m11["kwh_total"] = 1800.0;

  // Meter 12 — dual tariff
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

  // Meter 13 — dual tariff
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

  // Meter 14 — single tariff
  JsonObject m14 = meters.createNestedObject();
  m14["slave_id"]  = 14;
  m14["v1"] = 230.50; m14["v2"] = 231.20; m14["v3"] = 229.80;
  m14["i1"] = 5.500;  m14["i2"] = 5.300;  m14["i3"] = 5.700;
  m14["freq_hz"]   = 50.00;
  m14["p_total_w"] = 3800.0;
  m14["kwh_total"] = 2100.0;

  // Meter 15 — single tariff
  JsonObject m15 = meters.createNestedObject();
  m15["slave_id"]  = 15;
  m15["v1"] = 230.50; m15["v2"] = 231.20; m15["v3"] = 229.80;
  m15["i1"] = 3.600;  m15["i2"] = 3.500;  m15["i3"] = 3.700;
  m15["freq_hz"]   = 50.00;
  m15["p_total_w"] = 2500.0;
  m15["kwh_total"] = 1400.0;

  // ---- Inverters ----
  // 3 x 50kW Deye inverters, 4 MPPT strings each
  // battery_current_a: positive = discharging, negative = charging
  JsonArray inverters = doc.createNestedArray("inverters");

  JsonObject inv1 = inverters.createNestedObject();
  inv1["slave_id"]         = 1;
  inv1["status"]           = "running";
  inv1["p_total_kw"]       = 38.0;
  JsonArray mppt1 = inv1.createNestedArray("mppt_kw");
  mppt1.add(9.8); mppt1.add(9.5); mppt1.add(9.2); mppt1.add(9.5);
  inv1["battery_soc"]      = 72;
  inv1["battery_current_a"] = 15.5;
  inv1["battery_status"]   = "charging";

  JsonObject inv2 = inverters.createNestedObject();
  inv2["slave_id"]         = 2;
  inv2["status"]           = "running";
  inv2["p_total_kw"]       = 36.0;
  JsonArray mppt2 = inv2.createNestedArray("mppt_kw");
  mppt2.add(9.2); mppt2.add(9.0); mppt2.add(8.8); mppt2.add(9.0);
  inv2["battery_soc"]      = 68;
  inv2["battery_current_a"] = 12.3;
  inv2["battery_status"]   = "charging";

  JsonObject inv3 = inverters.createNestedObject();
  inv3["slave_id"]         = 3;
  inv3["status"]           = "running";
  inv3["p_total_kw"]       = 35.0;
  JsonArray mppt3 = inv3.createNestedArray("mppt_kw");
  mppt3.add(8.8); mppt3.add(8.8); mppt3.add(8.7); mppt3.add(8.7);
  inv3["battery_soc"]      = 65;
  inv3["battery_current_a"] = -5.2;
  inv3["battery_status"]   = "discharging";

  // ---- AC Units ----
  JsonArray ac_units = doc.createNestedArray("ac_units");

  JsonObject ac1 = ac_units.createNestedObject();
  ac1["slave_id"]      = 21;
  ac1["status"]        = "ok";
  ac1["temperature_c"] = 20;

  JsonObject ac2 = ac_units.createNestedObject();
  ac2["slave_id"]      = 22;
  ac2["status"]        = "ok";
  ac2["temperature_c"] = 21;

  // ---- Irradiance Meters ----
  JsonArray irradiance = doc.createNestedArray("irradiance_meters");

  JsonObject ir1 = irradiance.createNestedObject();
  ir1["slave_id"]             = 30;
  ir1["irradiance_w_per_m2"]  = 700;

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

  Serial.println("=== Paradise ESP32 Dummy Sender v2.0 ===");
  Serial.println("-----------------------------------------");

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
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

  // First upload ~10 seconds after boot
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
