/*
 * Smart Load Monitor — ESP32 Web Server
 * Paired with Arduino Uno (Smart Load Monitor sketch)
 *
 * Hardware:
 *   - ESP32 RX2 (GPIO16) ← Uno TX (pin 11 / SoftwareSerial)
 *   - Relay control: GPIO26 (mirrors Uno relay, or independent override)
 *   - Both share common GND; logic-level shift recommended (Uno=5V, ESP32=3.3V)
 *
 * Telemetry CSV from Uno (one line per second):
 *   voltage,current,apparentVA,realW,reactiveQ,distortion,loadType,device,
 *   totalKwh,sessionCostRs,mobileKwh,laptopKwh,unknownKwh
 *
 * Web UI: http://<ESP32-IP>/
 *   - Live gauges & table (auto-refresh via SSE)
 *   - Relay ON/OFF buttons
 *   - Device energy breakdown chart
 *   - Session stats & CESC cost estimate
 */

#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <vector>

// ─── Google Sheets integration for live data & backup ──────────────────────
// Replace with your Google Apps Script deployment URL
const char *GOOGLE_SCRIPT_URL =
    "https://script.google.com/macros/s/"
    "AKfycbxCdne0QgrguqF-EADH2WtX5meJoJ2VwvzsCknYEPsCWKLU4QVVgSrJ-uIY_rHHoY0/"
    "exec";

// ─── WiFi credentials ────────────────────────────────────────────────────────
static const char *WIFI_SSID = "Wlan";
static const char *WIFI_PASS = "9007671509";

// ─── Serial from Uno ─────────────────────────────────────────────────────────
#define UNO_SERIAL Serial2 // UART2: RX=GPIO16, TX=GPIO17
#define UNO_BAUD 115200

// ─── Relay pin (ESP32 side — optional independent control) ───────────────────
static const int RELAY_PIN = 26; // Active LOW (matches Uno convention)
static bool relayState = false;  // false = OFF (relay inactive = HIGH)

// ─── 16x2 LCD Display (I2C: SDA=GPIO21, SCL=GPIO22) ──────────────────────────
static LiquidCrystal_I2C lcd(0x27, 16, 2);
static unsigned long lastLcdUpdateMs = 0;
static const unsigned long LCD_UPDATE_INTERVAL_MS = 500;
static unsigned long lcdIpDisplayStartMs = 0;
static const unsigned long LCD_IP_DISPLAY_DURATION_MS = 10000;
static bool showingIpAddress = false;
static bool calibrationInProgress = false;
static unsigned long calibrationStartMs = 0;

// ─── Web server ──────────────────────────────────────────────────────────────
WebServer server(80);

// ─── Live telemetry struct ───────────────────────────────────────────────────
struct Telemetry {
  float voltage = 0;
  float current = 0;
  float apparentVA = 0;
  float realW = 0;
  float reactiveQ = 0;
  float distortion = 0;
  char loadType[16] = "—";
  char device[32] = "—";
  float totalKwh = 0;
  float sessionCostRs = 0;
  float mobileKwh = 0;
  float laptopKwh = 0;
  float unknownKwh = 0;
  char deviceClass[32] = "Unknown";
  float efficiency = 0;
  char unoRelayState[8] = "OFF";
  bool analysisActive = false;
  float analysisMinVA = 0;
  float analysisMaxVA = 0;
  char dynamicDevice[32] = "-";
  unsigned long analysisStartMs = 0;
  unsigned long lastUpdatedMs = 0;
  bool fresh = false;
};

static Telemetry tele;
static char rawLine[256];

// ─── Billing / prepaid state ─────────────────────────────────────────────────
struct BillingState {
  bool isActive = false;
  bool adminLock = false;
  bool timerRunning = false;

  unsigned long remainingSeconds = 0;
  unsigned long lastTick = 0;
  unsigned long sessionStartMs = 0;
  float sessionStartKwh = 0;

  bool oneDayAlertSent = false;

  float costPerHour = 5.0;
  bool relayAllowed = false;
};

struct BillingRecord {
  unsigned long duration = 0; // seconds
  float energyUsed = 0;       // kWh
  float cost = 0;             // INR
};

static BillingState billing;
static std::vector<BillingRecord> billingRecords;

const char *ADMIN_USER = "admin";
const char *ADMIN_PASS = "1234";
static bool isAdminSession = false;

const int EEPROM_SIZE = 64;

struct EEPROMData {
  unsigned long remainingSeconds;
  bool timerRunning;
  bool relayAllowed;
  bool adminLock;
  float costPerHour;
};

static EEPROMData eepromData;

void saveBillingToEEPROM() {
  eepromData.remainingSeconds = billing.remainingSeconds;
  eepromData.timerRunning = billing.timerRunning;
  eepromData.relayAllowed = billing.relayAllowed;
  eepromData.adminLock = billing.adminLock;
  eepromData.costPerHour = billing.costPerHour;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
}

void loadBillingFromEEPROM() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("EEPROM init failed");
    return;
  }
  EEPROM.get(0, eepromData);
  billing.remainingSeconds = eepromData.remainingSeconds;
  billing.timerRunning = eepromData.timerRunning;
  billing.relayAllowed = eepromData.relayAllowed;
  billing.adminLock = eepromData.adminLock;
  billing.costPerHour = eepromData.costPerHour;
  if (billing.remainingSeconds > 31536000UL) {
    billing.remainingSeconds = 0;
    billing.timerRunning = false;
    billing.relayAllowed = false;
    billing.isActive = false;
    billing.oneDayAlertSent = false;
  }
  if (billing.timerRunning) {
    billing.lastTick = millis();
    billing.sessionStartMs = millis();
  }
}

void recordBillingSession(unsigned long usedSeconds) {
  BillingRecord rec;
  rec.duration = usedSeconds;
  if (billing.sessionStartKwh > 0) {
    rec.energyUsed = tele.totalKwh - billing.sessionStartKwh;
    if (rec.energyUsed < 0)
      rec.energyUsed = 0;
  } else {
    rec.energyUsed = 0;
  }
  rec.cost = (billing.costPerHour / 3600.0f) * usedSeconds;
  billingRecords.push_back(rec);
}

// ─── Google Sheets backup functions ──────────────────────────────────────────
void uploadToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/json");

  // Create JSON payload with live data
  String json = "{";
  json += "\"timestamp\":\"" + String(millis()) + "\",";
  json += "\"voltage\":" + String(tele.voltage, 1) + ",";
  json += "\"current\":" + String(tele.current, 3) + ",";
  json += "\"power\":" + String(tele.realW, 1) + ",";
  json += "\"device\":\"" + String(tele.device) + "\",";
  json += "\"remaining_sec\":" + String(billing.remainingSeconds) + ",";
  json += "\"total_kwh\":" + String(tele.totalKwh, 3) + ",";
  json += "\"session_cost\":" + String(tele.sessionCostRs, 2) + ",";
  json += "\"relay_state\":\"" + String(relayState ? "ON" : "OFF") + "\"";
  json += "}";

  int httpResponseCode = http.POST(json);
  if (httpResponseCode > 0) {
    Serial.printf("[GOOGLE] Upload success: %d\n", httpResponseCode);
  } else {
    Serial.printf("[GOOGLE] Upload failed: %d\n", httpResponseCode);
  }
  http.end();
}

void fetchBackupFromGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  const int maxRetries = 3;
  for (int attempt = 1; attempt <= maxRetries; ++attempt) {
    Serial.printf("[GOOGLE] Fetch attempt %d/%d\n", attempt, maxRetries);

    HTTPClient http;
    String url = String(GOOGLE_SCRIPT_URL) + "?action=get";
    http.begin(url);
    http.setTimeout(10000); // 10 second timeout

    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
      String payload = http.getString();
      Serial.println("[GOOGLE] Fetched: " + payload);

      // Parse JSON and restore key values
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        if (doc.containsKey("remaining_sec")) {
          billing.remainingSeconds = doc["remaining_sec"];
          Serial.printf("[GOOGLE] Restored remaining_sec: %lu\n",
                        billing.remainingSeconds);
        }
        if (doc.containsKey("total_kwh")) {
          tele.totalKwh = doc["total_kwh"];
          Serial.printf("[GOOGLE] Restored total_kwh: %.3f\n", tele.totalKwh);
        }
        // Add more fields as needed
        http.end();
        return; // Success, exit
      } else {
        Serial.printf("[GOOGLE] JSON parse error: %s\n", error.c_str());
      }
    } else {
      Serial.printf("[GOOGLE] Fetch failed (attempt %d): %d\n", attempt,
                    httpResponseCode);
    }
    http.end();

    // Wait before retry (exponential backoff)
    if (attempt < maxRetries) {
      unsigned long delayMs = attempt * 2000; // 2s, 4s
      Serial.printf("[GOOGLE] Retrying in %lu ms...\n", delayMs);
      delay(delayMs);
    }
  }
  Serial.println(
      "[GOOGLE] All fetch attempts failed - using local EEPROM data");
}

void fetchBackupTask(void *parameter) {
  vTaskDelay(5000 / portTICK_PERIOD_MS); // Give system a short startup window
  fetchBackupFromGoogleSheets();
  vTaskDelete(NULL);
}

// Local dynamic load list
static const int kMaxDynamicLoads = 16;
struct DynamicLoadEntry {
  bool active = false;
  String name;
  float minW = 0;
  float maxW = 0;
};
static DynamicLoadEntry dynamicLoads[kMaxDynamicLoads];

void saveDynamicLoads() {
  Preferences prefs;
  prefs.begin("dynamicLoads", false);
  for (int i = 0; i < kMaxDynamicLoads; ++i) {
    String key = "load" + String(i);
    if (dynamicLoads[i].active) {
      String data = dynamicLoads[i].name + "," + String(dynamicLoads[i].minW) +
                    "," + String(dynamicLoads[i].maxW);
      prefs.putString(key.c_str(), data);
    } else {
      prefs.putString(key.c_str(), "");
    }
  }
  prefs.end();
}

void loadDynamicLoads() {
  Preferences prefs;
  prefs.begin("dynamicLoads", true);
  for (int i = 0; i < kMaxDynamicLoads; ++i) {
    String key = "load" + String(i);
    String data = prefs.getString(key.c_str(), "");
    if (data.length() > 0) {
      int comma1 = data.indexOf(',');
      int comma2 = data.indexOf(',', comma1 + 1);
      if (comma1 > 0 && comma2 > comma1) {
        dynamicLoads[i].active = true;
        dynamicLoads[i].name = data.substring(0, comma1);
        dynamicLoads[i].minW = data.substring(comma1 + 1, comma2).toFloat();
        dynamicLoads[i].maxW = data.substring(comma2 + 1).toFloat();
      }
    }
  }
  prefs.end();
}

// ─── Parse CSV from Uno ──────────────────────────────────────────────────────
void parseUnoLine(const char *line) {
  if (!line || strlen(line) < 5)
    return;

  char buf[256];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *tok = strtok(buf, ",");
  if (!tok)
    return;
  tele.voltage = atof(tok);

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.current = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.apparentVA = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.realW = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.reactiveQ = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.distortion = atof(tok);

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  strncpy(tele.loadType, tok, sizeof(tele.loadType) - 1);
  tele.loadType[sizeof(tele.loadType) - 1] = '\0';

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  strncpy(tele.device, tok, sizeof(tele.device) - 1);
  tele.device[sizeof(tele.device) - 1] = '\0';

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.totalKwh = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.sessionCostRs = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.mobileKwh = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.laptopKwh = atof(tok);
  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.unknownKwh = atof(tok);

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  strncpy(tele.deviceClass, tok, sizeof(tele.deviceClass) - 1);
  tele.deviceClass[sizeof(tele.deviceClass) - 1] = '\0';

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  tele.efficiency = atof(tok);

  tok = strtok(NULL, ",");
  if (!tok)
    return;
  strncpy(tele.unoRelayState, tok, sizeof(tele.unoRelayState) - 1);
  tele.unoRelayState[sizeof(tele.unoRelayState) - 1] = '\0';

  // Skip Uno's placeholder analysis fields — ESP32 owns these exclusively
  strtok(NULL, ","); // analysisActive placeholder — discard
  strtok(NULL, ","); // analysisMinVA placeholder  — discard
  strtok(NULL, ","); // analysisMaxVA placeholder  — discard

  // Sync relay state from Uno
  String unoState(tele.unoRelayState);
  unoState.toUpperCase();
  // ESP32 is authoritative for actual relay control. Keep Uno state in
  // telemetry only. relayState stays under ESP32 control via setRelay().

  // ESP32-side sanity clamping to prevent spikes / invalid values from going to
  // UI
  if (tele.realW < 0.0f)
    tele.realW = 0.0f;
  if (tele.apparentVA < tele.realW)
    tele.apparentVA = tele.realW;
  if (tele.current < 0.0f)
    tele.current = 0.0f;
  if (tele.voltage < 0.0f)
    tele.voltage = 0.0f;

  tele.lastUpdatedMs = millis();
  tele.fresh = true;
  lastLcdUpdateMs = 0; // Force immediate LCD update on new data

  // Force reset device from MAINS_OFF if voltage is back
  if (tele.voltage >= 50.0f && strcmp(tele.device, "MAINS_OFF") == 0) {
    strcpy(tele.device, "NO_LOAD");
  }

  // Display identified load on ESP32 serial monitor for live debugging
  Serial.printf("[LOAD] Identified: %s | Device class: %s | Real W: %.1f W\n",
                tele.device, tele.deviceClass, tele.realW);

  // ← FIX: track min/max wattage on ESP32 side during analysis
  if (tele.analysisActive && tele.realW > 0.5f) {
    // Skip first 2 seconds — filters are still ramping up, values are transient
    if ((millis() - tele.analysisStartMs) > 2000UL) {
      if (tele.realW < tele.analysisMinVA)
        tele.analysisMinVA = tele.realW;
      if (tele.realW > tele.analysisMaxVA)
        tele.analysisMaxVA = tele.realW;
    }
  }

  // Dynamic load detection
  strncpy(tele.dynamicDevice, "-", sizeof(tele.dynamicDevice));
  if (tele.realW > 0.5f) {
    for (int i = 0; i < kMaxDynamicLoads; ++i) {
      if (!dynamicLoads[i].active)
        continue;
      float buffer = (dynamicLoads[i].maxW - dynamicLoads[i].minW) * 0.15f +
                     2.0f; // ±15% + 2W floor
      if (tele.realW >= (dynamicLoads[i].minW - buffer) &&
          tele.realW <= (dynamicLoads[i].maxW + buffer)) {
        strncpy(tele.dynamicDevice, dynamicLoads[i].name.c_str(),
                sizeof(tele.dynamicDevice) - 1);
        tele.dynamicDevice[sizeof(tele.dynamicDevice) - 1] = '\0';
        break;
      }
    }
  }
}

// ─── JSON API endpoint ───────────────────────────────────────────────────────
void handleApiData() {
  StaticJsonDocument<512> doc;
  doc["v"] = tele.voltage;
  doc["i"] = tele.current;
  doc["s"] = tele.apparentVA;
  doc["p"] = tele.realW;
  doc["q"] = tele.reactiveQ;
  doc["di"] = tele.distortion;
  doc["pf"] = (tele.apparentVA > 0.5f) ? tele.realW / tele.apparentVA : 0.0f;
  doc["lt"] = tele.loadType;
  doc["dev"] = tele.device;
  doc["dynamic"] = tele.dynamicDevice;
  JsonArray dyn = doc.createNestedArray("dynamicLoads");
  for (int i = 0; i < kMaxDynamicLoads; ++i) {
    if (!dynamicLoads[i].active)
      continue;
    JsonObject item = dyn.createNestedObject();
    item["id"] = i;
    item["name"] = dynamicLoads[i].name;
    item["minW"] = dynamicLoads[i].minW;
    item["maxW"] = dynamicLoads[i].maxW;
  }
  doc["kwh"] = tele.totalKwh;
  doc["cost"] = tele.sessionCostRs;
  doc["mob_kwh"] = tele.mobileKwh;
  doc["lap_kwh"] = tele.laptopKwh;
  doc["unk_kwh"] = tele.unknownKwh;
  doc["relay"] = relayState;
  doc["billing_active"] = billing.isActive;
  doc["billing_admin_lock"] = billing.adminLock;
  doc["billing_timer_running"] = billing.timerRunning;
  doc["billing_remaining_sec"] = billing.remainingSeconds;
  doc["billing_session_kwh"] = billing.sessionStartKwh;
  doc["billing_cost_per_hour"] = billing.costPerHour;
  doc["billing_relay_allowed"] = billing.relayAllowed;
  doc["analysis_active"] = tele.analysisActive;
  doc["analysis_min_va"] = tele.analysisMinVA;
  doc["analysis_max_va"] = tele.analysisMaxVA;
  doc["age_ms"] = (unsigned long)(millis() - tele.lastUpdatedMs);
  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", out);
}

void handleApiLoads() {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.createNestedArray("loads");
  for (int i = 0; i < kMaxDynamicLoads; ++i) {
    if (!dynamicLoads[i].active)
      continue;
    JsonObject item = arr.createNestedObject();
    item["id"] = i;
    item["name"] = dynamicLoads[i].name;
    item["minW"] = dynamicLoads[i].minW;
    item["maxW"] = dynamicLoads[i].maxW;
  }
  doc["analysisActive"] = tele.analysisActive;
  doc["analysisMinVA"] = tele.analysisMinVA;
  doc["analysisMaxVA"] = tele.analysisMaxVA;
  doc["analysisValue"] = tele.realW;
  doc["dynamicDetected"] = tele.dynamicDevice;
  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleApiLoadAdd() {
  String name = server.arg("name");
  float minW = server.arg("min").toFloat();
  float maxW = server.arg("max").toFloat();
  bool ok = false;
  for (int i = 0; i < kMaxDynamicLoads; ++i) {
    if (!dynamicLoads[i].active) {
      dynamicLoads[i].active = true;
      dynamicLoads[i].name = name;
      dynamicLoads[i].minW = minW;
      dynamicLoads[i].maxW = maxW;
      UNO_SERIAL.printf("ADDLOAD,%s,%.1f,%.1f\n", name.c_str(), minW, maxW);
      ok = true;
      saveDynamicLoads();
      break;
    }
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleApiLoadUpdate() {
  int id = server.arg("id").toInt();
  String name = server.arg("name");
  float minW = server.arg("min").toFloat();
  float maxW = server.arg("max").toFloat();
  bool ok = false;
  if (id >= 0 && id < kMaxDynamicLoads && dynamicLoads[id].active) {
    dynamicLoads[id].name = name;
    dynamicLoads[id].minW = minW;
    dynamicLoads[id].maxW = maxW;
    UNO_SERIAL.printf("UPDATELOAD,%d,%s,%.1f,%.1f\n", id, name.c_str(), minW,
                      maxW);
    ok = true;
  }
  if (ok)
    saveDynamicLoads();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleApiLoadDelete() {
  String ids = server.arg("id");
  bool ok = false;
  while (ids.length() > 0) {
    int comma = ids.indexOf(',');
    String token = comma >= 0 ? ids.substring(0, comma) : ids;
    int id = token.toInt();
    if (id >= 0 && id < kMaxDynamicLoads && dynamicLoads[id].active) {
      dynamicLoads[id].active = false;
      dynamicLoads[id].name = "";
      dynamicLoads[id].minW = 0;
      dynamicLoads[id].maxW = 0;
      UNO_SERIAL.printf("DELETELOAD,%d\n", id);
      ok = true;
    }
    if (comma < 0)
      break;
    ids = ids.substring(comma + 1);
  }
  if (ok)
    saveDynamicLoads();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleApiAnalysisStart() {
  String name = server.arg("name");
  if (name.length() == 0)
    name = "DynLoad";
  tele.analysisActive = true;
  tele.analysisMinVA = 1e6;
  tele.analysisMaxVA = 0;
  tele.analysisStartMs = millis();
  UNO_SERIAL.print("STARTANALYSIS,");
  UNO_SERIAL.println(name);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiAnalysisStop() {
  bool ok = false;
  UNO_SERIAL.println("STOPANALYSIS");
  if (tele.analysisActive && tele.analysisMinVA < 1e5f &&
      tele.analysisMaxVA > 0) {
    // Get the name from the capture input (already in browser)
    String name = server.arg("name");
    if (name.length() == 0)
      name = "Analysis";

    // Find first available slot and add the captured load
    for (int i = 0; i < kMaxDynamicLoads; ++i) {
      if (!dynamicLoads[i].active) {
        dynamicLoads[i].active = true;
        dynamicLoads[i].name = name;
        dynamicLoads[i].minW = tele.analysisMinVA;
        dynamicLoads[i].maxW = tele.analysisMaxVA;
        UNO_SERIAL.printf("ADDLOAD,%s,%.1f,%.1f\n", name.c_str(),
                          tele.analysisMinVA, tele.analysisMaxVA);
        Serial.printf("[ANALYSIS] Saved load: %s (%.1f-%.1fW)\n", name.c_str(),
                      tele.analysisMinVA, tele.analysisMaxVA);
        ok = true;
        saveDynamicLoads();
        break;
      }
    }
  }
  tele.analysisActive = false;
  tele.analysisMinVA = 0;
  tele.analysisMaxVA = 0;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

// ─── Relay control ───────────────────────────────────────────────────────────
void setRelay(bool on) {
  // Admin lock supercedes everything - force OFF when locked
  if (billing.adminLock) {
    digitalWrite(RELAY_PIN, HIGH); // forced off (active-low)
    relayState = false;
    UNO_SERIAL.println("OFF");
    return;
  }

  // When timer is NOT running, relay must stay OFF regardless of request
  if (!billing.timerRunning) {
    digitalWrite(RELAY_PIN, HIGH);
    relayState = false;
    UNO_SERIAL.println("OFF");
    return;
  }

  // When remaining seconds is 0, relay must stay OFF
  if (billing.remainingSeconds == 0) {
    digitalWrite(RELAY_PIN, HIGH);
    relayState = false;
    UNO_SERIAL.println("OFF");
    return;
  }

  // Allow relay ON only when timer is active AND remaining > 0
  relayState = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  UNO_SERIAL.println(on ? "ON" : "OFF");
}
void handleRelayOn() {
  // Check admin lock first
  if (billing.adminLock) {
    server.send(403, "text/plain", "ADMIN_LOCKED");
    return;
  }

  // Check if timer is running and has credit
  if (!billing.timerRunning || billing.remainingSeconds == 0) {
    server.send(403, "text/plain", "NO_CREDIT");
    return;
  }

  setRelay(true);
  server.send(200, "text/plain", "ON");
}

void handleRelayOff() {
  setRelay(false);
  // Timer auto-pauses via updateBillingTimer() guard when relayState=false
  server.send(200, "text/plain", "OFF");
}

void handleRelayToggle() {
  // Check admin lock first
  if (billing.adminLock) {
    server.send(403, "text/plain", "ADMIN_LOCKED");
    return;
  }

  // Check if timer is running and has credit when trying to turn ON
  if (!relayState && (!billing.timerRunning || billing.remainingSeconds == 0)) {
    server.send(403, "text/plain", "NO_CREDIT");
    return;
  }

  setRelay(!relayState);
  server.send(200, "text/plain", relayState ? "ON" : "OFF");
}

void updateBillingTimer() {
  if (!billing.timerRunning)
    return;

  // Don't count down if relay is off (user paused)
  if (!relayState) {
    billing.lastTick = millis(); // keep timer synched while relay is off
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - billing.lastTick;
  if (elapsed < 1000UL)
    return;

  billing.lastTick = now;
  unsigned long elapsedSec = elapsed / 1000UL;
  if (elapsedSec == 0)
    return;

  unsigned long oldRemaining = billing.remainingSeconds;
  if (billing.remainingSeconds > elapsedSec) {
    billing.remainingSeconds -= elapsedSec;
  } else {
    billing.remainingSeconds = 0;
  }

  // 1-day left alert (send once when crossing into <= 86400 seconds)
  if (!billing.oneDayAlertSent && oldRemaining > 86400UL &&
      billing.remainingSeconds <= 86400UL) {
    UNO_SERIAL.println("BUZZ5");
    billing.oneDayAlertSent = true;
  }

  // Timer expired
  if (billing.remainingSeconds == 0) {
    billing.timerRunning = false;
    billing.relayAllowed = false;
    billing.isActive = false;

    // Record the session before turning off
    unsigned long usedSeconds =
        billing.sessionStartMs ? (millis() - billing.sessionStartMs) / 1000UL
                               : 0;
    recordBillingSession(usedSeconds);
    billing.sessionStartMs = 0;
    billing.sessionStartKwh = 0;

    // Force relay OFF
    setRelay(false);
  }

  saveBillingToEEPROM();
}

void startBillingTimer() {
  // Don't start if already running or if admin locked
  if (billing.adminLock || billing.timerRunning)
    return;
  if (billing.remainingSeconds == 0)
    return;

  if (billing.remainingSeconds > 86400UL) {
    billing.oneDayAlertSent = false;
  }

  billing.timerRunning = true;
  billing.relayAllowed = true;
  billing.lastTick = millis();

  if (billing.sessionStartMs == 0) {
    billing.sessionStartMs = millis();
    billing.sessionStartKwh = tele.totalKwh;
  }

  billing.isActive = true;

  // Only turn relay ON if we're starting the timer (not admin locked)
  if (!billing.adminLock) {
    setRelay(true);
  }

  saveBillingToEEPROM();
}

void pauseBillingTimer() {
  billing.timerRunning = false;
  setRelay(false);
  saveBillingToEEPROM();
}

void stopBillingTimer() {
  if (billing.timerRunning) {
    unsigned long usedSeconds =
        billing.sessionStartMs ? ((millis() - billing.sessionStartMs) / 1000UL)
                               : 0;
    recordBillingSession(usedSeconds);
  }
  billing.timerRunning = false;
  billing.relayAllowed = false;
  billing.isActive = false;
  billing.remainingSeconds = 0;
  billing.oneDayAlertSent = false;
  billing.sessionStartMs = 0;
  billing.sessionStartKwh = 0;
  setRelay(false);
  saveBillingToEEPROM();
}

void handleTopup() {
  // Admin lock blocks top-up
  if (billing.adminLock) {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"admin lock active\"}");
    return;
  }

  int seconds = server.arg("seconds").toInt();
  if (seconds <= 0) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid seconds\"}");
    return;
  }

  // Add to remaining seconds
  if (billing.remainingSeconds + (unsigned long)seconds > 31536000UL) {
    billing.remainingSeconds = 31536000UL;
  } else {
    billing.remainingSeconds += (unsigned long)seconds;
  }

  // Initialize timer state
  billing.relayAllowed = true;
  billing.timerRunning = true;
  billing.isActive = true;

  if (billing.sessionStartMs == 0) {
    billing.sessionStartMs = millis();
    billing.sessionStartKwh = tele.totalKwh;
  }

  billing.lastTick = millis();
  billing.oneDayAlertSent = (billing.remainingSeconds <= 86400UL);
  saveBillingToEEPROM();

  // Turn relay ON only if not admin locked
  if (!billing.adminLock) {
    setRelay(true);
  }

  // Return success with updated remaining seconds for frontend
  String response = String("{\"ok\":true,\"remaining_sec\":") +
                    billing.remainingSeconds + "}";
  server.send(200, "application/json", response);
}

void handleTimerStart() {
  if (requireAdmin()) {
    startBillingTimer();
    server.send(200, "application/json", "{\"ok\":true}");
  }
}
void handleTimerPause() {
  if (requireAdmin()) {
    pauseBillingTimer();
    server.send(200, "application/json", "{\"ok\":true}");
  }
}
void handleTimerStop() {
  if (requireAdmin()) {
    stopBillingTimer();
    server.send(200, "application/json", "{\"ok\":true}");
  }
}

bool requireAdmin() {
  if (!isAdminSession) {
    server.send(401, "application/json",
                "{\"ok\":false,\"error\":\"unauthorized\"}");
    return false;
  }
  return true;
}

void handleAdminLock() {
  if (!requireAdmin())
    return;
  String action = server.arg("action");

  if (action == "on") {
    billing.adminLock = true;
    // Force relay OFF immediately
    setRelay(false);
  } else if (action == "off") {
    billing.adminLock = false;

    // Restore timer state if credit exists
    if (billing.remainingSeconds > 0) {
      billing.timerRunning = true;
      billing.relayAllowed = true;
      billing.isActive = true;
      billing.lastTick = millis();

      if (billing.sessionStartMs == 0) {
        billing.sessionStartMs = millis();
        billing.sessionStartKwh = tele.totalKwh;
      }

      // Turn relay ON only if there was previous state OR we're restarting
      setRelay(true);
    } else {
      billing.timerRunning = false;
      billing.relayAllowed = false;
      billing.isActive = false;
    }
  } else {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid action\"}");
    return;
  }

  saveBillingToEEPROM();
  server.send(200, "application/json",
              String("{\"ok\":true,\"adminLock\":") +
                  (billing.adminLock ? "true" : "false") + "}");
}

void handleLogin() {
  String u = server.arg("user");
  String p = server.arg("pass");
  if (u == ADMIN_USER && p == ADMIN_PASS) {
    isAdminSession = true;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(401, "application/json", "{\"ok\":false}");
  }
}

void handleLogout() {
  isAdminSession = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRecords() {
  if (!requireAdmin())
    return;
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("records");
  for (auto &r : billingRecords) {
    JsonObject o = arr.createNestedObject();
    o["duration"] = r.duration;
    o["energyUsed"] = r.energyUsed;
    o["cost"] = r.cost;
  }
  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ─── LCD Display update ──────────────────────────────────────────────────────
void updateLcdDisplay() {
  unsigned long now = millis();
  if (showingIpAddress &&
      (now - lcdIpDisplayStartMs >= LCD_IP_DISPLAY_DURATION_MS)) {
    showingIpAddress = false;
    calibrationInProgress = true;
    calibrationStartMs = now;
    lcd.clear();
  }
  if (showingIpAddress)
    return;
  if (calibrationInProgress && (now - calibrationStartMs < 8000)) {
    static unsigned long lastCalibUpdateMs = 0;
    if (now - lastCalibUpdateMs >= 1000) {
      lastCalibUpdateMs = now;
      unsigned long rem = 8000 - (now - calibrationStartMs);
      lcd.setCursor(0, 0);
      lcd.print("** CALIBRATING **");
      lcd.setCursor(0, 1);
      char line[17];
      snprintf(line, sizeof(line), "Relay OFF %d sec", (int)(rem / 1000));
      lcd.print(line);
    }
    return;
  }
  if (calibrationInProgress) {
    calibrationInProgress = false;
    lcd.clear();
  }
  if (now - lastLcdUpdateMs < LCD_UPDATE_INTERVAL_MS)
    return;
  lastLcdUpdateMs = now;
  lcd.setCursor(0, 0);
  char line1[17];
  snprintf(line1, sizeof(line1), "V:%3.0f I:%4.2f %3dW", tele.voltage,
           tele.current, (int)tele.realW);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  char prefix[5];
  if (tele.dynamicDevice[0] != '-' && tele.dynamicDevice[0] != '\0') {
    strcpy(prefix, "Det:");
  } else {
    strcpy(prefix, "Load:");
  }
  const char *dispDev =
      (tele.dynamicDevice[0] != '-' && tele.dynamicDevice[0] != '\0')
          ? tele.dynamicDevice
          : tele.device;
  char line2[17];
  snprintf(line2, sizeof(line2), "%s%-3s", prefix, dispDev);
  // Clear the second line before printing new text
  lcd.setCursor(0, 1);
  for (int i = 0; i < 16; i++)
    lcd.print(" ");
  lcd.setCursor(0, 1);
  lcd.print(line2);

  // Debug LCD output
  Serial.printf("[LCD] Line1: '%s' | Line2: '%s'\n", line1, line2);
}

// ─── Dashboard HTML ──────────────────────────────────────────────────────────
static const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=yes, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#C08552">
<title>Smart Load Monitor · Kolkata</title>
<link rel="preconnect" href="https://cdnjs.cloudflare.com">
<link rel="preconnect" href="https://cdn.jsdelivr.net">
<link href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css" rel="stylesheet">
<style>
:root{--bg-color:#FFF8F0;--primary-text:#4B2E2B;--accent:#C08552;--secondary:#8C5A3C;--card-bg:#FFFFFF;--border-radius:12px;--transition:all 0.3s ease;--shadow:0 4px 20px rgba(75,46,43,0.05);--shadow-hover:0 8px 30px rgba(75,46,43,0.1);--sidebar-width:80px;--content-padding:2rem}
*{margin:0;padding:0;box-sizing:border-box;font-family:'Segoe UI',system-ui,-apple-system,BlinkMacSystemFont,sans-serif}
body{background-color:var(--bg-color);color:var(--primary-text);display:flex;height:100vh;overflow:hidden}
.sidebar{width:var(--sidebar-width);background-color:var(--card-bg);display:flex;flex-direction:column;align-items:center;padding:2rem 0;box-shadow:2px 0 15px rgba(0,0,0,0.03);z-index:10;flex-shrink:0;transition:width 0.3s ease}
.sidebar-logo{font-size:1.5rem;color:var(--accent);margin-bottom:3rem;font-weight:700}
.nav-items{display:flex;flex-direction:column;gap:2rem;width:100%}
.nav-item{position:relative;display:flex;justify-content:center;align-items:center;width:100%;padding:1rem 0;color:var(--secondary);cursor:pointer;transition:var(--transition);min-height:44px}
.nav-item:hover,.nav-item.active{color:var(--accent)}
.nav-item.active::before{content:'';position:absolute;left:0;top:50%;transform:translateY(-50%);width:4px;height:24px;background-color:var(--accent);border-radius:0 4px 4px 0}
.nav-item i{font-size:1.25rem}
.nav-tooltip{position:absolute;left:80px;background:var(--primary-text);color:white;padding:0.5rem 1rem;border-radius:6px;font-size:0.85rem;opacity:0;visibility:hidden;transition:var(--transition);white-space:nowrap;pointer-events:none;z-index:20}
.nav-item:hover .nav-tooltip{opacity:1;visibility:visible;left:70px}
.main-content{flex:1;display:flex;flex-direction:column;overflow-y:auto;overflow-x:hidden;padding:var(--content-padding);min-width:0;-webkit-overflow-scrolling:touch}
.top-bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:2.5rem;flex-wrap:wrap;gap:1rem}
.top-left{display:flex;flex-direction:column;gap:0.25rem}
.system-status{display:flex;align-items:center;gap:0.5rem;font-size:0.9rem;font-weight:500;color:var(--secondary)}
.status-dot{width:10px;height:10px;background-color:#22c55e;border-radius:50%;box-shadow:0 0 10px #22c55e80;animation:pulse 2s infinite}
@keyframes pulse{0%{transform:scale(0.95);box-shadow:0 0 0 0 rgba(34,197,94,0.7)}70%{transform:scale(1);box-shadow:0 0 0 6px rgba(34,197,94,0)}100%{transform:scale(0.95);box-shadow:0 0 0 0 rgba(34,197,94,0)}}
.current-load-title{font-size:1.75rem;font-weight:700;color:var(--primary-text)}
.top-right{display:flex;align-items:center;gap:2rem;flex-wrap:wrap;justify-content:flex-end;flex:1}
.clock{font-size:1.1rem;font-weight:500;color:var(--secondary)}
.relay-toggle{display:flex;align-items:center;gap:1rem;background:var(--card-bg);padding:0.75rem 1.5rem;border-radius:50px;box-shadow:var(--shadow);min-height:44px}
.switch-label{font-weight:600;font-size:0.9rem}
.switch{position:relative;display:inline-block;width:50px;height:26px}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;transition:.4s;border-radius:34px}
.slider:before{position:absolute;content:"";height:18px;width:18px;left:4px;bottom:4px;background-color:white;transition:.4s;border-radius:50%}
input:checked+.slider{background-color:var(--accent)}
input:checked+.slider:before{transform:translateX(24px)}
.kpi-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:1rem;margin-bottom:2rem}
.kpi-card{background:var(--card-bg);padding:1.5rem 1rem;border-radius:var(--border-radius);box-shadow:var(--shadow);display:flex;flex-direction:column;align-items:center;justify-content:center;transition:var(--transition);cursor:pointer;min-height:140px}
.kpi-card:hover{transform:translateY(-5px);box-shadow:var(--shadow-hover)}
.kpi-value{font-size:1.8rem;font-weight:700;color:var(--primary-text);margin-bottom:0.25rem;display:flex;align-items:baseline;gap:0.25rem;word-break:break-word}
.kpi-unit{font-size:0.9rem;color:var(--accent);font-weight:600}
.kpi-label{font-size:0.8rem;text-transform:uppercase;letter-spacing:1px;color:var(--secondary);font-weight:500;text-align:center}
.dashboard-grid{display:grid;grid-template-columns:3fr 1fr;gap:2rem;margin-bottom:2rem}
.section-header{font-size:1.1rem;font-weight:600;margin-bottom:1rem;color:var(--primary-text)}
.card-container{background:var(--card-bg);border-radius:var(--border-radius);padding:1.5rem;box-shadow:var(--shadow);overflow-x:auto}
.mini-card{background:var(--card-bg);border-radius:var(--border-radius);padding:1.5rem;box-shadow:var(--shadow);margin-bottom:1.5rem}
.mini-card:last-child{margin-bottom:0}
.breakdown-row{display:flex;justify-content:space-between;margin-bottom:1rem;align-items:center;flex-wrap:wrap;gap:0.5rem}
.breakdown-row:last-child{margin-bottom:0}
.breakdown-label{font-size:0.9rem;color:var(--secondary);display:flex;align-items:center;gap:0.5rem}
.breakdown-val{font-weight:600;color:var(--primary-text)}
.progress-bg{width:100%;height:6px;background:var(--bg-color);border-radius:10px;overflow:hidden;margin-top:0.5rem}
.progress-bar{height:100%;background:var(--accent);border-radius:10px;transition:width 0.5s ease}
.badge{padding:0.25rem 0.75rem;border-radius:20px;font-size:0.75rem;font-weight:600;background:rgba(192,133,82,0.1);color:var(--accent);white-space:nowrap}
.badge.active{background:rgba(34,197,94,0.1);color:#22c55e}
.side-stack{display:flex;flex-direction:column;gap:1.5rem;height:100%}
.device-info{font-size:1.5rem;font-weight:700;margin-bottom:0.5rem;word-break:break-word}
.runtime-cost{margin-top:1.5rem;display:flex;justify-content:space-between;flex-wrap:wrap;gap:1rem}
.runtime-cost>div{flex:1;min-width:80px}
.runtime-cost label{font-size:0.8rem;color:var(--secondary)}
.runtime-cost .val{font-weight:600;display:block}
.btn-primary{background:var(--accent);color:white;border:none;padding:0.8rem 1.6rem;border-radius:8px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:0.5rem;transition:all 0.2s ease;font-size:1rem;box-shadow:0 2px 8px rgba(0,0,0,0.15);min-height:44px;min-width:44px;text-decoration:none;flex-wrap:wrap}
.btn-primary:hover{filter:brightness(1.15);box-shadow:0 6px 16px rgba(0,0,0,0.25);transform:translateY(-2px)}
.btn-primary:active{transform:translateY(0)}
.btn-green{background:#22c55e!important}
.btn-red{background:#ef4444!important}
.btn-dark{background:var(--secondary)!important}
table{width:100%;border-collapse:collapse;margin-top:1rem;display:table}
thead{display:table-header-group}
tr{display:table-row}
th{text-align:left;padding:1rem;color:var(--secondary);font-weight:500;font-size:0.85rem;border-bottom:1px solid rgba(140,90,60,0.1);min-width:100px}
td{padding:0.75rem 1rem;font-size:0.95rem;color:var(--primary-text);border-bottom:1px solid rgba(140,90,60,0.05);vertical-align:middle;min-width:100px}
tr:hover td{background:rgba(255,248,240,0.5)}
.status-badge{padding:0.35rem 0.75rem;border-radius:20px;font-size:0.75rem;font-weight:600;background:rgba(192,133,82,0.1);color:var(--accent);white-space:nowrap}
.status-badge.on{background:rgba(34,197,94,0.1);color:#22c55e}
.load-row{display:flex;align-items:center;gap:0.75rem;padding:0.9rem 1.2rem;background:var(--card-bg);border-radius:10px;box-shadow:var(--shadow);border:1px solid rgba(140,90,60,0.08);flex-wrap:wrap}
.load-row input[type=text],.load-row input[type=number]{border:1.5px solid rgba(140,90,60,0.25);border-radius:6px;padding:0.45rem 0.6rem;font-size:0.9rem;color:var(--primary-text);background:var(--bg-color);outline:none;min-height:38px;font-size:16px}
.load-row input[type=text]:focus,.load-row input[type=number]:focus{border-color:var(--accent)}
.load-row input[type=text]{flex:2;min-width:80px}
.load-row input[type=number]{width:70px;min-width:70px}
.load-row .load-label{font-size:0.75rem;color:var(--secondary);font-weight:500;white-space:nowrap}
.analysis-banner{padding:1.25rem 1.5rem;background:rgba(34,197,94,0.08);border:1.5px solid #22c55e;border-radius:10px;display:flex;align-items:center;gap:1rem;margin-bottom:1.5rem;flex-wrap:wrap}
.analysis-banner.idle{background:rgba(192,133,82,0.06);border-color:var(--accent)}
.pulse-dot{width:10px;height:10px;border-radius:50%;background:#22c55e;flex-shrink:0;animation:pulse 1.2s infinite}
.pulse-dot.idle{background:var(--accent);animation:none}
.section-divider{height:1px;background:rgba(140,90,60,0.1);margin:1.5rem 0}
.loading-overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.5);display:flex;justify-content:center;align-items:center;z-index:9999}
.spinner{width:40px;height:40px;border:4px solid #f3f3f3;border-top:4px solid var(--accent);border-radius:50%;animation:spin 1s linear infinite}
@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}
input[type=text],input[type=number]{font-family:inherit}
@media(hover:hover){.btn-primary:hover{filter:brightness(1.15);transform:translateY(-2px)}.card-container:hover{transform:translateY(-2px);box-shadow:var(--shadow-hover)}}
.analytics-kpi-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:1rem;margin-bottom:1.5rem}
.analytics-charts-grid{display:grid;grid-template-columns:1fr 1fr;gap:1.5rem;margin-bottom:1.5rem}
@media(max-width:1024px){.dashboard-grid{grid-template-columns:1fr}.kpi-row{grid-template-columns:repeat(3,1fr)}.analytics-charts-grid{grid-template-columns:1fr}}
@media(max-width:768px){:root{--sidebar-width:60px;--content-padding:1rem}
  .main-content{padding:1rem}.kpi-row{grid-template-columns:repeat(2,1fr);gap:0.75rem}
  .top-bar{flex-direction:column;gap:1rem;align-items:flex-start}
  .top-right{width:100%;display:flex;flex-direction:column;align-items:stretch;gap:0.75rem}
  .top-right > div:last-child { flex-wrap: wrap; justify-content: space-between; gap: 0.5rem; }
  .relay-toggle{order:1;justify-content:space-between}
  #topupCostPreview { order: 2; justify-content: center; width: auto; min-width: 100px; }
  .top-right .btn-primary { order: 3; width: auto; min-width: 120px; }
  .card-container{padding:1rem}.kpi-card{padding:1rem;min-height:120px}.kpi-value{font-size:1.4rem}.device-info{font-size:1.2rem}.load-row{flex-direction:column;align-items:stretch;gap:0.5rem;padding:1rem}.load-row input[type=text],.load-row input[type=number]{width:100%!important}.btn-primary{width:100%;margin-bottom:0.5rem;min-height:48px}.nav-item{padding:1rem 0;min-height:54px}.nav-item i{font-size:1.5rem}#analysisBanner+.card-container>div{flex-direction:column}#analysisBanner+.card-container button{width:100%}.card-container>div{flex-wrap:wrap}.section-header{font-size:1rem}.analytics-kpi-grid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:480px){body{flex-direction:column}.sidebar{width:100%;height:65px;flex-direction:row;order:2;padding:0;padding-bottom:env(safe-area-inset-bottom);border-top:1px solid rgba(0,0,0,0.05);box-shadow:0 -2px 10px rgba(0,0,0,0.05)}.sidebar-logo{display:none}.nav-items{flex-direction:row;justify-content:space-around;height:100%;gap:0}.nav-item{padding:0;min-height:unset;flex:1;border-radius:0;display:flex;justify-content:center;align-items:center}.nav-item.active::before{width:100%;height:3px;top:0;left:0;transform:none;border-radius:0 0 4px 4px}.nav-tooltip{display:none}.main-content{padding:1rem;padding-bottom:1rem}.kpi-row{grid-template-columns:1fr}.dashboard-grid{gap:1rem}.section-header{font-size:1rem}.kpi-value{font-size:1.2rem}.top-right > div:last-child { flex-direction: column; gap: 0.5rem; }
  .top-right > div:last-child > div:first-child { width: 100%; justify-content: space-between; }
  #topupCostPreview { width: 100%; justify-content: center; }
  .top-right .btn-primary { width: 100%; }
  .relay-toggle{justify-content:space-between}input[type=text],input[type=number],select{width:100%!important;margin-bottom:0.5rem;min-height:48px}.card-container .custom-duration{flex-direction:column}input,select,textarea,button{font-size:16px!important}.graph-card.fullscreen{width:100vw;height:100vh;border-radius:0;padding:1rem}.analytics-kpi-grid{grid-template-columns:repeat(4,1fr);gap:0.25rem}.analytics-kpi-grid .kpi-card{padding:0.5rem 0.25rem;min-height:unset;border-radius:8px}.analytics-kpi-grid .kpi-value{font-size:1rem}.analytics-kpi-grid .kpi-label{font-size:0.55rem;letter-spacing:0}.analytics-kpi-grid .kpi-unit{font-size:0.7rem}}
.graph-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.6);z-index:99998;backdrop-filter:blur(4px);cursor:pointer}
.graph-overlay.active{display:block}
.graph-card{cursor:pointer;transition:transform 0.3s ease,box-shadow 0.3s ease;position:relative}
.graph-card:hover{transform:translateY(-2px);box-shadow:var(--shadow-hover)}
.graph-card::after{content:'\f0b2';font-family:'Font Awesome 6 Free';font-weight:900;position:absolute;top:1.25rem;right:1.25rem;color:var(--secondary);opacity:0;transition:0.2s}
.graph-card:hover::after{opacity:0.6}
.graph-card.fullscreen{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:95vw;height:85vh;z-index:99999;background:var(--card-bg);border-radius:16px;display:flex;flex-direction:column;padding:1.5rem;margin:0;cursor:default}
.graph-card.fullscreen > div:nth-child(2) { padding-right: 3.5rem; }
.graph-card.fullscreen::after{display:none}
.graph-card.fullscreen .canvas-wrapper{flex:1;height:auto!important;min-height:300px;margin-top:1rem}
.close-fullscreen{display:none;position:absolute;top:1.25rem;right:1.25rem;background:rgba(239,68,68,0.1);color:#ef4444;width:36px;height:36px;border-radius:50%;text-align:center;line-height:36px;cursor:pointer;transition:0.2s;font-size:1.2rem}
.close-fullscreen:hover{background:#ef4444;color:#fff}
.graph-card.fullscreen .close-fullscreen{display:block}
</style>
</head>
<body>

<nav class="sidebar">
  <div class="sidebar-logo"><i class="fa-solid fa-bolt"></i></div>
  <div class="nav-items">
    <div id="dashboardNavBtn" class="nav-item active">
      <i class="fa-solid fa-chart-line"></i><span class="nav-tooltip">Dashboard</span>
    </div>
    <div id="loadsNavBtn" class="nav-item">
      <i class="fa-solid fa-plug"></i><span class="nav-tooltip">Loads</span>
    </div>
    <div id="analyticsNavBtn" class="nav-item">
      <i class="fa-solid fa-chart-pie"></i><span class="nav-tooltip">Analytics</span>
    </div>
    <div id="billingNavBtn" class="nav-item">
      <i class="fa-solid fa-file-invoice-dollar"></i><span class="nav-tooltip">Billing</span>
    </div>
    <div class="nav-item">
      <i class="fa-solid fa-gear"></i><span class="nav-tooltip">Settings</span>
    </div>
  </div>
</nav>

<main class="main-content">

  <!-- ═══════════════════════════ DASHBOARD PANE ═══════════════════════════ -->
  <div id="dashboardPane">

    <header class="top-bar">
      <div class="top-left">
        <div class="system-status"><div class="status-dot"></div> Live Connection</div>
        <div id="billingStatus" class="system-status" style="margin-top:4px;font-size:0.85rem;color:#c2410c;">&nbsp;</div>
        <div class="current-load-title">Core System Overview</div>
      </div>
      <div class="top-right">
        <div class="clock" id="liveClock">00:00:00</div>
        <div class="relay-toggle">
          <span class="switch-label">RELAY</span>
          <label class="switch">
            <input type="checkbox" id="mainRelay" onchange="relayCmd(this.checked?'on':'off')">
            <span class="slider"></span>
          </label>
        </div>
        <div style="display:flex;gap:0.75rem;align-items:center;flex-wrap:wrap">
          <div style="display:flex;gap:0.5rem;align-items:center;background:var(--card-bg);padding:0.5rem 1rem;border-radius:50px;box-shadow:var(--shadow);">
            <select id="topupPreset" style="padding:0.5rem;border:1px solid rgba(140,90,60,0.25);border-radius:8px;font-size:0.85rem;background:var(--bg-color);" onchange="syncTopupFromPreset()">
              <option value="">Custom</option>
              <option value="1">1 Day</option>
              <option value="3">3 Days</option>
              <option value="7">7 Days</option>
              <option value="15">15 Days</option>
              <option value="30">30 Days</option>
              <option value="60">60 Days</option>
              <option value="90">90 Days</option>
            </select>
            <input id="topupDays" type="number" min="1" max="90" placeholder="days" style="width:75px;padding:0.5rem;border:1px solid rgba(140,90,60,0.25);border-radius:8px;font-size:0.85rem;background:var(--bg-color);" oninput="updateTopupCostPreview()">
          </div>
          <div id="topupCostPreview" style="background:var(--accent);color:white;padding:0.5rem 1rem;border-radius:50px;font-weight:700;font-size:0.9rem;box-shadow:var(--shadow);display:flex;align-items:center;gap:0.5rem;">
            <i class="fa-solid fa-indian-rupee-sign"></i>
            <span id="topupCostValue">0.00</span>
          </div>
          <button class="btn-primary" style="padding:0.5rem 1rem;font-size:0.9rem;" onclick="topup()">
            <i class="fa-solid fa-bolt"></i> Top-up
          </button>
        </div>
      </div>
    </header>

    <div class="kpi-row">
      <div class="kpi-card"><div class="kpi-value"><span id="val_V">0</span><span class="kpi-unit">V</span></div><div class="kpi-label">Voltage</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="val_I">0.00</span><span class="kpi-unit">A</span></div><div class="kpi-label">Current</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="val_P">0</span><span class="kpi-unit">W</span></div><div class="kpi-label">Real Power</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="val_S">0</span><span class="kpi-unit">VA</span></div><div class="kpi-label">Apparent Power</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="val_Q">0</span><span class="kpi-unit">VAr</span></div><div class="kpi-label">Reactive Power</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="val_PF">0.00</span></div><div class="kpi-label">Power Factor</div></div>
    </div>

    <div class="dashboard-grid">
      <div class="card-container">
        <div class="section-header">System Status</div>
        <div class="mini-card">
          <div class="section-header">Power Breakdown</div>
          <div class="breakdown-row"><span class="breakdown-label"><i class="fa-solid fa-fire"></i> Active Power</span><span class="breakdown-val" id="bd_P">0 W</span></div>
          <div class="progress-bg"><div class="progress-bar" id="bar_P" style="width:0%"></div></div>
          <div class="breakdown-row" style="margin-top:1rem"><span class="breakdown-label"><i class="fa-solid fa-wind"></i> Reactive Power</span><span class="breakdown-val" id="bd_Q">0 VAr</span></div>
          <div class="progress-bg"><div class="progress-bar" id="bar_Q" style="width:0%;background:var(--secondary);opacity:0.6"></div></div>
          <div class="breakdown-row" style="margin-top:1rem"><span class="breakdown-label"><i class="fa-solid fa-circle-info"></i> Distortion</span><span class="breakdown-val" id="bd_DI">0%</span></div>
        </div>
        <div class="mini-card">
          <div class="section-header">Identified Load</div>
          <div class="device-info" id="activeLoadType">Detecting...</div>
          <div style="font-size:0.9rem;color:var(--secondary);margin-bottom:0.5rem" id="dynamicLoad">Dynamic detection: -</div>
          <div class="badge active" id="activeLoadState">Idle</div>
          <div class="runtime-cost">
            <div><label>Session Energy</label><div class="val" id="sessionKwh">0.000 kWh</div></div>
            <div style="text-align:right"><label>Est. Cost</label><div class="val" id="estCost">₹0.00</div></div>
          </div>
        </div>
      </div>
      <div class="side-stack">
        <div class="card-container">
          <div class="section-header">Energy Breakdown</div>
            <div id="energyBreakdown"></div>
        </div>
        <div class="card-container">
          <div class="section-header">Device Status</div>
          <div style="text-align:center;padding:1rem 0">
            <div style="font-size:2rem;margin-bottom:0.5rem"><span id="relayStatusIcon" style="color:var(--accent)"><i class="fa-solid fa-power-off"></i></span></div>
            <div style="font-size:1.25rem;font-weight:700;margin-bottom:0.5rem" id="relayStatusText">OFFLINE</div>
            <div class="status-badge" id="relayBadge">OFF</div>
          </div>
        </div>
      </div>
    </div>

    <div class="card-container">
      <div class="section-header">Session Statistics</div>
      <table>
        <thead><tr><th>Metric</th><th>Value</th><th>Unit</th></tr></thead>
        <tbody>
          <tr><td>Total Energy</td><td id="total_kwh">0.000000</td><td>kWh</td></tr>
          <tr><td>Session Cost</td><td id="session_cost">₹0.000</td><td>INR</td></tr>
          <tr><td>Power Factor</td><td id="avg_pf">0.00</td><td>cos φ</td></tr>
          <tr><td>Data Age</td><td id="data_age">connecting…</td><td>seconds</td></tr>
        </tbody>
      </table>
    </div>

  </div><!-- /dashboardPane -->

  <!-- ════════════════════════════ LOADS PANE ═════════════════════════════ -->
  <div id="loadsPane" style="display:none;">

    <header class="top-bar">
      <div class="top-left">
        <div class="system-status"><div class="status-dot"></div> Live Connection</div>
        <div class="current-load-title">Dynamic Load Management</div>
      </div>
      <div class="top-right">
        <div class="clock" id="liveClockLoads">00:00:00</div>
      </div>
    </header>

    <!-- Analysis banner -->
    <div id="analysisBanner" class="analysis-banner idle">
      <div id="analysisDot" class="pulse-dot idle"></div>
      <div style="flex:1">
        <div style="font-weight:700;font-size:1rem;color:var(--primary-text)" id="analysisBannerTitle">Analysis idle</div>
        <div style="font-size:0.85rem;color:var(--secondary);margin-top:0.2rem" id="analysisBannerSub">Type a name below then press Start Analysis to begin capturing</div>
      </div>
    </div>

    <!-- Analysis controls card -->
    <div class="card-container" style="margin-bottom:1.5rem">
      <div class="section-header">Capture Load Range</div>
      <p style="font-size:0.9rem;color:var(--secondary);margin-bottom:1.25rem">
        1 — Type a name &nbsp;·&nbsp; 2 — Press <strong>Start Analysis</strong> &nbsp;·&nbsp; 3 — Plug in the device &nbsp;·&nbsp; 4 — Press <strong>Stop Analysis</strong>
      </p>
      <div style="display:flex;gap:1rem;flex-wrap:wrap;align-items:center;margin-bottom:1rem">
        <input id="captureNameInput" type="text" placeholder="Load name (e.g. Fan, TV, Charger)"
               style="flex:1;min-width:180px;padding:0.85rem 1rem;border:2px solid rgba(140,90,60,0.3);border-radius:8px;font-size:1rem;color:var(--primary-text);background:var(--bg-color);outline:none"/>
        <button class="btn-primary btn-green" onclick="startAnalysis()" style="font-size:1rem;padding:0.85rem 1.75rem">
          <i class="fa-solid fa-play"></i>&nbsp; Start Analysis
        </button>
        <button class="btn-primary btn-red" onclick="stopAnalysis()" style="font-size:1rem;padding:0.85rem 1.75rem">
          <i class="fa-solid fa-stop"></i>&nbsp; Stop Analysis
        </button>
      </div>
      <div style="font-size:0.85rem;color:var(--secondary)" id="analysisLiveRange">Range: — </div>
    </div>

    <!-- Manual add card -->
    <div class="card-container" style="margin-bottom:1.5rem">
      <div class="section-header">Add Load Manually</div>
      <div style="display:flex;gap:1rem;flex-wrap:wrap;align-items:flex-end">
        <div style="display:flex;flex-direction:column;gap:0.35rem;flex:2;min-width:140px">
          <label style="font-size:0.8rem;color:var(--secondary);font-weight:600">LOAD NAME</label>
          <input id="manualName" type="text" placeholder="e.g. Ceiling Fan"
                 style="padding:0.75rem;border:1.5px solid rgba(140,90,60,0.3);border-radius:8px;font-size:0.95rem;color:var(--primary-text);background:var(--bg-color);outline:none"/>
        </div>
        <div style="display:flex;flex-direction:column;gap:0.35rem;width:110px">
          <label style="font-size:0.8rem;color:var(--secondary);font-weight:600">MIN W</label>
          <input id="manualMin" type="number" placeholder="e.g. 40"
                 style="padding:0.75rem;border:1.5px solid rgba(140,90,60,0.3);border-radius:8px;font-size:0.95rem;color:var(--primary-text);background:var(--bg-color);outline:none;width:100%"/>
        </div>
        <div style="display:flex;flex-direction:column;gap:0.35rem;width:110px">
          <label style="font-size:0.8rem;color:var(--secondary);font-weight:600">MAX W</label>
          <input id="manualMax" type="number" placeholder="e.g. 55"
                 style="padding:0.75rem;border:1.5px solid rgba(140,90,60,0.3);border-radius:8px;font-size:0.95rem;color:var(--primary-text);background:var(--bg-color);outline:none;width:100%"/>
        </div>
        <button class="btn-primary" onclick="addLoad()" style="padding:0.75rem 1.5rem;font-size:0.95rem;align-self:flex-end">
          <i class="fa-solid fa-plus"></i>&nbsp; Add Load
        </button>
      </div>
    </div>

    <!-- Load list card -->
    <div class="card-container">
      <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem;flex-wrap:wrap;gap:0.75rem">
        <div class="section-header" style="margin-bottom:0">Configured Loads</div>
        <div style="display:flex;gap:0.75rem;flex-wrap:wrap">
          <button class="btn-primary btn-dark" onclick="toggleAllLoads(true)" style="font-size:0.85rem;padding:0.55rem 1rem">
            <i class="fa-solid fa-check-double"></i> Select All
          </button>
          <button class="btn-primary btn-dark" onclick="toggleAllLoads(false)" style="font-size:0.85rem;padding:0.55rem 1rem">
            <i class="fa-solid fa-times"></i> Deselect
          </button>
          <button class="btn-primary btn-red" onclick="deleteSelectedLoads()" style="font-size:0.85rem;padding:0.55rem 1rem">
            <i class="fa-solid fa-trash"></i> Delete Selected
          </button>
        </div>
      </div>
      <div id="loadList" style="display:flex;flex-direction:column;gap:0.6rem">
        <div style="color:var(--secondary);font-size:0.9rem;padding:1rem;text-align:center" id="loadListEmpty">No loads configured yet. Add one above or use analysis.</div>
      </div>
    </div>

  </div><!-- /loadsPane -->

  <!-- ═══════════════════════════ ANALYTICS PANE ══════════════════════════ -->
  <div id="analyticsPane" style="display:none;">

    <header class="top-bar">
      <div class="top-left">
        <div class="system-status"><div class="status-dot"></div> Live · updating every 2s</div>
        <div class="current-load-title">Analytics</div>
      </div>
      <div class="top-right">
        <div class="clock" id="liveClockAnalytics">00:00:00</div>
      </div>
    </header>

    <!-- Time window tabs -->
    <div style="display:flex;gap:0.5rem;margin-bottom:1.5rem">
      <button class="btn-primary" id="winTab60"  onclick="setAnalyticsWindow(60)"  style="font-size:0.8rem;padding:0.45rem 1rem;opacity:1">1 min</button>
      <button class="btn-primary" id="winTab300" onclick="setAnalyticsWindow(300)" style="font-size:0.8rem;padding:0.45rem 1rem;opacity:0.45">5 min</button>
      <button class="btn-primary" id="winTab900" onclick="setAnalyticsWindow(900)" style="font-size:0.8rem;padding:0.45rem 1rem;opacity:0.45">15 min</button>
      <button class="btn-primary" id="winTab3600" onclick="setAnalyticsWindow(3600)" style="font-size:0.8rem;padding:0.45rem 1rem;opacity:0.45">1 hr</button>
    </div>

    <!-- KPI strip -->
    <div class="analytics-kpi-grid">
      <div class="kpi-card"><div class="kpi-value"><span id="an_kv_V">—</span><span class="kpi-unit">V</span></div><div class="kpi-label">Avg Voltage</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="an_kv_P">—</span><span class="kpi-unit">W</span></div><div class="kpi-label">Peak Power</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="an_kv_PF">—</span></div><div class="kpi-label">Avg Power Factor</div></div>
      <div class="kpi-card"><div class="kpi-value"><span id="an_kv_KWH">—</span><span class="kpi-unit">kWh</span></div><div class="kpi-label">Session Energy</div></div>
    </div>

    <!-- 2-column grid: V, I, P, S, Q, PF -->
    <div class="analytics-charts-grid">

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Voltage (V)</div><div style="font-size:0.78rem;color:var(--secondary)">Mains RMS voltage</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_V">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cV"></canvas></div>
      </div>

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Current (A)</div><div style="font-size:0.78rem;color:var(--secondary)">RMS current draw</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_I">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cI"></canvas></div>
      </div>

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Real Power (W)</div><div style="font-size:0.78rem;color:var(--secondary)">Active power consumed</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_P">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cP"></canvas></div>
      </div>

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Apparent Power (VA)</div><div style="font-size:0.78rem;color:var(--secondary)">Total volt-ampere load</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_S">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cS"></canvas></div>
      </div>

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Reactive Power (VAr)</div><div style="font-size:0.78rem;color:var(--secondary)">Non-working component</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_Q">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cQ"></canvas></div>
      </div>

      <div class="card-container graph-card" onclick="toggleFullscreen(this)">
        <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
          <div><div class="section-header" style="margin-bottom:0">Power Factor</div><div style="font-size:0.78rem;color:var(--secondary)">cos φ · 1.0 = ideal</div></div>
          <span style="font-size:1.3rem;font-weight:700" id="an_hd_PF">—</span>
        </div>
        <div class="canvas-wrapper" style="position:relative;height:150px"><canvas id="an_cPF"></canvas></div>
      </div>

    </div>

    <!-- Full-width: Distortion -->
    <div class="card-container graph-card" onclick="toggleFullscreen(this)" style="margin-bottom:1.5rem">
      <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
      <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1rem">
        <div><div class="section-header" style="margin-bottom:0">Distortion Index (%)</div><div style="font-size:0.78rem;color:var(--secondary)">THD proxy — Q/S ratio over time</div></div>
        <span style="font-size:1.3rem;font-weight:700" id="an_hd_DI">—</span>
      </div>
      <div class="canvas-wrapper" style="position:relative;height:200px"><canvas id="an_cDI"></canvas></div>
    </div>

    <!-- Full-width: stacked energy -->
    <div class="card-container graph-card" onclick="toggleFullscreen(this)" style="margin-bottom:1.5rem">
      <div class="close-fullscreen" onclick="event.stopPropagation();closeFullscreen()"><i class="fa-solid fa-xmark"></i></div>
      <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:0.75rem">
        <div><div class="section-header" style="margin-bottom:0">Session Energy Breakdown (kWh)</div><div style="font-size:0.78rem;color:var(--secondary)">Mobile · Laptop · Other — stacked cumulative</div></div>
        <span style="font-size:1.3rem;font-weight:700" id="an_hd_KWH">—</span>
      </div>
      <div style="display:flex;gap:1.25rem;margin-bottom:0.75rem">
        <span style="display:flex;align-items:center;gap:5px;font-size:0.78rem;color:var(--secondary)"><span style="width:10px;height:10px;border-radius:2px;background:#818cf8;display:inline-block"></span>Mobile</span>
        <span style="display:flex;align-items:center;gap:5px;font-size:0.78rem;color:var(--secondary)"><span style="width:10px;height:10px;border-radius:2px;background:#34d399;display:inline-block"></span>Laptop</span>
        <span style="display:flex;align-items:center;gap:5px;font-size:0.78rem;color:var(--secondary)"><span style="width:10px;height:10px;border-radius:2px;background:#f87171;display:inline-block"></span>Other</span>
      </div>
      <div class="canvas-wrapper" style="position:relative;height:200px"><canvas id="an_cKWH"></canvas></div>
    </div>

  </div><!-- /analyticsPane -->

  <!-- ═══════════════════════════ BILLING / ADMIN PANE ═══════════════════════════ -->
  <div id="billingPane" style="display:none;">
    <header class="top-bar">
      <div class="top-left">
        <div class="system-status"><div class="status-dot"></div> Billing & Admin</div>
        <div style="margin-top:0.5rem;color:#c2410c;">Admin must login to control timer/lock</div>
      </div>
      <div class="top-right">
        <button class="btn-primary btn-dark" onclick="adminLogin()" style="font-size:0.85rem;padding:0.45rem 0.8rem;">Admin Login</button>
        <button class="btn-primary" onclick="adminLogout()" style="font-size:0.85rem;padding:0.45rem 0.8rem;">Logout</button>
      </div>
    </header>
    
    <!-- Current Timer Status Card -->
    <div class="card-container" style="margin-bottom:1.5rem; background: linear-gradient(135deg, var(--card-bg) 0%, rgba(192,133,82,0.05) 100%);">
      <div class="section-header" style="display: flex; align-items: center; gap: 0.5rem;">
        <i class="fa-solid fa-hourglass-half"></i> Current Timer Status
      </div>
      <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 1rem; margin-top: 1rem;">
        <div style="text-align: center; padding: 1rem; background: var(--bg-color); border-radius: 8px;">
          <div style="font-size: 0.85rem; color: var(--secondary);">Admin Lock</div>
          <div style="font-size: 1.5rem; font-weight: 700; margin-top: 0.5rem;" id="adminLockStatusDisplay">—</div>
        </div>
        <div style="text-align: center; padding: 1rem; background: var(--bg-color); border-radius: 8px;">
          <div style="font-size: 0.85rem; color: var(--secondary);">Timer Status</div>
          <div style="font-size: 1.5rem; font-weight: 700; margin-top: 0.5rem;" id="timerStatusDisplay">—</div>
        </div>
        <div style="text-align: center; padding: 1rem; background: var(--bg-color); border-radius: 8px;">
          <div style="font-size: 0.85rem; color: var(--secondary);">Remaining Time</div>
          <div style="font-size: 1.5rem; font-weight: 700; margin-top: 0.5rem; font-family: monospace;" id="remainingTimeDisplay">00:00:00</div>
        </div>
      </div>
      <div style="margin-top: 1rem; padding: 0.75rem; background: var(--bg-color); border-radius: 8px;">
        <div style="font-size: 0.85rem; color: var(--secondary);">Current Session</div>
        <div style="display: flex; justify-content: space-between; margin-top: 0.5rem;">
          <span>Energy Used: <strong id="sessionEnergyUsed">0.000</strong> kWh</span>
          <span>Est. Cost: <strong id="sessionCostDisplay">₹0.00</strong></span>
        </div>
      </div>
    </div>
    
    <div class="card-container" style="margin-bottom:1.5rem">
      <div class="section-header">Admin Controls</div>
      <div style="display:flex;gap:1rem;flex-wrap:wrap;">
        <button class="btn-primary btn-red" onclick="adminLock()" style="font-size:0.9rem;padding:0.6rem 1rem;"><i class="fa-solid fa-lock"></i> Lock Meter</button>
        <button class="btn-primary btn-green" onclick="adminUnlock()" style="font-size:0.9rem;padding:0.6rem 1rem;"><i class="fa-solid fa-lock-open"></i> Unlock Meter</button>
        <button class="btn-primary" onclick="adminPauseTimer()" style="font-size:0.9rem;padding:0.6rem 1rem;"><i class="fa-solid fa-pause"></i> Pause Timer</button>
        <button class="btn-primary btn-green" onclick="adminResumeTimer()" style="font-size:0.9rem;padding:0.6rem 1rem;"><i class="fa-solid fa-play"></i> Resume Timer</button>
      </div>
      <div style="margin-top:1rem;font-size:0.9rem;color:var(--secondary);">
        <div>Admin Lock: <span id="adminLockStatus">Unknown</span></div>
        <div>Timer Running: <span id="timerStatus">Unknown</span></div>
        <div>Remaining Time: <span id="remainingTime">Unknown</span></div>
      </div>
    </div>
    
    <div class="card-container" style="margin-bottom:1.5rem">
      <div class="section-header">Set Timer Duration</div>
      <div style="display:flex;gap:0.75rem;flex-wrap:wrap;margin-bottom:1rem">
        <button class="btn-primary" onclick="adminSetDuration(60)" style="font-size:0.85rem;padding:0.5rem 0.9rem">1 min</button>
        <button class="btn-primary" onclick="adminSetDuration(300)" style="font-size:0.85rem;padding:0.5rem 0.9rem">5 min</button>
        <button class="btn-primary" onclick="adminSetDuration(900)" style="font-size:0.85rem;padding:0.5rem 0.9rem">15 min</button>
        <button class="btn-primary" onclick="adminSetDuration(3600)" style="font-size:0.85rem;padding:0.5rem 0.9rem">1 hour</button>
        <button class="btn-primary" onclick="adminSetDuration(86400)" style="font-size:0.85rem;padding:0.5rem 0.9rem">1 Day</button>
        <button class="btn-primary" onclick="adminSetDuration(604800)" style="font-size:0.85rem;padding:0.5rem 0.9rem">1 Week</button>
        <button class="btn-primary" onclick="adminSetDuration(2592000)" style="font-size:0.85rem;padding:0.5rem 0.9rem">1 Month</button>
        <button class="btn-primary" onclick="adminSetDuration(7776000)" style="font-size:0.85rem;padding:0.5rem 0.9rem">3 Months</button>
      </div>
      
      <div style="margin-top: 1rem; padding-top: 1rem; border-top: 1px solid rgba(140,90,60,0.1);">
        <div class="section-header" style="font-size: 0.95rem;">Custom Duration</div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;align-items:center; margin-top: 0.75rem;">
          <div style="display: flex; flex-direction: column; gap: 0.25rem;">
            <label style="font-size: 0.7rem; color: var(--secondary);">Days</label>
            <input id="ct_d" type="number" min="0" max="365" placeholder="0" style="width:70px;padding:0.6rem;border:1px solid rgba(140,90,60,0.25);border-radius:6px;">
          </div>
          <div style="display: flex; flex-direction: column; gap: 0.25rem;">
            <label style="font-size: 0.7rem; color: var(--secondary);">Hours</label>
            <input id="ct_h" type="number" min="0" max="23" placeholder="0" style="width:65px;padding:0.6rem;border:1px solid rgba(140,90,60,0.25);border-radius:6px;">
          </div>
          <div style="display: flex; flex-direction: column; gap: 0.25rem;">
            <label style="font-size: 0.7rem; color: var(--secondary);">Minutes</label>
            <input id="ct_m" type="number" min="0" max="59" placeholder="0" style="width:65px;padding:0.6rem;border:1px solid rgba(140,90,60,0.25);border-radius:6px;">
          </div>
          <div style="display: flex; flex-direction: column; gap: 0.25rem;">
            <label style="font-size: 0.7rem; color: var(--secondary);">Seconds</label>
            <input id="ct_s" type="number" min="0" max="59" placeholder="0" style="width:65px;padding:0.6rem;border:1px solid rgba(140,90,60,0.25);border-radius:6px;">
          </div>
          <button class="btn-primary btn-green" onclick="adminApplyCustomTimer()" style="padding:0.6rem 1.2rem; margin-top: 1.2rem;">
            <i class="fa-solid fa-plus-circle"></i> Add Time
          </button>
          <button class="btn-primary btn-red" onclick="adminSetTimerZero()" style="padding:0.6rem 1.2rem; margin-top: 1.2rem;">
            <i class="fa-solid fa-stop"></i> Stop Timer
          </button>
        </div>
        <div style="margin-top: 0.75rem; font-size: 0.8rem; color: var(--secondary);">
          <i class="fa-solid fa-info-circle"></i> Custom duration adds time to existing remaining time
        </div>
      </div>
    </div>
    
    <div class="card-container">
      <div class="section-header">Billing Records</div>
      <div style="max-height: 300px; overflow-y: auto;">
        <table style="width:100%;">
          <thead>
            <tr><th>Duration</th><th>Energy Used</th><th>Cost (₹)</th><th>Date/Time</th></tr>
          </thead>
          <tbody id="billingRecordsTbody">
            <tr><td colspan="4">No records</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </div><!-- /billingPane -->

  <!-- Admin Login Modal -->
  <div id="adminModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.5);z-index:1000;display:flex;align-items:center;justify-content:center;">
    <div style="background:var(--card-bg);padding:2rem;border-radius:16px;width:320px;box-shadow:0 20px 60px rgba(0,0,0,0.3)">
      <div style="font-size:1.3rem;font-weight:700;margin-bottom:0.5rem">Admin Login</div>
      <div style="font-size:0.85rem;color:var(--secondary);margin-bottom:1.5rem">Enter credentials to access billing controls</div>
      <input id="adminUser" type="text" placeholder="Username" style="width:100%;padding:0.75rem;margin-bottom:0.75rem;border:1.5px solid rgba(140,90,60,0.3);border-radius:8px;font-size:1rem;outline:none;box-sizing:border-box">
      <input id="adminPass" type="password" placeholder="Password" style="width:100%;padding:0.75rem;margin-bottom:1rem;border:1.5px solid rgba(140,90,60,0.3);border-radius:8px;font-size:1rem;outline:none;box-sizing:border-box" onkeydown="if(event.key==='Enter')submitAdminLogin()">
      <div id="adminLoginError" style="color:#ef4444;font-size:0.85rem;margin-bottom:0.75rem;display:none">Invalid credentials</div>
      <div style="display:flex;gap:0.75rem">
        <button class="btn-primary btn-green" onclick="submitAdminLogin()" style="flex:1">Login</button>
        <button class="btn-primary btn-dark" onclick="closeAdminModal()" style="flex:1">Cancel</button>
      </div>
    </div>
  </div>

  <!-- Blocked Modal -->
  <div id="blockedModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.5);z-index:999;align-items:center;justify-content:center;">
    <div style="background:var(--card-bg);padding:2rem;border-radius:16px;width:300px;text-align:center">
      <div style="font-size:2.5rem;margin-bottom:1rem">🔒</div>
      <div id="blockedModalTitle" style="font-size:1.2rem;font-weight:700;margin-bottom:0.5rem">Blocked</div>
      <div id="blockedModalMsg" style="font-size:0.9rem;color:var(--secondary);margin-bottom:1.5rem">—</div>
      <button class="btn-primary" onclick="document.getElementById('blockedModal').style.display='none'">OK</button>
    </div>
  </div>

  <div id="graphOverlay" class="graph-overlay" onclick="closeFullscreen()"></div>
</main>

<script>
var systemData={
  v:0,i:0,s:0,p:0,q:0,di:0,pf:0,kwh:0,cost:0,relay:false,
  dev:"-",lt:"-",dynamic:"-",dynamicLoads:[],
  age_ms:9999,
  billing_cost_per_hour: 5.0
};
var isAdmin=false;
var relayCmdLockUntil = 0;
var analysisPolling=null;
var loadEnergyMap = {};
var lastEnergyUpdateTime = Date.now();

window.onerror = function(message, source, lineno, colno, error) {
  console.error('JS ERROR:', message, 'at', source + ':' + lineno + ':' + colno, error);
};

function renderDynamicLoadBreakdown(){
  var container = document.getElementById('energyBreakdown');
  if(!container) return;
  container.innerHTML = '';

  var loads = systemData.dynamicLoads || [];
  if(loads.length === 0){
    container.innerHTML = '<div style="color:var(--secondary)">No configured dynamic loads.</div>';
    return;
  }

  var totalEnergy = systemData.kwh || 0;
  if(totalEnergy <= 0){
    container.innerHTML = '<div style="color:var(--secondary)">Waiting for actual energy data from backend…</div>';
    return;
  }

  loads.forEach(function(load){
    var loadName = load.name || 'Unnamed';
    var energy = 0;
    var pct = 0;
    var isActive = (systemData.dynamic && systemData.dynamic === loadName);

    var segment =
      '<div class="breakdown-row" style="margin-bottom:0.3rem;align-items:flex-start">' +
        '<span class="breakdown-label" style="font-weight:600">'+loadName+'</span>' +
        '<span class="breakdown-val">'+energy.toFixed(3)+' kWh '+(isActive?'<span style="color:#22c55e;font-size:0.8rem">(active)</span>':'')+'</span>' +
      '</div>' +
      '<div class="progress-bg"><div class="progress-bar" style="width:'+pct+'%;background:'+(isActive?'#22c55e':'#C08552')+'"></div></div>';

    container.innerHTML += segment;
  });
}

setInterval(function(){
  var t=new Date().toLocaleTimeString();
  document.getElementById('liveClock').innerText=t;
  var el=document.getElementById('liveClockLoads');
  if(el) el.innerText=t;
  var el2=document.getElementById('liveClockAnalytics');
  if(el2) el2.innerText=t;
},1000);

function updateBillingTimerDisplay() {
  var remainingSec = systemData.billing_remaining_sec || 0;
  var timerRunning = systemData.billing_timer_running;
  var adminLock = systemData.billing_admin_lock;
  
  // Update dashboard status
  var billingMessageEl = document.getElementById('billingStatus');
  if (billingMessageEl) {
    if (adminLock) {
      billingMessageEl.innerText = '🔒 Admin locked - Meter OFF';
      billingMessageEl.style.color = '#ef4444';
    } else if (remainingSec > 0 && timerRunning) {
      var hours = Math.floor(remainingSec / 3600);
      var minutes = Math.floor((remainingSec % 3600) / 60);
      var seconds = remainingSec % 60;
      billingMessageEl.innerText = `⏱ ${hours.toString().padStart(2,'0')}:${minutes.toString().padStart(2,'0')}:${seconds.toString().padStart(2,'0')} remaining${systemData.relay ? '' : ' · paused'}`;
      billingMessageEl.style.color = remainingSec < 3600 ? '#ef4444' : (remainingSec < 86400 ? '#f59e0b' : '#22c55e');
    } else if (remainingSec === 0 && !timerRunning) {
      billingMessageEl.innerText = '⚠ Credit expired — Please top-up';
      billingMessageEl.style.color = '#ef4444';
    } else {
      billingMessageEl.innerText = '⏸ Timer stopped';
      billingMessageEl.style.color = '#f59e0b';
    }
  }
  
  // Update billing pane status displays
  var adminLockStatusEl = document.getElementById('adminLockStatus');
  if (adminLockStatusEl) {
    adminLockStatusEl.innerText = adminLock ? 'ENABLED' : 'DISABLED';
    adminLockStatusEl.style.color = adminLock ? '#ef4444' : '#22c55e';
  }
  
  var adminLockStatusDisplayEl = document.getElementById('adminLockStatusDisplay');
  if (adminLockStatusDisplayEl) {
    adminLockStatusDisplayEl.innerText = adminLock ? '🔒 Locked' : '🔓 Unlocked';
    adminLockStatusDisplayEl.style.color = adminLock ? '#ef4444' : '#22c55e';
  }
  
  var timerStatusEl = document.getElementById('timerStatus');
  if (timerStatusEl) {
    timerStatusEl.innerText = timerRunning ? '▶ RUNNING' : '⏸ STOPPED';
    timerStatusEl.style.color = timerRunning ? '#22c55e' : '#ef4444';
  }
  
  var timerStatusDisplayEl = document.getElementById('timerStatusDisplay');
  if (timerStatusDisplayEl) {
    timerStatusDisplayEl.innerText = timerRunning ? 'Active' : 'Inactive';
    timerStatusDisplayEl.style.color = timerRunning ? '#22c55e' : '#ef4444';
  }
  
  var remainingTimeEl = document.getElementById('remainingTime');
  if (remainingTimeEl) {
    if (remainingSec > 0) {
      var hours = Math.floor(remainingSec / 3600);
      var minutes = Math.floor((remainingSec % 3600) / 60);
      var seconds = remainingSec % 60;
      remainingTimeEl.innerText = hours.toString().padStart(2, '0') + ':' + 
                                 minutes.toString().padStart(2, '0') + ':' + 
                                 seconds.toString().padStart(2, '0');
      remainingTimeEl.style.color = remainingSec < 300 ? '#ef4444' : (remainingSec < 1800 ? '#f59e0b' : '#22c55e');
    } else {
      remainingTimeEl.innerText = '00:00:00';
      remainingTimeEl.style.color = '#ef4444';
    }
  }
  
  var remainingTimeDisplayEl = document.getElementById('remainingTimeDisplay');
  if (remainingTimeDisplayEl) {
    if (remainingSec > 0) {
      var hours = Math.floor(remainingSec / 3600);
      var minutes = Math.floor((remainingSec % 3600) / 60);
      var seconds = remainingSec % 60;
      remainingTimeDisplayEl.innerText = hours.toString().padStart(2, '0') + ':' + 
                                        minutes.toString().padStart(2, '0') + ':' + 
                                        seconds.toString().padStart(2, '0');
      remainingTimeDisplayEl.style.color = remainingSec < 300 ? '#ef4444' : (remainingSec < 1800 ? '#f59e0b' : '#22c55e');
    } else {
      remainingTimeDisplayEl.innerText = '00:00:00';
      remainingTimeDisplayEl.style.color = '#ef4444';
    }
  }
  
  // Update session info
  var sessionEnergyEl = document.getElementById('sessionEnergyUsed');
  if (sessionEnergyEl) {
    sessionEnergyEl.innerText = systemData.kwh.toFixed(3);
  }
  
  var sessionCostDisplayEl = document.getElementById('sessionCostDisplay');
  if (sessionCostDisplayEl) {
    sessionCostDisplayEl.innerText = '₹' + systemData.cost.toFixed(2);
  }
}

async function fetchData(){
  try{
    var r=await fetch('/api/data');
    var d=await r.json();
    var wasActive = systemData.billing_remaining_sec > 0;
    var relayLocked = Date.now() < relayCmdLockUntil;
    var savedRelay = systemData.relay;
    var previousRemaining = systemData.billing_remaining_sec;
    
    function hasChanged(oldData, newData){
      return Math.abs(oldData.p - newData.p) > 1 ||
             Math.abs(oldData.v - newData.v) > 1 ||
             Math.abs(oldData.i - newData.i) > 0.02 ||
             oldData.billing_remaining_sec !== newData.billing_remaining_sec ||
             oldData.billing_cost_per_hour !== newData.billing_cost_per_hour;
    }

    if (hasChanged(systemData, d)) {
      Object.assign(systemData, d);
      if (relayLocked) systemData.relay = savedRelay;
      updateUI();
    } else {
      // Apply at least the relay lock state even if values are steady
      if (relayLocked) systemData.relay = savedRelay;
      // Still update timer display even if other values haven't changed
      updateBillingTimerDisplay();
    }
    
    // Always update cost preview after fetch, especially if cost per hour changed
    updateTopupCostPreview();

    // Show notification when timer expires
    if (wasActive && systemData.billing_remaining_sec === 0) {
      showBlocked('Top-up Ended', 'Your prepaid credit has run out. Please top-up to continue.');
      // Also force relay UI update
      document.getElementById('mainRelay').checked = false;
    }

    safeChartUpdate();
  }catch(e){ console.error('Fetch error:', e); }
}

let lastChartUpdate = 0;
function safeChartUpdate(){
  if(Date.now() - lastChartUpdate > 3000){
    updateAnalyticsCharts();
    lastChartUpdate = Date.now();
  }
}

function updateUI(){
  document.getElementById('val_V').innerText=systemData.v.toFixed(1);
  document.getElementById('val_I').innerText=systemData.i.toFixed(2);
  document.getElementById('val_P').innerText=Math.round(systemData.p);
  document.getElementById('val_S').innerText=Math.round(systemData.s);
  document.getElementById('val_Q').innerText=Math.round(systemData.q);
  document.getElementById('val_PF').innerText=systemData.pf.toFixed(2);
  document.getElementById('bd_P').innerText=Math.round(systemData.p)+' W';
  document.getElementById('bd_Q').innerText=Math.round(systemData.q)+' VAr';
  document.getElementById('bd_DI').innerText=systemData.di.toFixed(1)+'%';
  var mx=500;
  document.getElementById('bar_P').style.width=Math.min(systemData.p/mx*100,100)+'%';
  document.getElementById('bar_Q').style.width=Math.min(systemData.q/mx*100,100)+'%';
  renderDynamicLoadBreakdown();
  // Determine best display name: prefer dynamic match, then Uno's name, clean up unknowns
  var rawDev = systemData.dev || '';
  var displayDev;
  if (systemData.dynamic && systemData.dynamic !== '-' && systemData.dynamic !== '') {
    displayDev = systemData.dynamic;                          // Dynamic match wins
  } else if (rawDev.startsWith('?')) {
    displayDev = 'Unknown (' + rawDev.substring(1) + ' VA)'; // Clean up "?42" → "Unknown (42 VA)"
  } else if (rawDev === 'NO_LOAD' || rawDev === '') {
    displayDev = 'No load';
  } else if (rawDev === 'MAINS_OFF') {
    displayDev = 'Mains off';
  } else {
    displayDev = rawDev;                                       // mobile / laptop etc.
  }
  document.getElementById('activeLoadType').innerText = displayDev;
  document.getElementById('dynamicLoad').innerText = systemData.dynamic && systemData.dynamic !== '-'
    ? 'Matched: ' + systemData.dynamic
    : (rawDev.startsWith('?') ? 'No match in load list' : 'Signature: ' + rawDev);
  var op=systemData.relay&&systemData.p>50;
  document.getElementById('activeLoadState').innerText=op?'OPERATING':(systemData.relay?'IDLE':'OFFLINE');
  document.getElementById('activeLoadState').className='badge'+(op?' active':'');
  document.getElementById('sessionKwh').innerText=systemData.kwh.toFixed(3)+' kWh';
  document.getElementById('estCost').innerText='₹'+systemData.cost.toFixed(2);
  var rt=document.getElementById('relayStatusText');
  var rb=document.getElementById('relayBadge');
  var ri=document.getElementById('relayStatusIcon');
  if(systemData.relay){rt.innerText=op?'OPERATING':'STANDBY';rb.innerText='ON';rb.className='status-badge on';ri.innerHTML='<i class="fa-solid fa-circle-check" style="color:#22c55e"></i>';}
  else{rt.innerText='OFFLINE';rb.innerText='OFF';rb.className='status-badge';ri.innerHTML='<i class="fa-solid fa-circle-xmark" style="color:#ef4444"></i>';}
  document.getElementById('mainRelay').checked=systemData.relay;
  var relayAllowed = !systemData.billing_admin_lock && systemData.billing_remaining_sec > 0;
  document.getElementById('mainRelay').disabled = !relayAllowed;
  var relayContainer = document.getElementById('mainRelay').parentElement;
  if (relayContainer) relayContainer.style.opacity = relayAllowed ? '1' : '0.4';
  
  // Update billing timer display
  updateBillingTimerDisplay();
  document.getElementById('total_kwh').innerText=systemData.kwh.toFixed(6);
  document.getElementById('session_cost').innerText='₹'+systemData.cost.toFixed(3);
  document.getElementById('avg_pf').innerText=systemData.pf.toFixed(2);
  document.getElementById('data_age').innerText=(systemData.age_ms/1000).toFixed(1)+'s';
  var dot=document.querySelector('.status-dot');
  dot.style.boxShadow=systemData.age_ms<4000?'0 0 10px #22c55e80':'none';
  
  // Update top-up cost preview
  updateTopupCostPreview();
}

function formatDaysHours(seconds) {
  var days = Math.floor(seconds / 86400);
  var hours = Math.floor((seconds % 86400) / 3600);
  var minutes = Math.floor((seconds % 3600) / 60);
  
  if (days > 0) return days + 'd ' + hours + 'h';
  if (hours > 0) return hours + 'h ' + minutes + 'm';
  if (minutes > 0) return minutes + 'm';
  return '< 1 minute';
}

function formatTime(sec){
  let h = Math.floor(sec / 3600);
  let m = Math.floor((sec % 3600) / 60);
  let s = sec % 60;
  return `${h}h ${m}m ${s}s`;
}

function setActivePane(pane){
  var dp = document.getElementById('dashboardPane');
  var lp = document.getElementById('loadsPane');
  var ap = document.getElementById('analyticsPane');
  var bp = document.getElementById('billingPane');
  var dn = document.getElementById('dashboardNavBtn');
  var ln = document.getElementById('loadsNavBtn');
  var an = document.getElementById('analyticsNavBtn');
  var bn = document.getElementById('billingNavBtn');

  if (!dp || !lp || !ap || !bp || !dn || !ln || !an || !bn) {
    console.error('setActivePane: missing required pane or nav elements');
    return;
  }

  dp.style.display = 'none';
  lp.style.display = 'none';
  ap.style.display = 'none';
  bp.style.display = 'none';

  dn.classList.remove('active');
  ln.classList.remove('active');
  an.classList.remove('active');
  bn.classList.remove('active');

  if (pane === 'dashboard') {
    dp.style.display = 'block';
    dn.classList.add('active');
  }
  if (pane === 'loads') {
    lp.style.display = 'block';
    ln.classList.add('active');
    fetchLoads();
  }
  if (pane === 'analytics') {
    ap.style.display = 'block';
    an.classList.add('active');
    if (typeof Chart !== 'undefined') {
      initAnalyticsCharts();
    } else {
      loadChartJs();
    }
  }
  if (pane === 'billing') {
    if (!isAdmin) {
      adminLogin();
      return;
    }
    bp.style.display = 'block';
    bn.classList.add('active');
    refreshBillingRecords();
  }
}

async function fetchLoads(){
  try{
    var r=await fetch('/api/loads');
    var d=await r.json();
    systemData.dynamicLoads = Array.isArray(d.loads) ? d.loads : [];
    renderDynamicLoadBreakdown();
    renderLoadList(d.loads);
    updateAnalysisBanner(d);
    // Update live range display during capture
    var rangeEl=document.getElementById('analysisLiveRange');
    if(d.analysisActive){
      rangeEl.innerText='Capturing… Min: '+d.analysisMinVA.toFixed(1)+' W  Max: '+d.analysisMaxVA.toFixed(1)+' W  Now: '+d.analysisValue.toFixed(1)+' W';
    } else {
      rangeEl.innerText='Range: —';
    }
  }catch(e){console.error(e);}
}

function renderLoadList(loads){
  var list=document.getElementById('loadList');
  var empty=document.getElementById('loadListEmpty');
  if(!loads||loads.length===0){
    list.innerHTML='';
    list.appendChild(empty);
    empty.style.display='block';
    return;
  }
  empty.style.display='none';
  // Preserve existing rows to avoid losing focus while editing
  var existingIds=Array.from(list.querySelectorAll('[data-load-id]')).map(function(el){return parseInt(el.getAttribute('data-load-id'));});
  var newIds=loads.map(function(l){return l.id;});
  // Remove rows no longer present
  existingIds.forEach(function(id){
    if(newIds.indexOf(id)<0){var el=list.querySelector('[data-load-id="'+id+'"]');if(el)el.remove();}
  });
  loads.forEach(function(load){
    var existing=list.querySelector('[data-load-id="'+load.id+'"]');
    if(existing){
      // Only update values if none of the inputs in this row are focused
      var focused=existing.querySelector(':focus');
      if(!focused){
        existing.querySelector('.ln').value=load.name;
        existing.querySelector('.lmin').value=load.minW.toFixed(1);
        existing.querySelector('.lmax').value=load.maxW.toFixed(1);
      }
      return;
    }
    var row=document.createElement('div');
    row.className='load-row';
    row.setAttribute('data-load-id',load.id);
    row.innerHTML=
      '<input type="checkbox" class="loadCheckbox" value="'+load.id+'" style="flex-shrink:0;width:18px;height:18px;accent-color:var(--accent);cursor:pointer"/>'+
      '<input type="text" class="ln" value="'+escHtml(load.name)+'" placeholder="Name" onchange="saveLoad('+load.id+',this)" style="flex:2;min-width:90px;padding:0.5rem 0.7rem;border:1.5px solid rgba(140,90,60,0.25);border-radius:6px;font-size:0.9rem;background:var(--bg-color);color:var(--primary-text);outline:none"/>'+
      '<div style="display:flex;align-items:center;gap:0.4rem;flex-shrink:0">'+
        '<span class="load-label">Min&nbsp;W</span>'+
        '<input type="number" step="0.1" class="lmin" value="'+load.minW.toFixed(1)+'" onchange="saveLoad('+load.id+',this)" style="width:80px;padding:0.5rem 0.5rem;border:1.5px solid rgba(140,90,60,0.25);border-radius:6px;font-size:0.9rem;background:var(--bg-color);color:var(--primary-text);outline:none"/>'+
      '</div>'+
      '<div style="display:flex;align-items:center;gap:0.4rem;flex-shrink:0">'+
        '<span class="load-label">Max&nbsp;W</span>'+
        '<input type="number" step="0.1" class="lmax" value="'+load.maxW.toFixed(1)+'" onchange="saveLoad('+load.id+',this)" style="width:80px;padding:0.5rem 0.5rem;border:1.5px solid rgba(140,90,60,0.25);border-radius:6px;font-size:0.9rem;background:var(--bg-color);color:var(--primary-text);outline:none"/>'+
      '</div>'+
      '<button onclick="deleteLoads(\''+load.id+'\')" style="flex-shrink:0;background:#fee2e2;color:#b91c1c;border:none;border-radius:6px;padding:0.45rem 0.75rem;cursor:pointer;font-size:0.85rem;font-weight:600" title="Delete"><i class="fa-solid fa-trash-can"></i></button>';
    list.appendChild(row);
  });
}

function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}

function saveLoad(id,changedInput){
  var row=changedInput.closest('[data-load-id]');
  var name=row.querySelector('.ln').value.trim();
  var minW=row.querySelector('.lmin').value;
  var maxW=row.querySelector('.lmax').value;
  if(!name||minW===''||maxW==='') return;
  fetch('/api/loads/update?id='+id+'&name='+encodeURIComponent(name)+'&min='+encodeURIComponent(minW)+'&max='+encodeURIComponent(maxW));
}

function updateAnalysisBanner(d){
  var banner=document.getElementById('analysisBanner');
  var dot=document.getElementById('analysisDot');
  var title=document.getElementById('analysisBannerTitle');
  var sub=document.getElementById('analysisBannerSub');
  if(d.analysisActive){
    banner.className='analysis-banner';
    dot.className='pulse-dot';
    title.innerText='Capturing in progress…';
    sub.innerText='Min: '+d.analysisMinVA.toFixed(1)+' W   Max: '+d.analysisMaxVA.toFixed(1)+' W   Current: '+d.analysisValue.toFixed(1)+' W';
  } else {
    banner.className='analysis-banner idle';
    dot.className='pulse-dot idle';
    title.innerText='Analysis idle';
    sub.innerText='Type a name then press Start Analysis to begin capturing';
  }
}

async function addLoad(){
  var name=document.getElementById('manualName').value.trim();
  var min=document.getElementById('manualMin').value;
  var max=document.getElementById('manualMax').value;
  if(!name||min===''||max===''){alert('Please fill in all fields.');return;}
  await fetch('/api/loads/add?name='+encodeURIComponent(name)+'&min='+encodeURIComponent(min)+'&max='+encodeURIComponent(max));
  document.getElementById('manualName').value='';
  document.getElementById('manualMin').value='';
  document.getElementById('manualMax').value='';
  await fetchLoads();
}

async function deleteLoads(id){
  if(!confirm('Delete this load?')) return;
  await fetch('/api/loads/delete?id='+id);
  await fetchLoads();
}

async function deleteSelectedLoads(){
  var sel=Array.from(document.querySelectorAll('.loadCheckbox:checked')).map(function(el){return el.value;});
  if(sel.length===0){alert('No loads selected.');return;}
  if(!confirm('Delete '+sel.length+' load(s)?')) return;
  await fetch('/api/loads/delete?id='+encodeURIComponent(sel.join(',')));
  await fetchLoads();
}

function toggleAllLoads(checked){
  document.querySelectorAll('.loadCheckbox').forEach(function(cb){cb.checked=checked;});
}

async function startAnalysis(){
  var name=document.getElementById('captureNameInput').value.trim();
  if(!name){document.getElementById('captureNameInput').focus();document.getElementById('captureNameInput').style.borderColor='#ef4444';setTimeout(function(){document.getElementById('captureNameInput').style.borderColor='';},2000);return;}
  await fetch('/api/analysis/start?name='+encodeURIComponent(name));
  await fetchLoads(); // Update banner immediately to show capturing
  // Fast polling during capture
  if(analysisPolling) clearInterval(analysisPolling);
  analysisPolling=setInterval(fetchLoads,1500);
}

async function stopAnalysis(){
  var name=document.getElementById('captureNameInput').value.trim()||'Analysis';
  var response = await fetch('/api/analysis/stop?name='+encodeURIComponent(name));
  var result = await response.json();
  if(!result.ok){
    alert('No load detected during analysis. Make sure the device is plugged in and drawing at least 0.5W, and wait a few seconds after starting.');
  }
  if(analysisPolling){clearInterval(analysisPolling);analysisPolling=null;}
  document.getElementById('captureNameInput').value='';
  await fetchLoads();
}

async function relayCmd(cmd){
  // Check admin lock first (frontend check for immediate feedback)
  if(systemData.billing_admin_lock){
    document.getElementById('mainRelay').checked = systemData.relay;
    showBlocked('Admin Has Disabled','The meter has been turned off by the administrator. Please contact the admin to restore access.');
    return;
  }
  
  // Check if timer is running and has credit
  if(systemData.billing_remaining_sec === 0 || !systemData.billing_timer_running){
    document.getElementById('mainRelay').checked = systemData.relay;
    showBlocked('Top-up Ended','Your prepaid credit has expired. Please top-up from the dashboard to continue using the meter.');
    return;
  }
  
  relayCmdLockUntil = Date.now() + 2500; // suppress fetchData UI updates for 2.5s
  
  try {
    var response = await fetch('/relay/' + (typeof cmd === 'string' ? cmd : (cmd ? 'on' : 'off')));
    var text = await response.text();
    
    if (response.status === 403) {
      document.getElementById('mainRelay').checked = systemData.relay;
      if (text === 'ADMIN_LOCKED') {
        showBlocked('Admin Lock Active', 'The meter has been locked by administrator.');
      } else if (text === 'NO_CREDIT') {
        showBlocked('No Credit', 'Please top-up to continue using the meter.');
      } else {
        showBlocked('Blocked', 'Relay operation blocked by billing state.');
      }
      return;
    }
    
    if (response.ok) {
      // Immediately update UI to reflect new state
      systemData.relay = (text === 'ON');
      document.getElementById('mainRelay').checked = systemData.relay;
      updateUI(); // Force UI update
    } else {
      document.getElementById('mainRelay').checked = systemData.relay;
      showBlocked('Error', 'Failed to control relay. Status: ' + response.status);
    }
  } catch (error) {
    console.error('Relay command error:', error);
    document.getElementById('mainRelay').checked = systemData.relay;
    showBlocked('Connection Error', 'Failed to communicate with the server.');
  }
}

function syncTopupFromPreset() {
  var val = document.getElementById('topupPreset').value;
  var daysInput = document.getElementById('topupDays');
  if (val && daysInput) {
    daysInput.value = val;
    // Force the preview update
    updateTopupCostPreview();
  }
}

async function topup(){
  var daysInput = document.getElementById('topupDays');
  var days = parseFloat(daysInput.value);
  
  if (!(days >= 1 && days <= 90)) { 
    showBlocked('Invalid top-up', 'Please enter 1–90 days'); 
    daysInput.style.borderColor = '#ef4444';
    setTimeout(function() {
      daysInput.style.borderColor = '';
    }, 2000);
    return; 
  }
  
  if (systemData.billing_admin_lock) {
    showBlocked('Top-up blocked', 'Admin lock is active. Please contact administrator.');
    return;
  }
  
  var seconds = Math.round(days * 86400);
  
  try {
    var res = await fetch('/api/topup?seconds=' + seconds);
    var data = await res.json();
    
    if (res.status === 403) {
      showBlocked('Top-up blocked', data.error || 'Admin lock active');
      return;
    }
    
    if (!res.ok) {
      showBlocked('Top-up failed', 'Error ' + res.status);
      return;
    }
    
    // Success! Update the UI
    if (data.ok && data.remaining_sec !== undefined) {
      if (systemData) {
        systemData.billing_remaining_sec = data.remaining_sec;
        systemData.billing_timer_running = true;
        systemData.billing_active = true; // Match C++ billing_active
      }
      
      showBlocked('Top-up Successful', 
                  'Added ' + days + ' day(s) of credit.\nRemaining: ' + 
                  formatDaysHours(data.remaining_sec));
      
      daysInput.value = '';
      var presetSelect = document.getElementById('topupPreset');
      if (presetSelect) presetSelect.value = '';
      
      updateTopupCostPreview();
      await fetchData();
      updateBillingTimerDisplay();
    }
  } catch (error) {
    console.error('Top-up error:', error);
    showBlocked('Connection Error', 'Failed to communicate with the server.');
  }
}

async function setAdminLock(on){
  await fetch('/api/admin/lock?action=' + (on ? 'on' : 'off'));
  await fetchData();
}

async function adminLock(){
  if(!isAdmin){ alert('Admin login required'); return; }
  await setAdminLock(true);
}

async function adminUnlock(){
  if(!isAdmin){ alert('Admin login required'); return; }
  await setAdminLock(false);
}

// Format duration for display
function formatDuration(seconds) {
  var days = Math.floor(seconds / 86400);
  var hours = Math.floor((seconds % 86400) / 3600);
  var minutes = Math.floor((seconds % 3600) / 60);
  var secs = seconds % 60;
  
  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m ${secs}s`;
  if (minutes > 0) return `${minutes}m ${secs}s`;
  return `${secs}s`;
}

// Update the adminSetDuration function to work with seconds
async function adminSetDuration(seconds) {
  if (!isAdmin) { adminLogin(); return; }
  var result = await fetch('/api/topup?seconds=' + seconds);
  if (result.ok) {
    await fetchData();
    await refreshBillingRecords();
    showBlocked('Timer Updated', `Added ${formatDuration(seconds)} to the timer.`);
  } else {
    showBlocked('Error', 'Failed to update timer.');
  }
}

// Add pause timer function
async function adminPauseTimer() {
  if (!isAdmin) { adminLogin(); return; }
  var response = await fetch('/api/timer/pause');
  if (response.ok) {
    await fetchData();
    showBlocked('Timer Paused', 'The timer has been paused. Relay is turned off.');
  } else {
    showBlocked('Error', 'Failed to pause timer.');
  }
}

// Add resume timer function
async function adminResumeTimer() {
  if (!isAdmin) { adminLogin(); return; }
  if (systemData.billing_remaining_sec === 0) {
    showBlocked('No Credit', 'Please add credit before resuming.');
    return;
  }
  var response = await fetch('/api/timer/start');
  if (response.ok) {
    await fetchData();
    showBlocked('Timer Resumed', 'The timer has been resumed.');
  } else {
    showBlocked('Error', 'Failed to resume timer.');
  }
}

// Set timer to zero (stop)
async function adminSetTimerZero() {
  if (!isAdmin) { adminLogin(); return; }
  if (confirm('Are you sure you want to stop the timer and clear remaining credit?')) {
    var response = await fetch('/api/timer/stop');
    if (response.ok) {
      await fetchData(); // Refresh all data
      showBlocked('Timer Stopped', 'The timer has been stopped and credit cleared.');
    } else {
      showBlocked('Error', 'Failed to stop timer.');
    }
  }
}

// Update adminSetDays to work with days
async function adminSetDays(days) {
  if (!isAdmin) { adminLogin(); return; }
  var sec = days * 86400;
  await adminSetDuration(sec);
}

// Update adminApplyCustomTimer to work with the new interface
async function adminApplyCustomTimer() {
  if (!isAdmin) { adminLogin(); return; }
  var d = parseInt(document.getElementById('ct_d').value) || 0;
  var h = parseInt(document.getElementById('ct_h').value) || 0;
  var m = parseInt(document.getElementById('ct_m').value) || 0;
  var s = parseInt(document.getElementById('ct_s').value) || 0;
  var total = d * 86400 + h * 3600 + m * 60 + s;
  if (total <= 0) { 
    showBlocked('Invalid duration', 'Please enter at least 1 second or use preset buttons'); 
    return; 
  }
  var result = await fetch('/api/topup?seconds=' + total);
  if (result.ok) {
    document.getElementById('ct_d').value = '';
    document.getElementById('ct_h').value = '';
    document.getElementById('ct_m').value = '';
    document.getElementById('ct_s').value = '';
    await fetchData();
    showBlocked('Timer Updated', `Added ${formatDuration(total)} to the timer.`);
  } else {
    showBlocked('Error', 'Failed to update timer.');
  }
}

async function adminStopTimer() {
  if (!isAdmin) { adminLogin(); return; }
  try {
    var response = await fetch('/api/timer/stop');
    if (response.ok) {
      await fetchData(); // Refresh all data
      showBlocked('Timer Stopped', 'The prepaid timer has been stopped.');
    } else {
      showBlocked('Error', 'Failed to stop timer. Status: ' + response.status);
    }
  } catch(e) {
    console.error('Stop timer error:', e);
    showBlocked('Error', 'Failed to communicate with server.');
  }
}

async function refreshBillingStatus() {
  try {
    var r = await fetch('/api/data');
    var d = await r.json();
    systemData.billing_remaining_sec = d.billing_remaining_sec;
    systemData.billing_timer_running = d.billing_timer_running;
    systemData.billing_admin_lock = d.billing_admin_lock;
    systemData.billing_relay_allowed = d.billing_relay_allowed;
    updateBillingTimerDisplay();
  } catch(e) {
    console.error('Failed to refresh billing status:', e);
  }
}

// Call this periodically to ensure timer display is accurate
setInterval(refreshBillingStatus, 10000);

function showBlocked(title, msg) {
  document.getElementById('blockedModalTitle').innerText = title;
  document.getElementById('blockedModalMsg').innerText = msg;
  document.getElementById('blockedModal').style.display = 'flex';
}

function adminLogin() {
  document.getElementById('adminModal').style.display = 'flex';
  document.getElementById('adminUser').value = '';
  document.getElementById('adminPass').value = '';
  document.getElementById('adminLoginError').style.display = 'none';
  setTimeout(() => document.getElementById('adminUser').focus(), 100);
}

function closeAdminModal() {
  document.getElementById('adminModal').style.display = 'none';
}

async function submitAdminLogin() {
  var user = document.getElementById('adminUser').value;
  var pass = document.getElementById('adminPass').value;
  var res = await fetch('/api/login?user=' + encodeURIComponent(user) + '&pass=' + encodeURIComponent(pass));
  if (res.status === 200) {
    isAdmin = true;
    closeAdminModal();
    setActivePane('billing');
    await fetchData();
    await refreshBillingRecords();
  } else {
    isAdmin = false;
    document.getElementById('adminLoginError').style.display = 'block';
  }
}

async function adminLogout(){
  await fetch('/api/logout');
  isAdmin = false;
  setActivePane('dashboard');
  isAdmin = false;
  alert('Logged out');
}

// Update refreshBillingRecords to include timestamps
async function refreshBillingRecords(){
  if(!isAdmin) return;
  var res = await fetch('/api/records');
  if(!res.ok) return;
  var data = await res.json();
  var tbody = document.getElementById('billingRecordsTbody');
  if(!tbody) return;
  if(!data.records || data.records.length === 0){
    tbody.innerHTML = '<tr><td colspan="4">No records</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  data.records.forEach(function(rec, index){
    var date = new Date();
    // Subtract cumulative time to estimate record date (simplified)
    var row = '<tr>' +
      '<td>' + formatDuration(rec.duration) + '</td>' +
      '<td>' + Number(rec.energyUsed).toFixed(3) + ' kWh</td>' +
      '<td>₹' + Number(rec.cost).toFixed(2) + '</td>' +
      '<td>Session #' + (data.records.length - index) + '</td>' +
      '</tr>';
    tbody.innerHTML += row;
  });
}

fetchData();
setInterval(function(){
  fetchData();
  loadChartJsIfNeeded();
  if (document.getElementById('loadsPane').style.display !== 'none' && !analysisPolling) {
    fetchLoads();
  }
}, 3000);

// Final top-up cost preview function
function updateTopupCostPreview() {
  // Get DOM elements
  var daysInput = document.getElementById('topupDays');
  var costPreviewEl = document.getElementById('topupCostPreview');
  var costValueEl = document.getElementById('topupCostValue');
  
  if (!daysInput || !costPreviewEl || !costValueEl) {
    console.error('Top-up elements not found');
    return;
  }
  
  // Get days value (handle empty or invalid input)
  var days = parseFloat(daysInput.value);
  var isValid = !isNaN(days) && days > 0 && days <= 90;
  
  // Get cost per hour from system data (default to 5.0 if 0 or undefined)
  var costPerHour = (systemData && systemData.billing_cost_per_hour > 0) ? 
                      systemData.billing_cost_per_hour : 5.0;
  
  if (isValid) {
    // Calculate total cost: days * 24 hours * cost per hour
    var totalCost = days * 24 * costPerHour;
    costValueEl.innerText = totalCost.toFixed(2);
    
    // Ensure the preview is visible and styled properly
    costPreviewEl.style.display = 'flex';
    costPreviewEl.style.backgroundColor = 'var(--accent)';
    costPreviewEl.style.opacity = '1';
    costPreviewEl.style.color = 'white';
  } else {
    // Show placeholder when no valid days entered
    costValueEl.innerText = '0.00';
    costPreviewEl.style.display = 'flex';
    costPreviewEl.style.backgroundColor = 'rgba(192,133,82,0.2)';
    costPreviewEl.style.opacity = '0.7';
    costPreviewEl.style.color = 'white';
  }
}

document.addEventListener('DOMContentLoaded', function() {
  // Initialize Pane
  setActivePane('dashboard');
  
  // Setup Live Clock update loop
  setInterval(function() {
    const now = new Date();
    const timeStr = now.toLocaleTimeString('en-IN', { hour12: false });
    ['liveClock', 'liveClockLoads', 'liveClockAnalytics'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.innerText = timeStr;
    });
  }, 1000);

  // Set up top-up days input with clean handling
  var topupDaysInput = document.getElementById('topupDays');
  if (topupDaysInput) {
    topupDaysInput.addEventListener('input', updateTopupCostPreview);
    
    // Also handle blur event to format/validate
    topupDaysInput.addEventListener('blur', function() {
      var val = parseFloat(this.value);
      if (this.value === '') return;
      if (isNaN(val) || val < 1) {
        this.value = '';
        updateTopupCostPreview();
      } else if (val > 90) {
        this.value = '90';
        updateTopupCostPreview();
        showBlocked('Maximum Days', 'Maximum top-up is 90 days');
      }
    });
    
    // Set initial value to 1 to show preview if empty
    if (!topupDaysInput.value) {
      topupDaysInput.value = '1';
    }
    updateTopupCostPreview();
  }
  
  // Set up preset selector - final comprehensive fix
  var presetSelect = document.getElementById('topupPreset');
  if (presetSelect) {
    // Clear any existing onchange and add our own explicit listener
    presetSelect.onchange = null;
    presetSelect.addEventListener('change', function() {
      var val = this.value;
      if (val && topupDaysInput) {
        topupDaysInput.value = val;
        // Update the preview immediately
        updateTopupCostPreview();
      }
    });
  }
});

// ── Analytics engine ───────────────────────────────────────────────────────
var chartJsReady = false;

function loadChartJs() {
  if (chartJsReady) return;
  var script = document.createElement('script');
  script.src = 'https://cdn.jsdelivr.net/npm/chart.js';
  script.onload = function() { chartJsReady = true; initAnalyticsCharts(); replayHistoryIntoCharts(); };
  document.head.appendChild(script);
}

function loadChartJsIfNeeded() {
  if (chartJsReady) return;
  if (document.getElementById('analyticsPane').style.display !== 'none') {
    loadChartJs();
  }
}

var anHistory={ts:[],V:[],I:[],P:[],S:[],Q:[],PF:[],DI:[],mob:[],lap:[],unk:[]};
var anMaxPts=60;
var anCharts={};

var anPalette={
  V:  {line:'#C08552',fill:'rgba(192,133,82,0.12)'},
  I:  {line:'#8C5A3C',fill:'rgba(140,90,60,0.10)'},
  P:  {line:'#d97706',fill:'rgba(217,119,6,0.12)'},
  S:  {line:'#7c3aed',fill:'rgba(124,58,237,0.10)'},
  Q:  {line:'#0891b2',fill:'rgba(8,145,178,0.10)'},
  PF: {line:'#16a34a',fill:'rgba(22,163,74,0.10)'},
  DI: {line:'#dc2626',fill:'rgba(220,38,38,0.08)'},
};

function anMakeChart(id,pal,yMin,yMax){
  var ctx=document.getElementById(id);
  if(!ctx) return null;
  return new Chart(ctx,{
    type:'line',
    data:{labels:[],datasets:[{data:[],borderColor:pal.line,backgroundColor:pal.fill,
      borderWidth:2,fill:true,tension:0.35,pointRadius:0}]},
    options:{responsive:true,maintainAspectRatio:false,animation:false,
      plugins:{legend:{display:false},tooltip:{mode:'index',intersect:false}},
      scales:{
        x:{ticks:{color:'#8C5A3C',font:{size:10},maxRotation:0,autoSkip:true,maxTicksLimit:8},
           grid:{color:'rgba(140,90,60,0.06)'}},
        y:{ticks:{color:'#8C5A3C',font:{size:10}},grid:{color:'rgba(140,90,60,0.08)'},
           min:yMin,max:yMax}
      }
    }
  });
}

function initAnalyticsCharts(){
  if(anCharts.V) return; // already built
  anCharts.V  = anMakeChart('an_cV',  anPalette.V,  150, 260);
  anCharts.I  = anMakeChart('an_cI',  anPalette.I,  0,   null);
  anCharts.P  = anMakeChart('an_cP',  anPalette.P,  0,   null);
  anCharts.S  = anMakeChart('an_cS',  anPalette.S,  0,   null);
  anCharts.Q  = anMakeChart('an_cQ',  anPalette.Q,  0,   null);
  anCharts.PF = anMakeChart('an_cPF', anPalette.PF, 0,   1.05);
  anCharts.DI = anMakeChart('an_cDI', anPalette.DI, 0,   null);

  var kwhCtx=document.getElementById('an_cKWH');
  if(kwhCtx){
    anCharts.KWH=new Chart(kwhCtx,{
      type:'line',
      data:{labels:[],datasets:[
        {label:'Mobile',data:[],borderColor:'#818cf8',backgroundColor:'rgba(129,140,248,0.45)',borderWidth:1.5,fill:'origin',tension:0.2,pointRadius:0},
        {label:'Laptop',data:[],borderColor:'#34d399',backgroundColor:'rgba(52,211,153,0.45)',borderWidth:1.5,fill:'-1',tension:0.2,pointRadius:0},
        {label:'Other', data:[],borderColor:'#f87171',backgroundColor:'rgba(248,113,113,0.45)',borderWidth:1.5,fill:'-1',tension:0.2,pointRadius:0},
      ]},
      options:{responsive:true,maintainAspectRatio:false,animation:false,
        plugins:{legend:{display:false},tooltip:{mode:'index',intersect:false}},
        scales:{
          x:{ticks:{color:'#8C5A3C',font:{size:10},maxRotation:0,autoSkip:true,maxTicksLimit:10},grid:{color:'rgba(140,90,60,0.06)'}},
          y:{stacked:true,ticks:{color:'#8C5A3C',font:{size:10}},grid:{color:'rgba(140,90,60,0.08)'}}
        }
      }
    });
  }
}

function setAnalyticsWindow(secs){
  anMaxPts=secs;
  [60,300,900,3600].forEach(function(s){
    var el=document.getElementById('winTab'+s);
    if(el) el.style.opacity=(s===secs?'1':'0.45');
  });
}

function anPush(arr,val){arr.push(val);if(arr.length>anMaxPts)arr.shift();}

function replayHistoryIntoCharts(){
  if(!anCharts.V) return;
  var d=systemData;
  var t=new Date().toLocaleTimeString('en-IN',{hour12:false,hour:'2-digit',minute:'2-digit',second:'2-digit'});
  var pf=(d.s>0.5?d.p/d.s:0);
  var di=(d.s>0.5?d.q/d.s*100:0);

  anPush(anHistory.ts,  t);
  anPush(anHistory.V,   +d.v.toFixed(2));
  anPush(anHistory.I,   +d.i.toFixed(4));
  anPush(anHistory.P,   +d.p.toFixed(2));
  anPush(anHistory.S,   +d.s.toFixed(2));
  anPush(anHistory.Q,   +d.q.toFixed(2));
  anPush(anHistory.PF,  +pf.toFixed(3));
  anPush(anHistory.DI,  +di.toFixed(2));
  anPush(anHistory.mob, +d.mob_kwh.toFixed(6));
  anPush(anHistory.lap, +d.lap_kwh.toFixed(6));
  anPush(anHistory.unk, +d.unk_kwh.toFixed(6));

  let neededWidth = Math.max(10, anHistory.ts.length * 15);

  ['V','I','P','S','Q','PF','DI'].forEach(function(k){
    var c=anCharts[k];
    if(!c) return;
    
    let wrapper = c.canvas.closest('.canvas-wrapper');
    let inner = c.canvas.closest('.canvas-inner');
    if (inner && wrapper) inner.style.width = Math.max(wrapper.clientWidth, neededWidth) + 'px';

    c.data.labels=anHistory.ts.slice();
    c.data.datasets[0].data=anHistory[k].slice();
    c.update('none');

    if (wrapper && wrapper.dataset.userScrolled !== "true") {
      wrapper.scrollLeft = wrapper.scrollWidth;
    }
  });

  if(anCharts.KWH){
    let wrapper = anCharts.KWH.canvas.closest('.canvas-wrapper');
    let inner = anCharts.KWH.canvas.closest('.canvas-inner');
    if (inner && wrapper) inner.style.width = Math.max(wrapper.clientWidth, neededWidth) + 'px';

    anCharts.KWH.data.labels=anHistory.ts.slice();
    anCharts.KWH.data.datasets[0].data=anHistory.mob.slice();
    anCharts.KWH.data.datasets[1].data=anHistory.lap.slice();
    anCharts.KWH.data.datasets[2].data=anHistory.unk.slice();
    anCharts.KWH.update('none');

    if (wrapper && wrapper.dataset.userScrolled !== "true") {
      wrapper.scrollLeft = wrapper.scrollWidth;
    }
  }

  // Live labels
  document.getElementById('an_hd_V').textContent  = d.v.toFixed(1)+' V';
  document.getElementById('an_hd_I').textContent  = d.i.toFixed(3)+' A';
  document.getElementById('an_hd_P').textContent  = d.p.toFixed(1)+' W';
  document.getElementById('an_hd_S').textContent  = d.s.toFixed(1)+' VA';
  document.getElementById('an_hd_Q').textContent  = d.q.toFixed(1)+' VAr';
  document.getElementById('an_hd_PF').textContent = pf.toFixed(3);
  document.getElementById('an_hd_DI').textContent = di.toFixed(1)+'%';
  document.getElementById('an_hd_KWH').textContent= d.kwh.toFixed(5)+' kWh';

  var avgV=anHistory.V.reduce(function(a,b){return a+b;},0)/anHistory.V.length||0;
  var peakP=Math.max.apply(null,anHistory.P.length?anHistory.P:[0]);
  var avgPF=anHistory.PF.reduce(function(a,b){return a+b;},0)/anHistory.PF.length||0;
  document.getElementById('an_kv_V').textContent  = avgV.toFixed(1);
  document.getElementById('an_kv_P').textContent  = peakP.toFixed(0);
  document.getElementById('an_kv_PF').textContent = avgPF.toFixed(2);
  document.getElementById('an_kv_KWH').textContent= d.kwh.toFixed(5);
}

function updateAnalyticsCharts(){
  if(!anCharts.V) return; // pane not yet opened
  replayHistoryIntoCharts();
}

document.getElementById('dashboardNavBtn').onclick = () => setActivePane('dashboard');
document.getElementById('loadsNavBtn').onclick = () => setActivePane('loads');
document.getElementById('analyticsNavBtn').onclick = () => setActivePane('analytics');
document.getElementById('billingNavBtn').onclick = () => setActivePane('billing');

// ═══════════════════════════ RESPONSIVE MOBILE IMPROVEMENTS ═══════════════════════════
// Touch-friendly improvements
document.addEventListener('DOMContentLoaded', function() {
  // Improve button tap feedback
  document.querySelectorAll('.btn-primary').forEach(btn => {
    btn.addEventListener('touchstart', function() {
      this.style.transform = 'scale(0.98)';
    });
    btn.addEventListener('touchend', function() {
      this.style.transform = '';
    });
  });
  
  // Prevent zoom on double tap (iOS)
  let lastTouchEnd = 0;
  document.addEventListener('touchend', function(e) {
    const now = Date.now();
    if (now - lastTouchEnd <= 300) {
      e.preventDefault();
    }
    lastTouchEnd = now;
  }, false);
});

// Detect mobile
function isMobile() {
  return window.matchMedia("(max-width: 768px)").matches;
}

// Debounce resize events
let resizeTimer;
window.addEventListener('resize', function() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(function() {
    if (typeof updateAnalyticsCharts === 'function') {
      updateAnalyticsCharts();
    }
  }, 250);
});

// Lazy load charts only when needed
let chartsInitialized = false;
const originalSetActivePane = setActivePane;
setActivePane = function(pane) {
  originalSetActivePane(pane);
  if (pane === 'analytics' && !chartsInitialized) {
    chartsInitialized = true;
    if (typeof initAnalyticsCharts === 'function') {
      setTimeout(() => initAnalyticsCharts(), 100);
    }
  }
};

// Add swipe navigation for mobile
let touchStartX = null;
let touchEndX = 0;
const panes = ['dashboard', 'loads', 'analytics', 'billing'];

document.addEventListener('touchstart', function(e) {
  if (document.querySelector('.graph-card.fullscreen') || e.target.closest('.canvas-wrapper') || e.target.closest('.canvas-inner')) {
    touchStartX = null;
    return;
  }
  touchStartX = e.changedTouches[0].screenX;
}, {passive: true});

document.addEventListener('touchend', function(e) {
  if (touchStartX === null) return;
  touchEndX = e.changedTouches[0].screenX;
  const swipeDistance = touchEndX - touchStartX;
  
  if (Math.abs(swipeDistance) > 120 && isMobile()) {
    const currentPane = getCurrentPane();
    const currentIndex = panes.indexOf(currentPane);
    
    if (swipeDistance > 0 && currentIndex > 0) {
      setActivePane(panes[currentIndex - 1]);
    } else if (swipeDistance < 0 && currentIndex < panes.length - 1) {
      setActivePane(panes[currentIndex + 1]);
    }
  }
  touchStartX = null;
}, {passive: true});

function getCurrentPane() {
  if (document.getElementById('dashboardPane').style.display !== 'none') return 'dashboard';
  if (document.getElementById('loadsPane').style.display !== 'none') return 'loads';
  if (document.getElementById('analyticsPane').style.display !== 'none') return 'analytics';
  if (document.getElementById('billingPane').style.display !== 'none') return 'billing';
  return 'dashboard';
}

// Improve touch targets
document.querySelectorAll('input, select, textarea, button').forEach(el => {
  if (!el.classList.contains('btn-primary')) {
    el.style.minHeight = '44px';
  }
});

// Setup scrollable chart containers for horizontally swiping charts
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('canvas[id^="an_c"]').forEach(canvas => {
    let wrapper = canvas.parentElement;
    wrapper.style.overflowX = 'auto';
    wrapper.style.overflowY = 'hidden';
    wrapper.style.webkitOverflowScrolling = 'touch';
    let inner = document.createElement('div');
    inner.className = 'canvas-inner';
    inner.style.height = '100%';
    inner.style.width = '100%';
    wrapper.insertBefore(inner, canvas);
    inner.appendChild(canvas);
    wrapper.addEventListener('scroll', () => {
      let isAtRight = (wrapper.scrollWidth - wrapper.scrollLeft - wrapper.clientWidth) < 15;
      wrapper.dataset.userScrolled = isAtRight ? "false" : "true";
    });
  });
});

// Graph Fullscreen Logic
function toggleFullscreen(card) {
  if (card.classList.contains('fullscreen')) return closeFullscreen();
  document.querySelectorAll('.graph-card.fullscreen').forEach(c => c.classList.remove('fullscreen'));
  card.classList.add('fullscreen');
  document.getElementById('graphOverlay').classList.add('active');
  setTimeout(() => { Object.values(anCharts).forEach(c => c && c.resize()); }, 300);
}
function closeFullscreen() {
  document.querySelectorAll('.graph-card.fullscreen').forEach(c => c.classList.remove('fullscreen'));
  document.getElementById('graphOverlay').classList.remove('active');
  setTimeout(() => { Object.values(anCharts).forEach(c => c && c.resize()); }, 300);
}
</script>
</body>
</html>
)=====";

void handleRoot() {
  server.sendHeader("Cache-Control", "public, max-age=3600");
  server.sendHeader("Vary", "Accept-Encoding");
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  UNO_SERIAL.begin(UNO_BAUD, SERIAL_8N1, 16, 17);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // idle = OFF (active-low)

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("  Smart Monitor");
  lcd.setCursor(0, 1);
  lcd.print("   Initializing");
  delay(2000);
  lcd.clear();

  Serial.printf("\nConnecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
    delay(200);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin("loadmonitor");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("IP Address:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString().c_str());
    lcdIpDisplayStartMs = millis();
    showingIpAddress = true;
  } else {
    Serial.println("\nWiFi failed — offline mode");
  }

  loadBillingFromEEPROM();
  loadDynamicLoads();

  // Fetch backup data from Google Sheets asynchronously so setup is
  // non-blocking
  if (WiFi.status() == WL_CONNECTED) {
    xTaskCreatePinnedToCore(fetchBackupTask, "GoogleFetch", 4096, NULL, 1, NULL,
                            0);
  }

  // Ensure relay is in correct state after loading EEPROM
  if (billing.adminLock || billing.remainingSeconds == 0 ||
      !billing.timerRunning) {
    setRelay(false);
  } else {
    setRelay(true);
  }

  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.on("/api/topup", handleTopup);
  server.on("/api/timer/start", handleTimerStart);
  server.on("/api/timer/pause", handleTimerPause);
  server.on("/api/timer/stop", handleTimerStop);
  server.on("/api/admin/lock", handleAdminLock);
  server.on("/api/login", handleLogin);
  server.on("/api/logout", handleLogout);
  server.on("/api/records", handleRecords);
  server.on("/api/loads", handleApiLoads);
  server.on("/api/loads/add", handleApiLoadAdd);
  server.on("/api/loads/update", handleApiLoadUpdate);
  server.on("/api/loads/delete", handleApiLoadDelete);
  server.on("/api/analysis/start", handleApiAnalysisStart);
  server.on("/api/analysis/stop", handleApiAnalysisStop);
  server.on("/relay/on", handleRelayOn);
  server.on("/relay/off", handleRelayOff);
  server.on("/relay/toggle", handleRelayToggle);
  server.begin();
  Serial.println("HTTP server started on port 80");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  updateLcdDisplay();

  updateBillingTimer();

  while (UNO_SERIAL.available()) {
    static int pos = 0;
    char c = UNO_SERIAL.read();
    if (c == '\n' || c == '\r') {
      if (pos > 4) {
        rawLine[pos] = '\0';
        parseUnoLine(rawLine);
      }
      pos = 0;
    } else if (pos < (int)sizeof(rawLine) - 2) {
      rawLine[pos++] = c;
    }
  }

  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped — reconnecting…");
      WiFi.reconnect();
    }
  }

  // Upload live data to Google Sheets every 5 minutes
  static unsigned long lastUpload = 0;
  if (millis() - lastUpload > 300000 &&
      WiFi.status() == WL_CONNECTED) { // 5 minutes
    lastUpload = millis();
    uploadToGoogleSheets();
  }
}