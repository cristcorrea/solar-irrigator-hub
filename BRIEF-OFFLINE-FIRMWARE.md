# Brief de implementación — almacenamiento de telemetría y particiones

> Destinatario: agente de firmware del HUB.
> Contexto completo: `ARQUITECTURA.md`, `SISTEMA-SMARTGROW.md`.
> La app Android se modifica en paralelo y por separado; los contratos JSON de este documento son **vinculantes** para ambos lados.

Trabajo dividido en dos etapas. **No empezar la etapa 2 hasta que la etapa 1 esté aprobada.**

**En ninguna de las dos etapas se flashea.** El flasheo y cualquier borrado de datos son una operación aparte, manual y consciente. Entregá el código compilado y el reporte; nada más.

---

## Condición de entrada

Antes de tocar nada, sobre un hub real:

```
esptool.py flash_id
```

- **8 MB** → seguir con la tabla de la etapa 1.
- **4 MB u otro valor** → **parar y reportar**. La tabla propuesta no entra y hay que rediseñarla (ranuras de 1,5 MB, y probablemente migrar de Bluedroid a NimBLE para recuperar espacio). No improvisar una tabla alternativa sin confirmarla.

El `sdkconfig` declara 8 MB, pero eso es configuración, no medición. Caso a descartar: el ESP32-WROVER tiene 4 MB de flash y 8 MB de PSRAM.

---

# Etapa 1 — Solo particiones

**Alcance:** únicamente `partitions.csv` y la verificación de que compila. **No modificar ningún `.c` ni `.h`.** No implementar OTA.

## Tabla a aplicar

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0xE000     # 56 KB
otadata,    data, ota,     0x17000,  0x2000     # 8 KB
phy_init,   data, phy,     0x19000,  0x1000     # 4 KB
ota_0,      app,  ota_0,   0x20000,  0x200000   # 2 MB
ota_1,      app,  ota_1,   0x220000, 0x200000   # 2 MB
telemetry,  data, 0x40,    0x420000, 0x080000   # 512 KB
```

Notas de diseño, para que no se reinterprete:

- **`ota_0` y `ota_1` quedan reservadas y vacías a propósito.** No se implementa OTA en este trabajo. Solo se reserva el espacio ahora, porque cambiar la tabla más adelante exige abrir cada equipo. Con `otadata` borrada y sin partición `factory`, el bootloader arranca la primera ranura OTA; el firmware se flashea en `ota_0` y arranca normal.
- **`telemetry` es una partición raw**, subtipo `0x40`. No es NVS, no es SPIFFS, no lleva sistema de archivos. Se accede con la API `esp_partition_*`.
- **NVS mantiene el offset `0x9000` y crece a 56 KB.** No se asume que los datos actuales sobrevivan: en prototipos se hará `erase-flash` y alta nueva.

## Qué verificar y reportar

1. `idf.py build` compila sin errores.
2. Salida de `idf.py partition-table`: confirmar offsets, tamaños y alineación (las particiones `app` deben quedar alineadas a 0x10000).
3. Tamaño del binario resultante contra los 2.097.152 bytes de `ota_0`, en bytes y en porcentaje. Referencia actual: 1.535.136 bytes sobre 1.572.864 (97,6 %).
4. Cualquier warning nuevo del build o del bootloader.

## Criterio de aceptación

Compila, los offsets coinciden con la tabla, y el binario entra en `ota_0` con margen. Reportar y **detenerse**.

---

# Etapa 2 — Código de almacenamiento

Solo con la etapa 1 aprobada.

## 2.1 · Reescribir `esfera_manager` como log circular

Reemplaza por completo el blob de NVS actual. El enfoque de hoy —`telemetry_blob_t` como variable local en `persist_entries_locked` (`esfera_manager.c:34`) y `nvs_set_blob` del bloque entero en cada lectura— se elimina.

**Registro, 20 bytes:**

```c
typedef struct __attribute__((packed)) {
    uint32_t ts;        // epoch UTC
    uint8_t  mac[6];    // binaria
    int16_t  hum;       // décimas de %
    int16_t  temp;      // décimas de °C
    uint16_t vbat;      // mV
    uint8_t  riego;     // 0/1
    uint8_t  flags;     // bit0 = ts válido (reloj sincronizado al registrar)
    uint16_t crc;       // CRC16 sobre los 18 bytes previos
} telemetry_rec_t;
```

**Sector:** 4096 bytes con cabecera de 44 (magic 4, número de secuencia 4, reservado 4, **mapa de confirmación 32**) ⇒ **202 registros por sector**. 128 sectores, uno siempre libre para rotar ⇒ **25.654 registros útiles**.

### Cómo se marca lo confirmado

Una página son 50 registros y un sector tiene 202, así que un ACK confirma **parte** de un sector. Como la flash solo borra sectores enteros, hace falta anotar hasta dónde se entregó — y hay que hacerlo sin copiar ni compactar nada.

La propiedad que lo resuelve: en flash NOR **un bit se puede pasar de 1 a 0 sin borrar el sector**. Es lo que usa NVS internamente para sus estados de entrada.

- La cabecera lleva un **mapa de bits, uno por registro** (202 bits en 32 bytes). Tras borrar el sector todos valen 1 = pendiente.
- Confirmar una página es **apagar sus bits**, reescribiendo la palabra de 32 bits correspondiente. Solo pasa bits de 1 a 0, así que es una escritura legal sin borrado previo. `esp_partition_write` exige offset y tamaño alineados a 4 bytes: por eso se reescribe de a palabras completas.
- **El CRC del registro no cubre el mapa**, que vive en la cabecera. Apagar bits nunca invalida un registro.
- Un sector con todos los bits apagados está libre y se puede borrar al rotar.
- **El ACK sale idempotente solo:** repetirlo apaga bits que ya estaban apagados y no cambia nada. No hace falta lógica extra para eso.

Esto exige que **el cifrado de flash esté desactivado**, cosa que hoy se cumple (no hay `CONFIG_SECURE_FLASH_ENC_ENABLED` en `sdkconfig`). Si alguna vez se activa, hay que rediseñar este punto.

**No compactar.** Copiar los registros restantes a otro sector en cada ACK parcial escribiría ~3 KB por cada 50 lecturas confirmadas: más amplificación de escritura que el blob de NVS que estamos reemplazando, y con la recuperación ante cortes en mitad de la copia como parte más delicada. El mapa de bits evita todo eso.

**Reglas no negociables:**

- **`head` —el puntero de escritura— NO se persiste.** Se reconstruye escaneando la partición al arrancar, con el número de secuencia de sector y el CRC de cada registro. Ahí el riesgo era escribir el registro y no llegar a actualizar un índice guardado aparte.
- **`tail` —lo confirmado— sí se persiste, pero dentro del propio sector**, en el mapa de bits de su cabecera. Lo que hay que evitar es un índice en NVS que se desincronice del log; un marcador pegado a los datos no puede desincronizarse.
- **Un registro con CRC inválido es una escritura parcial** y se descarta; marca el fin del área válida de ese sector.
- **Log lleno ⇒ se sobrescribe lo más viejo.** Nunca descartar lo nuevo, que es lo que hace hoy `esfera_manager_add`.
- **La escritura sale del camino de ESP-NOW.** El worker de recepción solo encola; una tarea dedicada escribe en flash. Esa tarea mantiene **siempre un sector borrado por delante**, de modo que ningún `esp_partition_erase_range` caiga dentro del camino crítico de recepción.
- **Sin migración del blob viejo.** Si `esfera_manager_init` encuentra la clave NVS `telemetry/pending`, la borra y sigue.

**API sugerida:**

```c
esp_err_t esfera_manager_init(void);
esp_err_t esfera_manager_add(const char *raw_payload, const char *mac_origen);
char     *esfera_manager_page_json(size_t max, uint32_t *page_id,
                                   uint32_t *last_seq, size_t *pend);
esp_err_t esfera_manager_ack(uint32_t page_id, uint32_t last_seq);
esp_err_t esfera_manager_erase_all(void);
size_t    esfera_manager_count(void);
```

## 2.2 · Paginación — en BLE **y** en MQTT

`esfera_manager_generate_json` construye hoy el array completo en heap. Con 25.908 registros son más de 3 MB: no entra. Esto aplica igual al `Data:true` de MQTT (`mqtt_manager.c`) que al de BluFi (`blufi_manager.c:139`); no es un problema exclusivo del BLE.

**Petición** (idéntica en ambos transportes):

```json
{"Data": true, "max": 50}
```

Si falta `max`, asumir 50. Techo duro 100.

**Respuesta:**

```json
{
  "page_id": 123,
  "last_seq": 9842,
  "pend": 417,
  "items": [
    {"mac":"112233445566","humedad":55.1,"temperatura":21.3,
     "bateria":3.92,"riego":0,"timestamp":"2026-08-23T10:00:00","tsOk":true}
  ]
}
```

Los nombres de campo de `items` se conservan tal como están hoy para no romper el parser de la app. `tsOk` es `flags.bit0`: cuando es `false`, la app muestra la lectura como hora estimada.

**Confirmación:**

```json
{"cmd":"ack_data","page_id":123,"last_seq":9842}
→ {"cmd":"ack_data","status":"ok","pend":417}
```

**El ACK debe ser idempotente.** Con el mapa de bits sale gratis: confirmar dos veces la misma página apaga bits ya apagados y no avanza nada. Un `page_id` desconocido o anterior al último confirmado se responde `ok` sin efecto. Lo que nunca puede pasar es que un ACK repetido o tardío tras una reconexión descarte lecturas que no llegaron.

**Nunca avanzar la cola al enviar.** Hoy el camino BLE borra en cuanto transmite: eso se elimina. En MQTT, el avance sigue atado al PUBACK.

## 2.3 · Reset de fábrica

`nvs_flash_erase()` no toca una partición raw, así que hoy el "borra todo" del botón sería falso.

- **Pulsación ≥ 3 s:** además de `esferas` y `config_store`, borrar **el log completo** (`esfera_manager_erase_all`). El borrado selectivo por MAC obligaría a compactar un buffer circular; no se hace. Si no se borrara, quedarían lecturas de esferas que el hub ya no reconoce.
- **Pulsación ≥ 8 s:** `nvs_flash_erase()` **más** `esp_partition_erase_range` sobre `telemetry`, y recién ahí reiniciar.

No tocar la pulsación corta en esta etapa.

## 2.4 · Dos bugs a corregir de paso

- **`hub_station.c:224`** — quitar `app_mode_manager_set(APP_MODE_ONLINE)`. Pisa el modo guardado en cada arranque, así que un hub offline que se reinicia entra en bucle de reconexión Wi-Fi contra una red que no existe, y cada reintento escanea todos los canales rompiendo ESP-NOW.
- **`blufi_manager.c:506`** — el canal 6 se fija solo en `blufi_init`. Fijarlo también al cambiar a offline en caliente, para que el canal en runtime y el de después de un reinicio coincidan.

## 2.5 · Persistencia del reloj

Sin esto, un corte de luz deja el hub en 1970 y las esferas dejan de regar hasta que alguien se acerque con el teléfono.

- Persistir en NVS el epoch (cada ~10 min), la cadena `TZ` en formato POSIX y un flag `time_valid`.
- Al arrancar: restaurar con `settimeofday()`, reponer `TZ` y llamar a `tzset()`.
- El reloj restaurado queda atrasado lo que duró el corte; se corrige en la siguiente sincronización. Los registros ya guardados conservan su marca: para eso existe `flags.bit0`.

---

## Fuera del alcance de este brief

Va en un brief posterior, porque exige cambios sincronizados en la app:

- Nombre BLE `SG0-`/`SG1-<MAC>` y apagado del advertising
- `provisioning_state`, token de vinculación y alta en dos fases
- Activar `NEGOTIATE_SECURITY`
- Pulsación corta del botón
- OTA, en cualquier forma

## Qué reportar al terminar la etapa 2

1. Build limpio y tamaño del binario contra `ota_0`.
2. Cómo se reconstruye `head` al arrancar, cómo se lee el mapa de confirmación, y qué pasa ante un registro con CRC roto.
3. Tiempo medido de `esp_partition_erase_range` de un sector, para dimensionar la cola.
4. Heap libre real al armar una página de 50 registros, con Bluedroid y mbedTLS cargados. Si queda ajustado, proponer un `max` por defecto menor.
5. Plan de prueba: llenado del log, vuelta completa del circular, corte de energía a mitad de escritura, **reinicio con un sector confirmado a medias**, ACK duplicado y ACK fuera de orden.
