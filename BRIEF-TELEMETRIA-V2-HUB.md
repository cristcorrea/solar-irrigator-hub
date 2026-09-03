# Brief — Telemetría v2, estado de configuración y cierre del canal BLE

> Destinatario: agente de firmware del HUB (`solar-irrigator-hub`, ESP32).
> Contraparte: `solar-irrigator-slave/BRIEF-TELEMETRIA-V2.md`, que se implementa en paralelo.
> **El contrato de la sección 1 es vinculante**: los dos lados se implementan contra él, carácter a
> carácter. La app Android se modifica también en paralelo contra los contratos de las secciones 1, 3 y 6.
> Contexto del sistema completo: `D:\Firmware\SISTEMA-SMARTGROW.md`.
>
> Antecedentes ya implementados: `BRIEF-OFFLINE-FIRMWARE.md` y `BRIEF-ALTA-VINCULACION.md`.
> Como siempre: **no se flashea**. Entregá el código compilado y el reporte.

El trabajo va en dos etapas. **No empezar la etapa 2 hasta que la etapa 1 esté aprobada**, porque la
etapa 1 cambia el formato del registro persistido y conviene validarla aislada.

---

# Etapa 1 — Telemetría v2

## 1.1 Contrato de telemetría (vinculante)

La esfera pasa a enviar:

```
hum,temp,vbat,riego,ml,st MACESFERA
```

Ejemplo: `45.6,23.4,4.12,1,148,17 A0B1C2D3E4F5`

| Campo | Formato | Rango a validar | Significado |
|---|---|---|---|
| `hum` | `%f` | 0 … 100 | % HR del aire. **0.0 significa "sensor caído"** si el bit 0 de `st` está apagado |
| `temp` | `%f` | -50 … 100 | °C. Igual que arriba |
| `vbat` | `%f` | 0 … 20 | Volts |
| `riego` | `%d` | 0 o 1 | 1 = la esfera **intentó** regar en este ciclo |
| `ml` | `%d` | 0 … 65535 | ml realmente entregados según el caudalímetro |
| `st` | `%d` | 0 … 255 | Mapa de bits |

Bits de `st`: 0 = AHT20 válido · 1 = riego cortado por falta de caudal · 2 = batería en aviso ·
3 = riego omitido por batería baja · 4 = la esfera tiene hora válida · 5-7 reservados.

**El hub no interpreta `st`**: lo valida, lo persiste y lo reenvía. Interpretarlo es tarea de la app.

## 1.2 Parser: aceptar las dos versiones

El parser actual exige exactamente 5 campos y rechaza un sexto con el guarda `%c`
([telemetry_log.c:485](components/esfera_manager/telemetry_log.c:485)):

```c
sscanf(raw_payload, "%f,%f,%f,%d %12s %c", &h, &t, &v, &r, claimed, &trailing) != 5
```

Pasa a intentar primero el formato v2 y caer al v1 si no encaja:

```c
/* v2: 7 conversiones exitosas, la 8ª (%c) debe fallar */
int n = sscanf(raw, "%f,%f,%f,%d,%d,%d %12s %c", &h,&t,&v,&r,&ml,&st, claimed, &trailing);
if (n != 7) {
    /* v1: firmware de esfera anterior a este brief */
    n = sscanf(raw, "%f,%f,%f,%d %12s %c", &h,&t,&v,&r, claimed, &trailing);
    if (n != 5) return ESP_ERR_INVALID_ARG;
    ml = 0;
    st = 0x01;              /* solo bit0: el firmware v1 únicamente enviaba con el sensor sano */
}
```

Las validaciones existentes se conservan tal cual (rangos, `isfinite`, y que `claimed` coincida con
la MAC radio del remitente). Se añaden `ml <= 65535` y `st <= 255`.

La compatibilidad hacia atrás importa porque el hub y las esferas se actualizan por separado y a
mano: hay que poder desplegar el hub primero sin dejar mudas a las esferas.

## 1.3 Registro persistido: 24 bytes

```c
typedef struct __attribute__((packed)) {
    uint32_t ts;        /* epoch UTC                                   offset 0  */
    uint8_t  mac[6];    /*                                                     4  */
    int16_t  hum;       /* décimas de %                                       10  */
    int16_t  temp;      /* décimas de °C                                      12  */
    uint16_t vbat;      /* mV                                                 14  */
    uint16_t ml;        /* NUEVO: ml entregados                               16  */
    uint8_t  riego;     /* 0/1                                                18  */
    uint8_t  st;        /* NUEVO: estado reportado por la esfera              19  */
    uint8_t  flags;     /* bit0 = reloj del HUB válido al registrar           20  */
    uint8_t  reserved;  /* 0                                                  21  */
    uint16_t crc;       /* CRC16-CCITT sobre los 22 bytes previos             22  */
} telemetry_rec_t;      /* 24 bytes */
```

Consecuencias que hay que aplicar en el mismo cambio:

| Constante | Antes | Ahora |
|---|---|---|
| `_Static_assert(sizeof(telemetry_rec_t) == …)` | 20 | **24** |
| `RECORDS_PER_SECTOR` = (4096 − 44) / tamaño | 202 | **168** |
| Capacidad útil = (128 − 1) × registros | 25.654 | **21.336** |
| `SECTOR_MAGIC` | `0x5347544C` | **`0x5347544D`** |

El mapa de confirmación de la cabecera sigue siendo de 32 bytes (256 bits), que cubre de sobra los
168 registros.

**El cambio de magic es deliberado**: hace que `scan_partition()` ignore los sectores del formato
viejo y arranque con el log vacío, en vez de leer basura interpretada con el tamaño equivocado. No se
implementa migración: en prototipos se hace `erase-flash`. Verificá que el camino de "partición sin
ningún sector válido" funciona (es el mismo que el de una partición recién borrada).

`last_seq` pasa a valer `sector.seq * 168 + slot`. Es un valor opaco para la app, así que el cambio no
rompe nada, y como el log arranca vacío no hay problema de continuidad.

## 1.4 Página JSON

Los nombres de campo existentes **no se tocan**. Se añaden dos:

```json
{
  "page_id": 123, "last_seq": 9842, "pend": 417,
  "items": [
    {"mac":"112233445566","humedad":55.1,"temperatura":21.3,"bateria":3.920,
     "riego":1,"ml":148,"st":17,"timestamp":"2026-08-23T10:00:00","tsOk":true}
  ]
}
```

`PAGE_JSON_ITEM_SIZE` sube de **160 a 200**. Con `max = 100` eso son 128 + 20.000 ≈ **20 KB de heap**
por página, contra los ~16 KB de hoy. **Medí el heap libre real** al armar una página de 100 con
Bluedroid y mbedTLS cargados; si queda ajustado, proponé bajar el techo duro `PAGE_MAX` de 100 a 75 y
reportalo — no lo cambies por tu cuenta, porque el techo es parte del contrato con la app.

Idéntico en BLE y en MQTT, como hasta ahora: los dos usan `esfera_manager_page_json`.

---

# Etapa 2 — Estado de configuración y cierre del canal BLE

## 2.1 Sacar `tok` y `Data` de la configuración almacenada

Hoy `esfera_manager_store_config` canonicaliza **todo el objeto** que le llega, así que cuando la
config entra por BLE el token de vinculación se guarda en `config_store` y después **se retransmite en
claro por ESP-NOW a la esfera** ([blufi_manager.c:292](components/blufi_manager/blufi_manager.c:292) →
[esfera_manager.c:301-317](components/esfera_manager/esfera_manager.c:301) →
[mqtt_manager.c:254](components/mqtt_manager/mqtt_manager.c:254)).

Verificado: la esfera solo parsea `ts`, `diasRiego`, `horaRiego` y `ml`; no usa `tok` ni `Data` para
nada. Y el hub valida el token contra `hub_config/prov_token`, no contra `config_store`. Así que se
pueden borrar sin ningún efecto colateral.

En `esfera_manager_store_config`, después de parsear y **antes** de `cJSON_PrintUnformatted`:

```c
cJSON_DeleteItemFromObjectCaseSensitive(json, "tok");
cJSON_DeleteItemFromObjectCaseSensitive(json, "Data");
```

Hacerlo ahí y no en `blufi_manager.c` cubre de una vez los dos transportes. Efecto secundario bueno:
el JSON que viaja por ESP-NOW baja de ~175 a ~127 bytes, con lo que el margen contra el límite de
250 pasa de 55 a más de 100 bytes.

## 2.2 Puerta de autorización en el aprovisionamiento Wi-Fi

Los eventos BluFi nativos (`SET_WIFI_OPMODE`, `RECV_STA_SSID`, `RECV_STA_PASSWD`, `RECV_STA_BSSID`,
`REQ_CONNECT_TO_AP`, `REQ_DISCONNECT_FROM_AP`) se aceptan hoy **sin autorización de ningún tipo**
([blufi_manager.c:723-809](components/blufi_manager/blufi_manager.c:723)). El protocolo BluFi no
tiene dónde meter un token en esos eventos, así que la autorización va **por sesión BLE**:

```c
static bool s_session_authorized;   /* vale para la conexión BLE en curso */
```

| Momento | Acción |
|---|---|
| `ESP_BLUFI_EVENT_BLE_CONNECT` y `..._BLE_DISCONNECT` | `s_session_authorized = false` |
| Custom data que supera la comprobación de token | `s_session_authorized = true` |
| `confirm_alta` correcto | `s_session_authorized = true` |

Regla en los eventos Wi-Fi listados:

```
si (provisioning_manager_state() == PROVISIONED && !s_session_authorized)
    → log + blufi_send_error_only("unauthorized") + ignorar el evento
si no
    → comportamiento actual
```

Con `VIRGEN` o `PENDING` se permite siempre: es el camino del alta.

**No hay regresión en el alta online.** La app hace `begin_alta` → `confirm_alta` → aprovisionamiento,
todo sobre la misma conexión BLE, así que para cuando llegan el SSID y la contraseña la sesión ya
está autorizada.

El impacto que esto cierra no es el robo de datos: es que hoy cualquiera dentro del alcance BLE de un
hub **offline vinculado** —que anuncia permanentemente por diseño— puede forzarle una conexión Wi-Fi,
y eso le cambia el canal de radio ([blufi_manager.c:544](components/blufi_manager/blufi_manager.c:544))
y **le rompe el ESP-NOW con todas las esferas**. Es una denegación de servicio sobre el riego.

## 2.3 `get_cfg_status`: exponer si la esfera aplicó la configuración

El hub ya sabe esto y lo persiste: `cfg_ack_set_applied()` pone `config_store/a<MAC12>` a 1 cuando
llega `CFG_OK` ([mqtt_manager.c:120-134](components/mqtt_manager/mqtt_manager.c:120)), y
`esfera_manager_store_config` lo pone a 0 con cada config nueva. El problema es que **solo se publica
por MQTT** (`cfgAck`, [mqtt_manager.c:471](components/mqtt_manager/mqtt_manager.c:471) — cero
referencias en `blufi_manager.c`), y encima como mensaje espontáneo: si la app estaba cerrada, se
pierde, porque no hay retained ni sesión persistente.

Se agrega un comando **de consulta, idéntico en los dos transportes**:

**BLE** (con token, como todos):
```json
→ {"cmd":"get_cfg_status","tok":"<32 hex>"}
← {"cmd":"get_cfg_status","status":"ok","items":[
     {"MACSLAVE":"A0B1C2D3E4F5","applied":true},
     {"MACSLAVE":"112233445566","applied":false}]}
```

**MQTT** (topic `ismart/hub/<MACHUB>`, respuesta en `ismart/app/<MACHUB>`, QoS 1):
```json
→ {"MACHUB":"885721A8791C","cmd":"get_cfg_status"}
← {"cmd":"get_cfg_status","status":"ok","items":[…]}
```

Implementación: recorrer las entradas del namespace `config_store` con `nvs_entry_find` /
`nvs_entry_info`, quedarse con las claves de 13 caracteres que empiezan por `a`, y reportar
`MACSLAVE = clave+1` con el valor del `u8`. **Tope de 20 items** por respuesta para acotar el buffer;
si hubiera más, devolver los 20 primeros — no hay instalaciones con esa cantidad de esferas por hub.

Dos detalles de implementación:

- `blufi_send_json_response` usa un buffer fijo de 160 bytes ([blufi_manager.c:81-106](components/blufi_manager/blufi_manager.c:81)). Esta respuesta puede llegar a ~950 bytes, así que necesita un camino propio con buffer dinámico, igual que el de la página de telemetría.
- La respuesta lleva `"cmd"`, y el parser de páginas de la app descarta todo objeto que tenga `"cmd"`. Eso es correcto y deliberado: garantiza que esta respuesta nunca se confunda con una página de datos.

`cfgAck` espontáneo por MQTT **se mantiene** tal como está: sirve para actualizar la UI en vivo. El
comando nuevo es el que cubre el arranque de la app y el modo offline.

## 2.4 Opcional: apagar el Bluetooth clásico

`CONFIG_BT_CLASSIC_ENABLED=y` ([sdkconfig:531](sdkconfig:531), heredado de `sdkconfig.defaults:8`) en
un producto que solo usa BLE. El controlador ya corre en BLE puro
(`CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y`, [sdkconfig:777](sdkconfig:777)) y
`esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` ya libera su RAM, así que lo que sobra es el
**código del host Bluedroid clásico ocupando flash**. No hay ni un uso de APIs clásicas en el
proyecto (verificado sobre `esp_bt_gap_`, `esp_a2d_`, `esp_spp_`, `esp_hf_`, `esp_avrc_`).

No es urgente: el binario está al 73,7 % de `ota_0`. Hacelo **al final**, midiendo el tamaño del
binario antes y después, y verificando que BluFi sigue funcionando igual. Si aparece cualquier
problema, revertilo: no vale la pena arriesgar el canal de alta por unos KB.

---

## Fuera del alcance de este brief

- **OTA.** Sigue reservado el espacio y sin código. Dos reglas mientras tanto: no volver a tocar
  `partitions.csv`, y vigilar que el binario no se acerque a los 2 MB de `ota_0`.
- **Riego bajo demanda** desde la app. Necesita que el hub encole la orden y la entregue en la ventana
  de 2,5 s del próximo contacto. Brief aparte.
- **`NEGOTIATE_SECURITY`** en BluFi. Sigue siendo un cambio coordinado con la app y va al final de
  todo.
- **Cifrado ESP-NOW** (PMK/LMK).
- **Identidad por hub en MQTT.** Hoy el certificado de cliente y las credenciales son globales para
  toda la flota. Es un problema de diseño del backend, no de este firmware.

---

## Qué reportar

1. Build limpio y tamaño del binario contra los 2.097.152 B de `ota_0`, en bytes y porcentaje.
2. Registro capturado en las dos versiones del parser: una esfera con firmware v2 y una con v1,
   conviviendo.
3. Heap libre real al armar una página de 100 registros con el item de 200 bytes. Si queda ajustado,
   cuál `PAGE_MAX` proponés.
4. Que tras el cambio de `SECTOR_MAGIC` el arranque con la partición vieja no se cuelga ni interpreta
   basura: cuántos sectores descarta y en qué estado queda.
5. Que la config guardada en `config_store` ya no contiene `tok` ni `Data`, y el tamaño real del JSON
   que sale por ESP-NOW.
6. Qué pasa si llegan SSID y contraseña por BLE a un hub `PROVISIONED` sin haber enviado antes ningún
   comando con token, y confirmación de que el alta online completa sigue funcionando de punta a
   punta.
7. Respuesta real de `get_cfg_status` por los dos transportes, con dos esferas en estados distintos.
8. Plan de prueba: esfera v1 y esfera v2 en el mismo hub, vuelta completa del log circular con el
   registro de 24 bytes, ACK duplicado, corte de energía a mitad de escritura, y alta online completa
   con la puerta de autorización activa.
