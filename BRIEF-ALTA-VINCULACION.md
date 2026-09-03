> [!NOTE]
> **Implementado (2026-09-02).** Este brief esta en el codigo desde `ccef524`: nombres `SG0-`/`SG1-`,
> `provisioning_state`, alta en dos fases con token de 16 bytes, politica de advertising y pulsacion
> corta del boton. Sigue **fuera de alcance** `NEGOTIATE_SECURITY`, asi que el token continua viajando
> en claro. Se conserva como especificacion de origen. Estado actual: `D:\Firmware\SISTEMA-SMARTGROW.md`.

# Brief 3 — Alta, vinculación y token

> Destinatario: agente de firmware del HUB.
> Antecedentes: `BRIEF-OFFLINE-FIRMWARE.md` (ya implementado y verificado en banco).
> La app Android se modifica en paralelo. **Los contratos JSON de este documento son vinculantes**: se implementan contra ellos de los dos lados.

Este brief habilita el alta en modo offline y cierra el agujero de que cualquier teléfono pueda tomar un hub ya vinculado. Igual que antes: **no se flashea**. Entregá el código compilado y el reporte.

---

## 1. El hub declara su identidad y su estado en el nombre BLE

Hoy todos los hubs se anuncian con la constante `BLUFI_DEVICE_NAME` del IDF ([blufi_init.c:215](components/blufi_manager/blufi_init.c:215)), así que en la lista de la app son indistinguibles entre sí y de uno ya dado de alta.

El nombre pasa a ser:

| Nombre | Estado |
|---|---|
| `SG0-885721A8791C` | Virgen, acepta el alta |
| `SG1-885721A8791C` | Ya vinculado |

La MAC es la Wi-Fi STA (`mac_local`), 12 hex en mayúsculas, sin separadores. El nombre **se cambia en caliente** al confirmar el alta, sin reiniciar.

Con esto la app conoce la MAC del hub antes de conectarse, en los dos modos. No hace falta ningún comando `get_id`.

## 2. `provisioning_state`, separado de `mode`

Son dos cosas distintas y hoy están mezcladas. Nuevo estado en NVS:

| Estado | Significado |
|---|---|
| `VIRGEN` | Sin dueño. Se anuncia `SG0-`. Acepta `begin_alta` y nada más |
| `PENDING` | Alta en curso: token emitido pero sin confirmar. Sigue anunciándose `SG0-` |
| `PROVISIONED` | Vinculado. Se anuncia `SG1-`. Exige token en todo comando |

`mode` (online/offline) sigue existiendo aparte y solo decide qué servicios corren.

Tras un `nvs_flash_erase` el estado vuelve a `VIRGEN`, que es lo correcto: un hub reseteado tiene que poder darse de alta otra vez.

## 3. Alta en dos fases

El punto de esto: si el hub se marcara vinculado al generar el token y el BLE se cortara antes de que la app lo guarde, quedaría en `SG1-` con un token que no tiene nadie. Invisible para el alta y sin dueño, rescatable solo con el botón físico.

**Paso 1 — la app abre el alta**

```json
{"cmd":"begin_alta","mode":"online"}     // o "offline"
```

El hub, si está `VIRGEN`:
- genera 16 bytes aleatorios (`esp_fill_random`), los guarda en NVS
- fija `mode` al valor recibido
- pasa a `PENDING` con un timeout de **120 s**
- **sigue anunciándose `SG0-`**

```json
{"cmd":"begin_alta","status":"ok","mac":"885721A8791C","tok":"<32 hex>"}
```

Si ya está `PROVISIONED`, responde esto **sin exigir token**: el nombre `SG1-` ya expone ese estado, así que la respuesta no revela nada nuevo.

```json
{"cmd":"begin_alta","status":"error","error":"already_provisioned"}
```

Si ya está `PENDING`, **reinicia el alta**: emite un token nuevo, descarta el anterior y reinicia el timeout. Cubre el caso de que la app no haya recibido la respuesta y reintente.

**Paso 2 — según el modo**

- **online**: la app hace el aprovisionamiento BluFi de siempre. El hub conecta al Wi-Fi y manda su MAC como custom data al obtener IP, como hoy.
- **offline**: la app manda `set_time`. No se toca el Wi-Fi.

**Paso 3 — la app confirma**

```json
{"cmd":"confirm_alta","tok":"<32 hex>"}
```

El hub, si el token coincide y está en `PENDING`:
- pasa a `PROVISIONED`
- se renombra a `SG1-<MAC>`
- si el modo es online, **detiene el advertising**

```json
{"cmd":"confirm_alta","status":"ok"}
```

Errores de `confirm_alta`:

| Situación | Respuesta |
|---|---|
| Token incorrecto estando `PENDING` | `{"status":"error","error":"unauthorized"}` |
| Estando `VIRGEN` o `PROVISIONED` | `{"cmd":"confirm_alta","status":"error","error":"invalid_state"}` |

**Si el timeout vence sin `confirm_alta`**: vuelve a `VIRGEN`, descarta el token, sigue en `SG0-`.

**Si el hub arranca y encuentra `PENDING`**: descarta el token y vuelve a `VIRGEN` inmediatamente. El timeout vive en RAM y no sobrevive a un corte de energía; sin esta regla un hub podría quedar atrapado en un alta a medias. Es preferible que el usuario repita el alta a que el hub quede inservible.

**Persistencia transaccional.** `begin_alta` toca token, modo y estado. Guardarlos en ese orden con el estado al final y un solo `nvs_commit()`, de modo que el estado sea el marcador de validez: si el commit falla, el hub sigue `VIRGEN`. En `confirm_alta`, `PROVISIONED` se publica en RAM recién después de que el commit haya terminado bien.

## 4. El token protege el canal

Con `provisioning_state` en `PENDING` o `PROVISIONED`, **todo** custom data debe traer `"tok"` con el token correcto:

```json
{"cmd":"set_time","unix":1788290792,"tz":"CET-1CEST,M3.5.0/2,M10.5.0/3","tok":"<32 hex>"}
{"Data":true,"max":50,"tok":"<32 hex>"}
{"cmd":"ack_data","page_id":123,"last_seq":9842,"tok":"<32 hex>"}
```

Sin token o con token incorrecto:

```json
{"status":"error","error":"unauthorized"}
```

Único comando exento: `begin_alta` cuando el estado es `VIRGEN`.

Hoy cualquiera que se conecte a un hub offline puede llevarse su telemetría —y borrarla, porque el ACK avanza la cola—, cambiarle el riego a las plantas de otro o romperle el reloj. El token cierra eso.

**Esto no aplica a MQTT.** Ahí el canal ya está autenticado por el broker.

## 5. Advertising según el estado

En `blufi_manager.c:530` y `:545` el advertising se reenciende siempre, sin condición.

| Estado y modo | Advertising |
|---|---|
| `VIRGEN` o `PENDING` | Encendido |
| `PROVISIONED` + online | **Apagado**. Se reactiva 2 min con la pulsación corta del botón |
| `PROVISIONED` + offline | Encendido — el BLE es el canal de datos, no puede apagarse |

El hub offline vinculado sigue visible, pero se anuncia `SG1-`, así que la app de cualquier teléfono lo excluye de la lista de alta, y el firmware además rechaza el `begin_alta`.

## 6. Pulsación corta del botón

Hoy no hace nada ([button_manager.c:116](components/button_manager/button_manager.c:116), con un `TODO`). Pasa a ser: **hacerse visible 2 minutos**, o sea reactivar el advertising en un hub online vinculado. Es la vía para revincular sin borrar las esferas emparejadas.

## 7. Confirmar que el reset limpia la vinculación

La pulsación de 8 s ya borra NVS y la partición `telemetry`. Verificar que con eso caen también el token y `provisioning_state`, y que el hub vuelve a anunciarse `SG0-`.

---

## Fuera del alcance de este brief

**`NEGOTIATE_SECURITY`.** El cifrado de BluFi es lo que protege al token en el único momento en que viaja, así que corresponde activarlo. Pero es un **cambio coordinado**: si el firmware lo activa y la app no, no conecta nada. Va en un paso aparte, al final, cuando todo lo demás esté andando, y se despliega en los dos lados a la vez.

**OTA.** Sigue reservado el espacio y sin código.

## Qué reportar

1. Build limpio y tamaño del binario contra `ota_0`.
2. Que el renombrado BLE en caliente funciona con Bluedroid, sin reiniciar.
3. Qué pasa si llega `confirm_alta` con un token que no coincide, y si llega estando en `VIRGEN`.
4. Que un hub en `PROVISIONED` rechaza `begin_alta` y responde `already_provisioned`.
5. Plan de prueba: alta online completa, alta offline completa, corte del BLE entre `begin_alta` y `confirm_alta` (tiene que volver a `VIRGEN` tras el timeout), comando sin token estando `PROVISIONED`, y reset de 8 s devolviendo el hub a `SG0-`.
