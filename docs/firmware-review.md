# Firmware review (ESP-IDF 5.5.3)

## Verified architecture

The configured target is `esp32` (Xtensa, ESP32-WROOM family), not ESP32-C3. Boot starts NVS, the persisted application mode, pending telemetry, GPIO managers, Wi-Fi/BLUFI, the ESP-NOW receive worker and ESP-NOW itself. A mode task starts SNTP and MQTT only when online and connected to Wi-Fi.

| Area | Implementation |
| --- | --- |
| Boot/mode | `main/hub_station.c`, `components/app_mode_manager` |
| Wi-Fi and BLE | BLUFI in `components/blufi_manager` |
| MQTT/TLS and ESP-NOW routing | `components/mqtt_manager` |
| Pending telemetry, device registry and configuration | `components/esfera_manager` |
| Time | `components/time_sync` |
| Physical reset | `components/button_manager` |
| Device detection | `components/detector_manager` |

ESP-NOW reception is copied into a 64-element FreeRTOS queue and processed by `espnow_worker`; the radio and receive path stay active in online and offline modes. Wi-Fi reconnect is requested only in online mode. MQTT uses mutual TLS and the native client reconnect behavior.

## Partition baseline

The 8 MiB flash is configured with one 24 KiB NVS partition, 4 KiB PHY data and one 1.5 MiB factory application. There are no OTA slots. The validated binary occupies about 95% of the factory partition. IRAM is also at 98.19% (2,377 bytes free). The partition table must not be changed without a migration/recovery plan, and further IRAM growth needs close monitoring.

## Corrections applied

- ESP-NOW processing no longer depends on MQTT connecting first.
- SNTP has a bounded startup wait and is initialized once.
- Wi-Fi reconnect follows the persisted online/offline mode.
- MQTT and ESP-NOW payload lengths, topics and sender MACs are validated.
- Unknown ESP-NOW senders cannot submit telemetry or control messages before registration.
- Pending telemetry survives reboot and is removed after MQTT QoS 1 confirmation.
- Offline BLUFI accepts the same data request and configuration JSON used online.
- Wi-Fi and MQTT credentials are no longer printed.
- NVS ACK keys now fit ESP-IDF's key-length limit.

## Residual risks and hardware validation

1. The radio protocol is fixed to channel 6. An online access point must therefore use channel 6 for ESP-NOW coexistence; changing this requires a coordinated irrigation-device protocol decision.
2. BLUFI considers data delivered when its send API accepts the response; there is no application-level BLE ACK. A disconnect at that point may cause data loss. Add request IDs and an app ACK before production.
3. MQTT and BLUFI event handlers still perform JSON/NVS work. ESP-NOW callbacks are short, but the app inputs should eventually feed a shared command queue.
4. The telemetry blob is committed on each accepted sample. This guarantees reboot persistence but needs endurance testing at the real sampling frequency.
5. A valid `HELLO_HUB` self-registers a device. MAC consistency is checked, but ownership/authentication is not defined.
6. Secure boot, flash encryption and OTA are not configured. Product security and update requirements remain open.
7. Build validation passed; radio coexistence, broker, app, reboot and power-loss tests require the physical Hub and at least one irrigation device.
