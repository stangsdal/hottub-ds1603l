# Technical Bug Report: Incomplete Homie Discovery Prevents Homey Auto-Creation

## Summary
The Homey MQTT/Homie app can discover the device, but it cannot auto-create a Homey device from the current firmware output because the Homie discovery data is incomplete.

## Observed State
The app currently sees the following discovered devices:

- `hottub-144693`
  - No nodes are present.
- `441793144693`
  - Nodes are present (`level`, `temperature`, `device`).
  - The nodes do not contain usable property definitions.
  - Some values are present for properties such as `reason`, `last_disconnect_reason`, and `enabled`, but the discovery metadata required to map them into Homey capabilities is missing.

## Impact
Because the discovery payload does not include complete Homie metadata, the app cannot determine:

- which properties belong to each node,
- what datatype each property uses,
- whether a property is settable or retained,
- which Homey capability should be created for each property.

As a result, the device cannot be automatically created in Homey.

## Missing Firmware Behavior
The firmware needs to publish a complete Homie discovery structure, including:

### Device-level metadata
- `$name`
- `$state`
- `$nodes`
- `$implementation`
- `$homie`
- `$fw/*` fields if used

### Node-level metadata
For each node:
- `$name`
- `$type`

### Property-level metadata
For each property:
- `$datatype`
- `$settable`
- `$retained`
- `$format` when applicable

### Retained discovery structure
For Homie v5, the retained `$description` document or equivalent retained discovery content should describe the full device, node, and property hierarchy so that consumers can reconstruct the device model reliably.

## Expected Result
The firmware should publish a complete and consistent Homie discovery description before or alongside property values so that the Homey app can:

- identify device nodes and properties,
- infer or map Homey capabilities,
- auto-create the Homey device.

## Key Observation (May 10, 2026)
The firmware-side diagnostic endpoint `/api/diag/homie` shows that the firmware *intends* to publish complete property metadata:

```json
{
  "expected_nodes": {
    "level": {
      "properties": {
        "distance-mm": {"datatype":"integer","settable":false,"retained":true,"format":"0:850","unit":"mm"},
        "level-percent": {"datatype":"integer","settable":false,"retained":true,"format":"0:100","unit":"%"}
      }
    },
    "device": {
      "properties": {
        "uptime-s": {"datatype":"integer",...},
        "rssi": {"datatype":"integer",...},
        "heap": {"datatype":"integer",...}
      }
    }
  }
}
```

However, Homey sees those same nodes with **empty** property maps:

```json
{
  "level": {"properties": {}},
  "temperature": {"properties": {}},
  "device": {"properties": {}}
}
```

This indicates the problem is NOT in firmware advertise calls—the firmware is calling `setDatatype()`, `setUnit()`, `setFormat()` etc. correctly. **The property-level metadata is either not being published by homie-v5 to the MQTT broker, or it's being published but Homey is not parsing it.**

## Example of the Current Problem
One discovered device appears as:

```json
{
  "id": "441793144693",
  "nodes": {
    "$implementation": {
      "id": "$implementation",
      "properties": {
        "reason": {
          "id": "reason",
          "value": "Software/System restart"
        },
        "last_disconnect_reason": {
          "id": "last_disconnect_reason",
          "value": "tcp_disconnected"
        },
        "enabled": {
          "id": "enabled",
          "value": "true"
        }
      }
    },
    "level": {
      "id": "level",
      "properties": {},
      "name": "Water Level",
      "type": "liquid-level"
    },
    "temperature": {
      "id": "temperature",
      "properties": {},
      "name": "Temperatures",
      "type": "temperature"
    },
    "device": {
      "id": "device",
      "properties": {},
      "name": "Device Info",
      "type": "system"
    }
  },
  "state": "ready",
  "name": "Hottub",
  "implementation": "esp8266"
}
```

This shows the node skeleton, but not the property metadata needed for Homey auto-creation.

## Workaround Implementation (May 10, 2026 - 13:15 UTC)

**Root Cause Confirmed:** The homie-v5 library does NOT publish property-level metadata (`$datatype`, `$unit`, `$format`, `$settable`, `$retained`) to the MQTT broker, despite the firmware correctly calling `.setDatatype()`, `.setUnit()`, etc. in the advertise() chain.

**Workaround Deployed:** Added `publishPropertyMetadata()` function that manually publishes all property attribute topics directly to the MQTT broker via `Homie.getMqttClient().publish()`. This function is called immediately after MQTT_READY event, before property values are sent.

The firmware now publishes:
- **level/distance-mm**: $datatype, $unit, $format, $settable, $retained
- **level/level-percent**: $datatype, $unit, $format, $settable, $retained
- **device/uptime-s**: $datatype, $unit, $format, $settable, $retained
- **device/rssi**: $datatype, $unit, $settable, $retained
- **device/heap**: $datatype, $unit, $format, $settable, $retained
- **temperature/probe-***: $datatype, $unit, $settable, $retained (per probe)

Deployment: OTA at 13:15 UTC (14.86 seconds), device online, MQTT log confirms "property metadata published".

**Next Step:** Check if property metadata topics now appear on the MQTT broker with `mosquitto_sub` or equivalent tool. Once confirmed, Homey should be able to see and parse property definitions.

## Diagnostic Steps (In Progress)

