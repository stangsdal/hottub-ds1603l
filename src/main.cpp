#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Homie.h>

// Sensor configuration
static const uint8_t kSensorRxPin = 14;
static const uint32_t kSensorBaud = 9600;
static const uint16_t kTankHeightMm = 850;

static const uint8_t kOneWirePin = 13;
static const uint8_t kMaxTempSensors = 8;
static const size_t kTempPropIdLen = 24;
static const size_t kTempPropNameLen = 16;

static const char* kPropDistanceMmLegacy = "distance-mm";
static const char* kPropLevelPercentLegacy = "level-percent";
static const char* kPropUptimeLegacy = "uptime-s";

// Timing constants
static const unsigned long kBlinkIntervalMs = 500;
static const unsigned long kSensorReportIntervalMs = 1500;
static const unsigned long kTempReportIntervalMs = 3000;
static const unsigned long kLevelLogIntervalMs = 5 * 60 * 1000;  // 5 minutes
static const unsigned long kMqttPublishIntervalMs = 15000;

unsigned long lastBlinkAt = 0;
unsigned long lastSensorReportAt = 0;
unsigned long lastTempReportAt = 0;
unsigned long lastLevelLogAt = 0;
unsigned long lastMqttPublishAt = 0;
bool ledState = false;

OneWire oneWire(kOneWirePin);
DallasTemperature tempSensors(&oneWire);
uint8_t tempSensorCount = 0;
float latestTemperaturesC[kMaxTempSensors] = {DEVICE_DISCONNECTED_C};

SoftwareSerial sensorSerial(kSensorRxPin, -1);
uint8_t sensorFrame[4] = {0, 0, 0, 0};
uint8_t sensorFrameIndex = 0;
uint16_t latestDistanceMm = 0;
bool sensorHasReading = false;
uint32_t validFrameCount = 0;
uint32_t badFrameCount = 0;
bool webServerStarted = false;
bool arduinoOtaStarted = false;
bool mqttReady = false;
bool mqttLogEnabled = true;
String mqttLastEvent = "boot";
unsigned long mqttLastEventAtMs = 0;

static const uint8_t kMaxMqttLogEntries = 80;
String mqttLog[kMaxMqttLogEntries] = {};
uint8_t mqttLogHead = 0;
uint8_t mqttLogCount = 0;

char levelDistanceFormat[16] = "0:850";
char tempPropLegacyIds[kMaxTempSensors][kTempPropIdLen] = {};
char tempPropNames[kMaxTempSensors][kTempPropNameLen] = {};

AsyncWebServer statusServer(80);

// Level logging (4 hours, every 5 minutes)
struct LevelLogEntry {
  uint32_t timestampS;
  uint16_t levelPercent;
};
static const uint8_t kMaxLogEntries = 48;
LevelLogEntry levelLog[kMaxLogEntries] = {};
uint8_t levelLogIndex = 0;
uint8_t levelLogCount = 0;

#if defined(LED_BUILTIN)
const int kLedPin = LED_BUILTIN;
const bool kHasBuiltInLed = true;
#else
const int kLedPin = -1;
const bool kHasBuiltInLed = false;
#endif

// Homie nodes and properties
HomieNode levelNode("level", "Water Level", "liquid-level");
HomieNode temperatureNode("temperature", "Temperatures", "temperature");
HomieNode deviceNode("device", "Device Info", "system");

// Property handlers for settable properties (if any commands are added later)
bool handleSetProperty(const HomieRange& range, const String& property, const String& value) {
  return true;  // Read-only for now
}

uint16_t calculateLevelPercent(uint16_t distanceMm, uint16_t tankHeightMm) {
  if (tankHeightMm == 0) {
    return 0;
  }
  return static_cast<uint16_t>((static_cast<uint32_t>(distanceMm) * 100U) / tankHeightMm);
}

uint16_t getCurrentLevelPercent() {
  if (!sensorHasReading) {
    return 0;
  }
  return calculateLevelPercent(latestDistanceMm, kTankHeightMm);
}

void appendMqttLog(const String& message) {
  if (!mqttLogEnabled) {
    return;
  }

  String entry = String(millis() / 1000) + "s " + message;
  mqttLog[mqttLogHead] = entry;
  mqttLogHead = (mqttLogHead + 1) % kMaxMqttLogEntries;
  if (mqttLogCount < kMaxMqttLogEntries) {
    mqttLogCount++;
  }
}

void clearMqttLog() {
  for (uint8_t i = 0; i < kMaxMqttLogEntries; i++) {
    mqttLog[i] = "";
  }
  mqttLogHead = 0;
  mqttLogCount = 0;
}

void setMqttEvent(const String& eventName) {
  mqttLastEvent = eventName;
  mqttLastEventAtMs = millis();
  appendMqttLog(eventName);
}

String getMqttStateText() {
  if (mqttReady) {
    return "ready";
  }
  if (Homie.isConnected()) {
    return "connected";
  }
  return "disconnected";
}

String buildLevelJson() {
  String json = "{";
  json += "\"sensor_has_reading\":";
  json += sensorHasReading ? "true" : "false";
  json += ",\"distance_mm\":";
  json += sensorHasReading ? String(latestDistanceMm) : "null";
  json += ",\"level_percent\":";
  json += sensorHasReading ? String(getCurrentLevelPercent()) : "null";
  json += ",\"valid_frames\":" + String(validFrameCount);
  json += ",\"bad_frames\":" + String(badFrameCount);
  json += "}";
  return json;
}

String buildMqttJson() {
  String json = "{";
  json += "\"mqtt_state\":\"" + getMqttStateText() + "\"";
  json += ",\"mqtt_ready\":";
  json += mqttReady ? "true" : "false";
  json += ",\"homie_connected\":";
  json += Homie.isConnected() ? "true" : "false";
  json += ",\"log_enabled\":";
  json += mqttLogEnabled ? "true" : "false";
  json += ",\"log_count\":" + String(mqttLogCount);
  json += ",\"last_event\":\"" + mqttLastEvent + "\"";
  json += ",\"last_event_age_s\":" + String((millis() - mqttLastEventAtMs) / 1000);
  json += "}";
  return json;
}

String buildHomieDiagnosticJson() {
  String json = "{";
  json += "\"device\":{";
  json += "\"id\":\"" + String(Homie.getConfiguration().deviceId) + "\",";
  json += "\"name\":\"" + String(Homie.getConfiguration().name) + "\",";
  json += "\"state\":\"" + getMqttStateText() + "\",";
  json += "\"base_topic\":\"" + String(Homie.getConfiguration().mqtt.baseTopic) + "\"";
  json += "},";

  json += "\"expected_nodes\":{";

  json += "\"level\":{";
  json += "\"name\":\"Water Level\",";
  json += "\"type\":\"liquid-level\",";
  json += "\"properties\":{";
  json += "\"distance-mm\":{\"datatype\":\"integer\",\"settable\":false,\"retained\":true,\"format\":\"" + String(levelDistanceFormat) + "\",\"unit\":\"mm\"},";
  json += "\"level-percent\":{\"datatype\":\"integer\",\"settable\":false,\"retained\":true,\"format\":\"0:100\",\"unit\":\"%\"}";
  json += "}},";

  json += "\"temperature\":{";
  json += "\"name\":\"Temperatures\",";
  json += "\"type\":\"temperature\",";
  json += "\"properties\":{";
  for (uint8_t i = 0; i < tempSensorCount && i < kMaxTempSensors; i++) {
    if (i > 0) {
      json += ",";
    }
    json += "\"" + String(tempPropLegacyIds[i]) + "\":{";
    json += "\"datatype\":\"float\",\"settable\":false,\"retained\":true,\"unit\":\"C\"";
    json += "}";
  }
  json += "}},";

  json += "\"device\":{";
  json += "\"name\":\"Device Info\",";
  json += "\"type\":\"system\",";
  json += "\"properties\":{";
  json += "\"uptime-s\":{\"datatype\":\"integer\",\"settable\":false,\"retained\":true,\"format\":\"0:\",\"unit\":\"s\"},";
  json += "\"rssi\":{\"datatype\":\"integer\",\"settable\":false,\"retained\":true,\"unit\":\"dBm\"},";
  json += "\"heap\":{\"datatype\":\"integer\",\"settable\":false,\"retained\":true,\"format\":\"0:\",\"unit\":\"B\"}";
  json += "}}";

  json += "},";
  json += "\"runtime\":{";
  json += "\"temp_sensor_count\":" + String(tempSensorCount) + ",";
  json += "\"sensor_has_reading\":" + String(sensorHasReading ? "true" : "false") + ",";
  json += "\"distance_mm\":" + String(sensorHasReading ? String(latestDistanceMm) : "null") + ",";
  json += "\"level_percent\":" + String(sensorHasReading ? String(getCurrentLevelPercent()) : "null");
  json += "}";
  json += "}";
  return json;
}

String buildHomieDiagnosticHtml() {
  String html;
  html.reserve(6000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Homie Diagnostic</title>";
  html += "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Arial,sans-serif;max-width:900px;margin:24px auto;padding:0 16px;color:#111}h1{font-size:1.4rem}pre{background:#f7f7f7;border:1px solid #e5e5e5;padding:10px;border-radius:6px;overflow:auto;white-space:pre-wrap;word-break:break-word}a.btn{display:inline-block;margin-right:8px;padding:8px 10px;border:1px solid #bbb;border-radius:6px;text-decoration:none;color:#111}</style></head><body>";
  html += "<h1>Homie Diagnostic</h1>";
  html += "<p>This is the firmware-side expected Homie model.</p>";
  html += "<p><a class='btn' href='/api/diag/homie'>Raw JSON</a><a class='btn' href='/status'>Back to status</a></p>";
  html += "<pre>" + buildHomieDiagnosticJson() + "</pre>";
  html += "</body></html>";
  return html;
}

String buildStatusHtml() {
  String html;
  html.reserve(1200);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Hot Tub Monitor</title>";
  html += "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Arial,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;color:#111}h1{font-size:1.4rem}table{border-collapse:collapse;width:100%}td{padding:8px;border-bottom:1px solid #ddd}code{background:#f5f5f5;padding:2px 4px;border-radius:4px}</style></head><body>";
  html += "<h1>Hot Tub Monitor Status</h1><table>";
  html += "<tr><td>WiFi SSID</td><td>" + WiFi.SSID() + "</td></tr>";
  html += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
  html += "<tr><td>MQTT</td><td>" + getMqttStateText() + "</td></tr>";
  html += "<tr><td>Distance</td><td>" + (sensorHasReading ? String(latestDistanceMm) + " mm" : String("n/a")) + "</td></tr>";
  html += "<tr><td>Tank level</td><td>" + (sensorHasReading ? String(getCurrentLevelPercent()) + " %" : String("n/a")) + "</td></tr>";
  html += "<tr><td>Heap</td><td>" + String(ESP.getFreeHeap()) + " B</td></tr>";
  html += "</table><p>API: <code>/api/level</code> | <code>/api/mqtt</code></p>";
  html += "<p><a href='/mqtt'>MQTT status and log</a> | <a href='/diag/homie'>Homie diagnostic</a></p></body></html>";
  return html;
}

String buildMqttStatusHtml() {
  String html;
  html.reserve(5000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>MQTT Status</title>";
  html += "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Arial,sans-serif;max-width:820px;margin:24px auto;padding:0 16px;color:#111}h1{font-size:1.4rem}table{border-collapse:collapse;width:100%;margin-bottom:16px}td{padding:8px;border-bottom:1px solid #ddd}a.btn{display:inline-block;margin-right:8px;padding:8px 10px;border:1px solid #bbb;border-radius:6px;text-decoration:none;color:#111}pre{background:#f7f7f7;border:1px solid #e5e5e5;padding:10px;border-radius:6px;max-height:420px;overflow:auto;white-space:pre-wrap;word-break:break-word}</style></head><body>";
  html += "<h1>MQTT Status and Log</h1><table>";
  html += "<tr><td>MQTT state</td><td>" + getMqttStateText() + "</td></tr>";
  html += "<tr><td>Homie connected</td><td>" + String(Homie.isConnected() ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Last MQTT event</td><td>" + mqttLastEvent + "</td></tr>";
  html += "<tr><td>Last event age</td><td>" + String((millis() - mqttLastEventAtMs) / 1000) + " s</td></tr>";
  html += "<tr><td>Log enabled</td><td>" + String(mqttLogEnabled ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Log entries</td><td>" + String(mqttLogCount) + " / " + String(kMaxMqttLogEntries) + "</td></tr>";
  html += "</table>";

  html += "<p>";
  html += "<a class='btn' href='/api/mqtt/logging?enabled=1'>Enable log</a>";
  html += "<a class='btn' href='/api/mqtt/logging?enabled=0'>Disable log</a>";
  html += "<a class='btn' href='/api/mqtt/logging?clear=1'>Clear log</a>";
  html += "<a class='btn' href='/status'>Back to status</a>";
  html += "</p>";

  html += "<h2>Log</h2><pre>";
  if (mqttLogCount == 0) {
    html += "(empty)";
  } else {
    for (uint8_t i = 0; i < mqttLogCount; i++) {
      uint8_t idx = (mqttLogHead + kMaxMqttLogEntries - 1 - i) % kMaxMqttLogEntries;
      if (mqttLog[idx].length() > 0) {
        html += mqttLog[idx] + "\n";
      }
    }
  }
  html += "</pre></body></html>";
  return html;
}

void startStatusWebServer() {
  if (webServerStarted) {
    return;
  }

  statusServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", buildStatusHtml());
  });

  statusServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", buildStatusHtml());
  });

  statusServer.on("/api/level", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildLevelJson());
  });

  statusServer.on("/mqtt", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", buildMqttStatusHtml());
  });

  statusServer.on("/api/mqtt/logging", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool clearRequested = false;

    for (size_t i = 0; i < request->params(); i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (p->name() == "enabled") {
        String enabledRaw = p->value();
        bool enableLog = enabledRaw == "1" || enabledRaw == "true" || enabledRaw == "on";
        if (!enableLog && mqttLogEnabled) {
          appendMqttLog("mqtt log disabled from web");
        }
        mqttLogEnabled = enableLog;
        if (mqttLogEnabled) {
          appendMqttLog("mqtt log enabled from web");
        }
      }

      if (p->name() == "clear" && p->value() == "1") {
        clearRequested = true;
      }
    }

    if (clearRequested) {
      clearMqttLog();
      appendMqttLog("mqtt log cleared from web");
    }

    request->send(200, "application/json", buildMqttJson());
  });

  statusServer.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildMqttJson());
  });

  statusServer.on("/diag/homie", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", buildHomieDiagnosticHtml());
  });

  statusServer.on("/api/diag/homie", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildHomieDiagnosticJson());
  });

  statusServer.begin();
  webServerStarted = true;
  Serial.printf("Status web ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void startArduinoOta() {
  if (arduinoOtaStarted) {
    return;
  }

  String host = String("hottub-monitor-") + String(ESP.getChipId(), HEX);
  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.onStart([]() {
    Serial.println("Arduino OTA start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("Arduino OTA end");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Arduino OTA error: %u\n", static_cast<unsigned int>(error));
  });
  ArduinoOTA.begin();
  arduinoOtaStarted = true;
  Serial.printf("Arduino OTA ready: host %s\n", host.c_str());
}

void publishSensorReadings() {
  if (!Homie.isConnected()) {
    return;
  }

  // Publish level sensor data
  if (sensorHasReading) {
    levelNode.setProperty(kPropDistanceMmLegacy).send(String(latestDistanceMm));
    uint16_t levelPct = calculateLevelPercent(latestDistanceMm, kTankHeightMm);
    levelNode.setProperty(kPropLevelPercentLegacy).send(String(levelPct));
  }

  // Publish device info
  deviceNode.setProperty(kPropUptimeLegacy).send(String(millis() / 1000));
  deviceNode.setProperty("rssi").send(String(WiFi.RSSI()));
  deviceNode.setProperty("heap").send(String(ESP.getFreeHeap()));

  // Publish temperature readings
  for (uint8_t i = 0; i < tempSensorCount && i < kMaxTempSensors; i++) {
    if (latestTemperaturesC[i] != DEVICE_DISCONNECTED_C) {
      temperatureNode.setProperty(tempPropLegacyIds[i]).send(String(latestTemperaturesC[i], 1));
    }
  }

  appendMqttLog("mqtt publish cycle complete");
}

void recordLevelLog(unsigned long now) {
  if (now - lastLevelLogAt >= kLevelLogIntervalMs) {
    lastLevelLogAt = now;

    // Skip log samples until we have at least one valid sensor frame.
    if (!sensorHasReading) {
      Serial.println("Level log: skipped (no sensor reading yet)");
      return;
    }

    uint16_t level = calculateLevelPercent(latestDistanceMm, kTankHeightMm);
    uint32_t ts = static_cast<uint32_t>(time(nullptr));
    if (ts < 1609459200UL) {
      ts = now / 1000UL;
    }

    levelLog[levelLogIndex].timestampS = ts;
    levelLog[levelLogIndex].levelPercent = level;
    levelLogIndex = (levelLogIndex + 1) % kMaxLogEntries;

    if (levelLogCount < kMaxLogEntries) {
      levelLogCount++;
    }

    Serial.print("Level log: ");
    Serial.print(level);
    Serial.println("%");
  }
}

void printChipInfo() {
  Serial.println("=== Board Diagnostics ===");
  Serial.println("Platform: ESP8266");
  Serial.printf("Chip ID: 0x%06X\n", ESP.getChipId());
  Serial.printf("CPU Freq: %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipRealSize());
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Built-in LED: %s\n", kHasBuiltInLed ? "yes" : "no");
}

void onHomieEvent(const HomieEvent& event) {
  switch (event.type) {
    case HomieEventType::WIFI_CONNECTED:
      Serial.println("WiFi connected");
      setMqttEvent("wifi_connected");
      if (!MDNS.begin("hottub-monitor")) {
        Serial.println("mDNS start failed");
      }
      startStatusWebServer();
      startArduinoOta();
      break;
    case HomieEventType::WIFI_DISCONNECTED:
      Serial.println("WiFi disconnected");
      mqttReady = false;
      setMqttEvent("wifi_disconnected");
      break;
    case HomieEventType::MQTT_DISCONNECTED:
      Serial.println("MQTT disconnected");
      mqttReady = false;
      setMqttEvent("mqtt_disconnected");
      break;
    case HomieEventType::MQTT_READY:
      Serial.println("MQTT ready - publishing initial data");
      mqttReady = true;
      setMqttEvent("mqtt_ready");
      publishSensorReadings();
      break;
    case HomieEventType::OTA_STARTED:
      Serial.println("OTA update started");
      break;
    case HomieEventType::OTA_FAILED:
      Serial.println("OTA update failed");
      break;
    default:
      break;
  }
}

bool tryParseSensorFrame(uint16_t& distanceMm) {
  while (sensorSerial.available() > 0) {
    uint8_t b = static_cast<uint8_t>(sensorSerial.read());

    if (sensorFrameIndex == 0) {
      if (b != 0xFF) {
        continue;
      }
      sensorFrame[sensorFrameIndex++] = b;
      continue;
    }

    sensorFrame[sensorFrameIndex++] = b;
    if (sensorFrameIndex < 4) {
      continue;
    }

    sensorFrameIndex = 0;
    uint8_t checksum = static_cast<uint8_t>(sensorFrame[0] + sensorFrame[1] + sensorFrame[2]);
    if (checksum != sensorFrame[3]) {
      badFrameCount++;
      continue;
    }

    validFrameCount++;
    distanceMm = static_cast<uint16_t>((sensorFrame[1] << 8) | sensorFrame[2]);
    return true;
  }

  return false;
}


void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(1000);
  Serial.println();
  Serial.println("Booting Homie-based hottub monitor...");

  if (kHasBuiltInLed) {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, LOW);
  }

  // Initialize sensors
  sensorSerial.begin(kSensorBaud);
  tempSensors.begin();
  tempSensorCount = tempSensors.getDeviceCount();

  for (uint8_t i = 0; i < kMaxTempSensors; i++) {
    latestTemperaturesC[i] = DEVICE_DISCONNECTED_C;
  }

  printChipInfo();
  Serial.println("\nWiring summary:");
  Serial.printf("  DS1603L TX (yellow) -> D5 (GPIO%u)\n", kSensorRxPin);
  Serial.printf("  DS18B20 data        -> D4 (GPIO%u) + 4.7k to 3.3V\n", kOneWirePin);
  Serial.printf("  DS18B20 found: %u sensor(s)\n", tempSensorCount);

  // Configure Homie
  Homie_setFirmware("hottub-monitor", "1.0.0");

  snprintf(levelDistanceFormat, sizeof(levelDistanceFormat), "0:%u", kTankHeightMm);

  // Set up level node
  Serial.println("  Advertising level/distance-mm...");
  levelNode.advertise(kPropDistanceMmLegacy)
    .setName("Distance")
    .setDatatype("integer")
    .setUnit("mm")
    .setFormat(levelDistanceFormat)
    .setRetained(true);

  Serial.println("  Advertising level/level-percent...");
  levelNode.advertise(kPropLevelPercentLegacy)
    .setName("Level")
    .setDatatype("integer")
    .setUnit("%")
    .setFormat("0:100")
    .setRetained(true);

  // Set up temperature node (advertise probes based on count)
  Serial.printf("  Setting up temperature node with %u probe(s)...\n", tempSensorCount);
  if (tempSensorCount > 0) {
    for (uint8_t i = 0; i < tempSensorCount && i < kMaxTempSensors; i++) {
      snprintf(tempPropLegacyIds[i], sizeof(tempPropLegacyIds[i]), "probe-%u-temp-c", i);
      snprintf(tempPropNames[i], sizeof(tempPropNames[i]), "Probe %u", i);

      Serial.printf("    Advertising temperature/%s...\n", tempPropLegacyIds[i]);
      temperatureNode.advertise(tempPropLegacyIds[i])
        .setName(tempPropNames[i])
        .setDatatype("float")
        .setUnit("C")
        .setRetained(true);
    }
  }

  // Set up device info node
  Serial.println("  Advertising device/uptime-s...");
  deviceNode.advertise(kPropUptimeLegacy)
    .setName("Uptime")
    .setDatatype("integer")
    .setUnit("s")
    .setFormat("0:")
    .setRetained(true);

  Serial.println("  Advertising device/rssi...");
  deviceNode.advertise("rssi")
    .setName("WiFi RSSI")
    .setDatatype("integer")
    .setUnit("dBm")
    .setRetained(true);

  Serial.println("  Advertising device/heap...");
  deviceNode.advertise("heap")
    .setName("Free Heap")
    .setDatatype("integer")
    .setUnit("B")
    .setFormat("0:")
    .setRetained(true);

  // Set up Homie event handler
  Homie.onEvent(onHomieEvent);

  // Initialize Homie
  Homie.setup();

  Serial.println("\nHomie initialized, connecting to network...");
}

void loop() {
  unsigned long now = millis();
  uint16_t distanceMm = 0;

  // Core Homie loop (handles WiFi, MQTT, and OTA)
  Homie.loop();
  if (arduinoOtaStarted) {
    ArduinoOTA.handle();
  }

  // Blink LED if available
  if (kHasBuiltInLed && (now - lastBlinkAt >= kBlinkIntervalMs)) {
    lastBlinkAt = now;
    ledState = !ledState;
    digitalWrite(kLedPin, ledState ? HIGH : LOW);
  }

  // Parse sensor frames continuously
  if (tryParseSensorFrame(distanceMm)) {
    latestDistanceMm = distanceMm;
    sensorHasReading = true;
  }

  // Report sensor status to serial
  if (now - lastSensorReportAt >= kSensorReportIntervalMs) {
    lastSensorReportAt = now;
    if (!sensorHasReading) {
      Serial.println("DS1603L: waiting for valid frame...");
    } else {
      uint16_t levelPercent = calculateLevelPercent(latestDistanceMm, kTankHeightMm);
      Serial.print("DS1603L: distance ");
      Serial.print(latestDistanceMm);
      Serial.print(" mm, level ");
      Serial.print(levelPercent);
      Serial.print("%, valid ");
      Serial.print(validFrameCount);
      Serial.print(", bad ");
      Serial.println(badFrameCount);
    }
  }

  // Read and report temperature sensors
  if (now - lastTempReportAt >= kTempReportIntervalMs) {
    lastTempReportAt = now;
    if (tempSensorCount == 0) {
      Serial.println("DS18B20: no sensors found on OneWire bus");
    } else {
      tempSensors.requestTemperatures();
      for (uint8_t i = 0; i < tempSensorCount; i++) {
        float tempC = tempSensors.getTempCByIndex(i);
        if (i < kMaxTempSensors) {
          latestTemperaturesC[i] = tempC;
        }
        Serial.print("DS18B20[");
        Serial.print(i);
        Serial.print("]: ");
        if (tempC == DEVICE_DISCONNECTED_C) {
          Serial.println("disconnected");
        } else {
          Serial.print(tempC, 1);
          Serial.println(" C");
        }
      }
    }
  }

  // Publish readings to MQTT
  if (Homie.isConnected() && now - lastMqttPublishAt >= kMqttPublishIntervalMs) {
    lastMqttPublishAt = now;
    publishSensorReadings();
  }

  // Record level history
  recordLevelLog(now);
}