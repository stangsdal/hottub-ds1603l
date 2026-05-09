#include <Arduino.h>
#include <SoftwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Homie.h>

// Sensor configuration
static const uint8_t kSensorRxPin = 14;
static const uint32_t kSensorBaud = 9600;
static const uint16_t kTankHeightMm = 850;

static const uint8_t kOneWirePin = 13;
static const uint8_t kMaxTempSensors = 8;

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

void publishSensorReadings() {
  if (!Homie.isConnected()) {
    return;
  }

  // Publish level sensor data
  if (sensorHasReading) {
    levelNode.setProperty("distance-mm").send(String(latestDistanceMm));
    uint16_t levelPct = calculateLevelPercent(latestDistanceMm, kTankHeightMm);
    levelNode.setProperty("level-percent").send(String(levelPct));
  }

  // Publish device info
  deviceNode.setProperty("uptime-s").send(String(millis() / 1000));
  deviceNode.setProperty("rssi").send(String(WiFi.RSSI()));
  deviceNode.setProperty("heap").send(String(ESP.getFreeHeap()));

  // Publish temperature readings
  for (uint8_t i = 0; i < tempSensorCount && i < kMaxTempSensors; i++) {
    if (latestTemperaturesC[i] != DEVICE_DISCONNECTED_C) {
      String propId = String("probe-") + i + "-temp-c";
      temperatureNode.setProperty(propId.c_str()).send(String(latestTemperaturesC[i], 1));
    }
  }
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
      break;
    case HomieEventType::WIFI_DISCONNECTED:
      Serial.println("WiFi disconnected");
      break;
    case HomieEventType::MQTT_DISCONNECTED:
      Serial.println("MQTT disconnected");
      break;
    case HomieEventType::MQTT_READY:
      Serial.println("MQTT ready - publishing initial data");
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

  // Set up level node
  levelNode.advertise("distance-mm")
    .setName("Distance")
    .setDatatype("integer")
    .setUnit("mm")
    .setFormat(String("0:" + String(kTankHeightMm)).c_str());

  levelNode.advertise("level-percent")
    .setName("Level")
    .setDatatype("integer")
    .setUnit("%")
    .setFormat("0:100");

  // Set up temperature node (advertise probes based on count)
  if (tempSensorCount > 0) {
    for (uint8_t i = 0; i < tempSensorCount && i < kMaxTempSensors; i++) {
      String propId = String("probe-") + i + "-temp-c";
      temperatureNode.advertise(propId.c_str())
        .setName(("Probe " + String(i)).c_str())
        .setDatatype("float")
        .setUnit("°C");
    }
  }

  // Set up device info node
  deviceNode.advertise("uptime-s")
    .setName("Uptime")
    .setDatatype("integer")
    .setUnit("s")
    .setFormat("0:");

  deviceNode.advertise("rssi")
    .setName("WiFi RSSI")
    .setDatatype("integer")
    .setUnit("dBm");

  deviceNode.advertise("heap")
    .setName("Free Heap")
    .setDatatype("integer")
    .setUnit("B")
    .setFormat("0:");

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