> [!WARNING]
> **Documento desactualizado (2026-09-02).** El dump ya no es un array plano sino un objeto paginado
> con `page_id`, `last_seq`, `pend` e `items`, y el borrado exige `ack_data` explicito. El catalogo
> BLE esta incompleto: faltan `ack_data`, `begin_alta`, `confirm_alta`, el parametro `max` y el
> campo `tok` obligatorio. Contratos reales: `D:\Firmware\SISTEMA-SMARTGROW.md`, secciones 3 a 5.

# Communication message formats

## MQTT

- Subscribe: `ismart/hub/<MACHUB>`
- Publish: `ismart/app/<MACHUB>`
- `<MACHUB>` is the 12-character uppercase station MAC without separators.
- Incoming payloads must be complete, unfragmented JSON smaller than 256 bytes and on the exact subscription topic.

Data request:

```json
{"Data":true}
```

Configuration requires a valid `MACSLAVE` (12 hexadecimal characters, with or without colons). Existing application fields are preserved. The canonical payload plus the timestamp added for ESP-NOW must fit 250 bytes.

Pending telemetry is published as a JSON array with `mac`, `humedad`, `temperatura`, `bateria`, `riego` and `timestamp`. Stored records are removed only after the matching QoS 1 publish event.

## BLUFI custom data

BLUFI accepts the same `{"Data":true}` request and configuration object as MQTT. It also accepts:

```json
{"cmd":"get_mode"}
{"cmd":"set_mode","mode":"online"}
{"cmd":"set_mode","mode":"offline"}
{"cmd":"set_time","unix":1781992800,"tz":"CET-1CEST,M3.5.0/2,M10.5.0/3"}
```

Inbound custom data is limited to 512 bytes; configuration remains limited to 255 bytes by the common storage validator.

## ESP-NOW

- Discovery from Hub: `HELLO_ESFERA,<MACHUB>`
- Pairing from device: `HELLO_HUB,<MACSLAVE>`; the declared MAC must equal the radio sender MAC.
- Configuration acknowledgement: `CFG_OK` or `CFG_ERR[:reason]`.
- Final Hub acknowledgement: `ACK_END`.
- Telemetry: `<humidity>,<temperature>,<voltage>,<irrigation> <MACSLAVE>`.

Telemetry constraints are humidity 0–100, temperature -50–100 °C, voltage 0–20 V and irrigation 0/1. The payload MAC must match the ESP-NOW sender. Receive payloads of 128 bytes or more are rejected rather than truncated.

