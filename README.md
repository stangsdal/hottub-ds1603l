# Hottub Level and Temperature Monitor

An ESP8266-based MQTT monitor for tracking water level and temperature in a hot tub, using the **Homie v5** IoT framework for reliable WiFi/MQTT connectivity and automatic Home Assistant discovery.

## Features

- **Water Level Monitoring** - DS1603L ultrasonic distance sensor
- **Temperature Monitoring** - Up to 8 DS18B20 temperature sensors on OneWire bus
- **Reliable MQTT** - Homie v5 framework with strict WiFi/MQTT recovery and backoff timers
- **Automatic Discovery** - Home Assistant MQTT discovery (Homie convention)
- **Web Configuration** - Built-in web UI and HTTP API (provided by Homie)
- **OTA Updates** - Over-the-air firmware updates via MQTT
- **Auto-Recovery** - Automatic reboot if offline for >15 minutes

## Hardware

### Board
- **ESP8266 (ESP-12E)** - 80 MHz, 80 KB RAM, 4 MB Flash

### Sensors
- **DS1603L** - Ultrasonic distance sensor (water level)
  - TX (yellow) → GPIO14 (D5) via SoftwareSerial
  - Baud rate: 9600
  - Tank height: 850 mm (configurable)

- **DS18B20** - Temperature sensors (up to 8)
  - Data (yellow) → GPIO13 (D4) 
  - 4.7 kΩ pull-up resistor to 3.3V
  - OneWire protocol

### Pin Summary
```
GPIO14 (D5) - DS1603L RX (SoftwareSerial)
GPIO13 (D4) - DS18B20 OneWire data line
LED_BUILTIN - Status indicator
```

## Dependencies

The project uses **PlatformIO** and includes:
- `paulstoffregen/OneWire` - OneWire protocol
- `milesburton/DallasTemperature` - DS18B20 driver
- `labodj/homie-v5` - MQTT IoT framework (v3.6.0+)

See `platformio.ini` for the full dependency list.

## Building and Uploading

### Build
```bash
pio run
```

### Upload to Device
```bash
pio run --target upload --upload-port <DEVICE_IP_OR_PORT>
```

### Upload Filesystem (Homie UI bundle)
This project includes [data/homie/ui_bundle.gz](data/homie/ui_bundle.gz). Upload it once after flashing firmware:

```bash
pio run --target uploadfs --upload-port <SERIAL_PORT>
```

Notes:
- `uploadfs` should be done over serial in this setup.
- If no serial device is connected, PlatformIO will fail opening the port.

### Monitor Serial Output
```bash
pio run --target upload --upload-port <DEVICE_IP_OR_PORT> -m
```

## Configuration

### Initial WiFi Setup
1. Power on the device
2. Join the WiFi AP: **`Homie-xxxxxxxxxxxx`** (device-specific suffix)
3. Open http://192.168.123.1 in a browser
4. Configure:
   - WiFi network SSID and password
   - MQTT broker host and port
   - MQTT username and password (optional)

### Web Interface
Once connected to WiFi and MQTT, access the device web interface:
- **IP Address**: Displayed on serial output or check your router
- **Config AP Address** (configuration mode): `192.168.123.1`

Configure MQTT settings and view device status through the web UI.

## MQTT Topics

The device publishes to the following Homie v5 topics:

### Device Info
- `homie/5/<device-id>/device/uptime-s` - Uptime in seconds
- `homie/5/<device-id>/device/rssi` - WiFi signal strength (dBm)
- `homie/5/<device-id>/device/heap` - Free heap memory (bytes)

### Water Level
- `homie/5/<device-id>/level/distance-mm` - Raw distance (mm)
- `homie/5/<device-id>/level/level-percent` - Level as percentage

### Temperature
- `homie/5/<device-id>/temperature/probe-<n>-temp-c` - Temperature (°C)

### Device Description
- `homie/5/<device-id>/$description` - JSON discovery document

## Home Assistant Integration

The device publishes Homie MQTT topics and metadata. If you use Home Assistant,
integrate through your Homie-compatible flow/bridge, or consume the topics directly.
  
## Serial Output

The device logs diagnostic information to serial (115200 baud):
- Sensor readings (every 1.5-3 seconds)
- WiFi connection status
- MQTT connection events
- Temperature sensor discovery
- Homie initialization and events

## Troubleshooting

### WiFi not connecting
- Check SSID and password in the web UI (http://device-ip)
- Device will reboot into setup mode if WiFi fails

### "UI bundle not loaded"
This means `/homie/ui_bundle.gz` is missing in the device filesystem.

Fix:
1. Ensure the file exists in this project at [data/homie/ui_bundle.gz](data/homie/ui_bundle.gz)
2. Upload filesystem over serial:
  - `pio run --target uploadfs --upload-port <SERIAL_PORT>`
3. Reboot the device and reconnect to the Homie AP

If you cannot upload filesystem right now, use the hosted configurator:
- https://labodj.github.io/homie-esp8266/configurators/v2/

### Sensors not reading
- Verify wiring matches the pin configuration above
- Check serial output for sensor discovery messages
- Ensure pull-up resistor (4.7 kΩ) is present for DS18B20

### MQTT not connecting
- Verify broker address, port, and credentials in web UI
- Check serial output for connection errors
- Device logs MQTT events to the web dashboard

## Architecture

The refactored codebase uses the **Homie v5 IoT framework** which:
- Handles WiFi reconnection with strict backoff timers
- Manages MQTT connection and auto-reconnection
- Provides web UI and HTTP API for configuration
- Implements OTA firmware updates over MQTT
- Auto-reboots after 15+ minutes offline (recovery policy)

The core sensor logic remains independent and reusable:
- `tryParseSensorFrame()` - DS1603L frame parsing
- `calculateLevelPercent()` - Level calculation
- Temperature sensor reading and logging

## Development

### File Structure
```
src/main.cpp          - Main sketch with Homie integration
platformio.ini        - Build configuration
doc/                  - Documentation
```

### Key Functions
- `setup()` - Initializes sensors and Homie framework
- `loop()` - Main loop (calls Homie.loop(), processes sensors)
- `publishSensorReadings()` - Publishes all sensor data via Homie
- `onHomieEvent()` - Handles Homie events (WiFi, MQTT, OTA)
- `recordLevelLog()` - Logs level history (every 5 minutes)

## License

[Your license here]

## References

- [Homie Convention](https://homieiot.github.io/)
- [Homie v5 Library](https://registry.platformio.org/libraries/labodj/homie-v5)
- [Home Assistant MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
- [DS1603L Sensor](https://www.dfrobot.com/product-1592.html)
- [DS18B20 Temperature Sensor](https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf)
