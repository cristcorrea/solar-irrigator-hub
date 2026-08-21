# NVS map

Partition: `nvs`, offset `0x9000`, size `0x6000` (24 KiB).

| Namespace | Key | Type | Owner | Deletion rule |
| --- | --- | --- | --- | --- |
| `hub_config` | `work_mode` | `u8` (`0` offline, `1` online) | app mode manager | Full factory reset only |
| `telemetry` | `pending` | versioned blob, max. 32 records | esfera manager | Oldest delivered records after MQTT PUBACK; after accepted BLUFI send |
| `esferas` | 12-hex-character device MAC | string `registered` | esfera manager | Three-second button reset or full reset |
| `config_store` | 12-hex-character device MAC | canonical JSON string | esfera manager | Replaced by new config; three-second button reset or full reset |
| `config_store` | `a` + 12-character MAC | `u8` ACK flag | MQTT/ESP-NOW flow | Reset to `0` with new config; set to `1` on `CFG_OK` |

ESP-IDF also uses the default NVS partition internally for persisted Wi-Fi configuration and BLE bonding. Those keys are private implementation details and must not be edited by application code.

The eight-second button action erases the whole default NVS partition, including Wi-Fi credentials, mode and pending telemetry. The three-second action only erases `esferas` and `config_store`.

The telemetry blob contains a magic value, schema version, record count and up to 32 fixed records. An incompatible/corrupt blob is not loaded. No partition migration was introduced.

