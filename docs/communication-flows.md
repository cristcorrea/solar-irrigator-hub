# Communication flows

## Boot and mode changes

1. Initialize NVS, load `work_mode` and pending telemetry.
2. Start Wi-Fi in station mode and BLUFI.
3. Create the ESP-NOW receive queue/worker, then start ESP-NOW.
4. Offline mode keeps Wi-Fi disconnected while retaining the radio and BLUFI.
5. Online mode reconnects Wi-Fi; after IP acquisition the mode task starts SNTP and MQTT.
6. Returning offline stops MQTT but leaves ESP-NOW active.

## Irrigation device to app

1. A device sends ESP-NOW data.
2. The callback copies a bounded payload into a queue without blocking.
3. The worker verifies registration, sender MAC, format and numeric ranges.
4. The record is committed to NVS.
5. MQTT serves a snapshot and deletes its records after PUBACK. BLUFI serves a snapshot and deletes it after the BLUFI send call succeeds.

## App configuration to irrigation device

1. MQTT or BLUFI validates JSON and `MACSLAVE` using the same storage function.
2. Canonical JSON is committed under the target MAC and its applied ACK is reset.
3. On a validated `HELLO_HUB`, the Hub registers the ESP-NOW peer and sends that target's configuration with `ts`.
4. `CFG_OK` persists the ACK, optionally reports it through MQTT and returns `ACK_END` to the same peer.

