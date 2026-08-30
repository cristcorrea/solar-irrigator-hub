# SmartGrow — Detalles finos, cabos sueltos y punto de partida

> Quinto documento de la serie. Los otros cuatro describen **cómo funciona** el sistema:
> `ARQUITECTURA.md` (hub) · `solar-irrigator-slave/ARQUITECTURA.md` · `smartgrowapp/ARQUITECTURA.md` · `SISTEMA-SMARTGROW.md`
>
> Este describe **qué falta, qué sobra y qué está a medias**. Es el backlog técnico real, sacado de leer los tres códigos línea por línea.

## Cómo leer este documento

Cada hallazgo indica **dónde está**, **qué pasa hoy** y **qué haría falta para cerrarlo**. La severidad es:

| | Significado |
|---|---|
| 🔴 | Una función que el usuario cree que existe, no funciona (o hay riesgo de pérdida de trabajo/datos) |
| 🟠 | Funciona a medias, degrada la experiencia o el consumo |
| 🟡 | Deuda técnica: no rompe nada hoy, pero confunde y hará daño más adelante |
| ⚪ | Limpieza: código muerto, restos de refactor |

---

## 1. Empieza por aquí (los 6 que más pesan)

Si solo vas a tocar seis cosas, que sean estas. El detalle de cada una está más abajo.

| # | Hallazgo | Dónde | Severidad |
|---|---|---|---|
| 1 | Tienes **6 archivos sin commitear** en el hub, y uno de ellos anula el modo offline | hub, working tree | 🔴 |
| 2 | Las **notificaciones nunca se muestran**: el código que las dispara está en una pantalla que nadie renderiza | app | 🔴 |
| 3 | La esfera **siempre reporta `riego=0`** → la alerta "riego fallido" es un falso positivo permanente | esfera → app | 🔴 |
| 4 | **Si el usuario no abre la app, no se recolectan datos** y a partir de 32 lecturas el hub las descarta | app + hub | 🔴 |
| 5 | La **clave privada del cliente MQTT está en el repo** y viaja dentro del APK, junto con la contraseña del broker | app | 🔴 |
| 6 | El **modo offline** está completo en el firmware y **ausente en la app** | app | 🟠 |

---

## 2. Lo primero de todo: trabajo sin guardar

🔴 **El hub tiene 6 archivos modificados sin commitear.** `git status` muestra cambios en `detector_manager.c/.h`, `mqtt_manager.c`, `hub_station.c/.h` y `sdkconfig`. Ese diff **no es trivial**: contiene el descubrimiento con reintentos y notificación (`detector_manager_notify_esfera_response`), el corte del broadcast cuando la esfera responde, el `force_send` de configuración tras recibir telemetría y el alta de peer antes de responder. Es decir, buena parte de lo que hoy hace funcionar el emparejamiento **existe solo en tu disco**.

**Qué hacer:** commitear eso antes que nada. Y decidir el punto siguiente, porque va en el mismo diff.

🔴 **`app_mode_manager_set(APP_MODE_ONLINE)` fuerza modo online en cada arranque** — [main/hub_station.c:224](main/hub_station.c). Es una línea **añadida en ese diff sin commitear**, justo después de `app_mode_manager_init()`, que lee el modo guardado en NVS. El efecto: el modo persistido se pisa en cada reset, así que **la rama `offline-bluetooth-mode` no puede quedarse en offline tras un reinicio**. Tiene toda la pinta de un atajo de depuración que se quedó.

**Qué hacer:** si era temporal, borrarla. Si querías un arranque seguro en online, hacerlo condicional (p. ej. solo si no hay modo válido en NVS) en vez de incondicional.

🟡 **Ramas divergentes**: existe `origin/feature/offline-bluetooth-mode` además de tu `offline-bluetooth-mode` local, más `origin/codex/verificar-último-push-realizado` y `origin/migracion-idf-5.5.3`. Conviene consolidar antes de que el árbol se enrede más.

⚪ El `ARQUITECTURA.md` del slave está sin trackear (lo creé en la sesión anterior).

---

## 3. Cadenas rotas de punta a punta

Estas son las más valiosas: cada una atraviesa dos o tres códigos, y por eso ninguna se ve mirando un solo repo.

### 3.1 🔴 `riego` siempre viaja en 0 (y arrastra dos bugs más)

En [solar_irrigator_slave.c:577](../solar-irrigator-slave/main/solar_irrigator_slave.c) se declara `bool irrigation_done = false;` y **nunca se actualiza**, ni siquiera después de que `pump_controller_irrigate()` riegue de verdad. Ese valor es el que se envía en el CSV de telemetría.

La cadena de consecuencias:

1. El hub guarda y publica siempre `"riego":0`.
2. La app consulta `countIrrigation(... AND irrigation > 0)` — [SampleDao.kt:24](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/data/db/dao/SampleDao.kt) — que **nunca encuentra nada**. Resultado: pasadas 3 h de la hora programada, `AlertLogic` levanta la alerta **CRITICAL "Fallo de riego"** todos los días, aunque el riego haya salido perfecto.
3. La app simula el nivel del depósito descontando `item.irrigation` de `tankLevel` — [HomeViewModel.kt:159,179](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/home/HomeViewModel.kt). Como siempre resta 0, **el depósito no baja nunca** y la alerta de "nivel bajo" jamás se dispara.

**Qué haría falta:** decidir qué significa el campo. Hoy es un booleano ("¿regué en este ciclo?") pero la app lo trata como **mililitros** (`val usage = item.irrigation`, y luego `tankLevel - usage`). Son dos contratos distintos sobre el mismo campo. Lo más útil sería que la esfera reporte **ml realmente entregados** (que ya calcula: `irrigation_pulses_to_ml(pulse_count)` en [pump_controller.c:160](../solar-irrigator-slave/components/pump_controller/pump_controller.c)) y que el firmware del hub y la app lo interpreten igual. Eso cierra el bug de alerta y el del depósito de una vez.

### 3.2 🔴 La recolección de datos depende de que la app esté abierta

Dos decisiones independientes se combinan mal:

- El hub **solo publica cuando se lo piden** (`{"Data":true}`); nunca por iniciativa propia.
- La app **solo mantiene MQTT en primer plano**: `AppLifecycleObserver` conecta en `onStart` y desconecta en `onStop` — [AppLifecycleObserver.kt:8-14](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/service/AppLifecycleObserver.kt).

Con la app cerrada nadie pide datos. Y el buffer del hub tiene **32 entradas**; al llenarse, `esfera_manager_add` **descarta la lectura nueva** y conserva las viejas ([esfera_manager.c:158](components/esfera_manager/esfera_manager.c)). Con dos esferas reportando cada hora son ~16 h de margen: un fin de semana sin abrir la app y se pierde todo lo posterior.

Hay rastros de que esto estaba previsto: en [MainActivity.kt:20](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/MainActivity.kt) hay comentada una línea `startForegroundService(..., MqttService::class.java)` — **y `MqttService` no existe en el proyecto**. El manifiesto ya declara `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_DATA_SYNC`, `FOREGROUND_SERVICE_CONNECTED_DEVICE` y `WAKE_LOCK` para ese servicio fantasma. También está la dependencia `androidx.work.runtime.ktx` (WorkManager) declarada y **sin una sola referencia** en el código.

**Qué haría falta**, en orden de menor a mayor esfuerzo:
1. Cambiar la política del buffer del hub: al llenarse, **descartar la más vieja** en vez de la nueva (rotar), para que lo que se pierda sea lo menos relevante.
2. Ampliar `MAX_ENTRADAS` (ojo con la NVS, ver §4.4).
3. Implementar de verdad el sondeo en segundo plano: o el `MqttService` en primer plano que quedó a medias, o un `Worker` periódico de WorkManager (la dependencia ya está).

### 3.3 🔴 Las notificaciones nunca llegan al usuario

`HomeViewModel` emite alertas por `notificationEvents` y hay una función `showAlertNotification()` que las convierte en notificación del sistema — pero vive dentro de `HomeScreen` ([HomeScreen.kt:44-48](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/home/HomeScreen.kt)), **y `HomeScreen` no la renderiza nadie**: la ruta `Home` del `NavGraph` llama directamente a `HubPagerRoute` ([NavGraph.kt:110-117](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/navigation/NavGraph.kt)). El `import` de `HomeScreen` sigue en el NavGraph (línea 26) pero solo lo menciona un comentario.

O sea: toda la maquinaria de alertas funciona, calcula bien y se muestra en pantalla… pero **el `collect` que dispara las notificaciones nunca se ejecuta**.

Dos problemas adicionales que aparecerían en cuanto lo arregles:
- **`POST_NOTIFICATIONS` nunca se pide en runtime.** Está declarado dos veces en el manifiesto (líneas 18 y 27) y `PermissionsScreen` tiene textos preparados para él (`prettyName`/`descriptionFor`), pero `BlePermissionController.requiredPermissions` no lo incluye, así que la pantalla de permisos no lo solicita. En Android 13+ las notificaciones se descartan en silencio.
- **`NotificationChannels`** ([service/NotificationChannels.kt](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/service/NotificationChannels.kt)) define un canal `"mqtt_foreground"` que **nadie invoca**, mientras `showAlertNotification` crea por su cuenta otro canal `"smartgrow_alerts"` en cada notificación. Dos conceptos de canal, ninguno coordinado.

**Qué haría falta:** mover el `collect` de `notificationEvents` a `HubPagerRoute` (o al `MainActivity`), añadir `POST_NOTIFICATIONS` a los permisos solicitados, y unificar la creación del canal en `NotificationChannels.ensureCreated()` llamándolo al arrancar.

### 3.4 🟠 El modo offline está listo en el firmware y no existe en la app

El hub implementa por BLE, en `blufi_handle_custom_json` ([blufi_manager.c:120-262](components/blufi_manager/blufi_manager.c)): `{"Data":true}` (volcado de telemetría), `{"MACSLAVE":...}` (guardar configuración), `{"cmd":"get_mode"}`, `{"cmd":"set_mode"}` y `{"cmd":"set_time"}`.

En la app **no hay ni una sola referencia** a esos comandos: un grep por `set_mode`, `get_mode`, `set_time` o `postCustomData` en todo `app/src/main/java` no devuelve nada. `AndroidBlufiFacade` solo implementa `requestWifiNetworks`, `provision` y `disconnect`; la única custom data que procesa es la MAC de confirmación en `onReceiveCustomData`.

Es decir: **la rama en la que estás trabajando está terminada en un extremo y sin empezar en el otro.**

**Qué haría falta:** añadir a `BlufiFacade` un método para enviar custom data JSON y exponer un flujo de respuestas; luego un `ViewModel`/pantalla que permita conmutar el modo, poner la hora en la puesta en marcha (sustituto de SNTP) y recoger el volcado por BLE. Ojo con §4.5: el volcado por BLE borra sin confirmación.

### 3.5 🟠 `cfgAck` se publica y nadie lo escucha

Cuando la esfera confirma una configuración, el hub publica `{"MACSLAVE":"..","cfgAck":"ok"}` (o `"err"` con motivo) en `ismart/app/<MACHUB>` — [mqtt_manager.c:409-467](components/mqtt_manager/mqtt_manager.c). La app **no lo procesa**: `MqttMessageParser.parseDump()` intenta deserializar todo mensaje como una lista de `DumpItem` y, si falla, devuelve lista vacía; el `cfgAck` cae ahí silenciosamente.

Mientras tanto la app marca `configSent = true` **en el momento de publicar** ([CardDetailViewModel.kt:206](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/carddetail/CardDetailViewModel.kt)), no cuando la esfera aplica la configuración. El usuario ve "configurado" aunque la esfera no se haya enterado (y puede tardar hasta 1 h en enterarse, o no enterarse nunca si falla).

**Qué haría falta:** distinguir tres estados en la `Card` (`pendiente de enviar` / `enviada al hub` / `aplicada por la esfera`), parsear el `cfgAck` en el `MqttManager` y reflejarlo en la UI. Es el feedback que hoy le falta al flujo de configuración, y el firmware ya te lo está dando gratis.

### 3.6 🟡 `colorLED` y `riegoAuto`: viajan de punta a punta y mueren

- `colorLED`: la app tiene selector de color (`setLedColor`) y lo envía en el JSON. El hub lo guarda y lo reenvía. **La esfera no lo parsea** — `peer_manager_on_data_recv` solo lee `ts`, `diasRiego`, `horaRiego` y `ml`. El LED de la esfera solo hace animaciones de estado fijas.
- `riegoAuto`: existe en la configuración estándar del hub ([mqtt_manager.c:46](components/mqtt_manager/mqtt_manager.c)) y en el modelo `ConfigCommand.kt` de la app… **pero la app no lo envía** (el JSON se construye a mano en `saveAndSendSchedule` y no lo incluye) y la esfera no lo lee. Es un campo huérfano en los tres códigos a la vez.

**Qué haría falta:** decidir si son features o restos. Si `colorLED` es feature, implementarlo en la esfera es barato (ya hay `led_manager_set_rgb`). Si `riegoAuto` iba a ser "riego por humedad en vez de por horario", eso es una feature de producto que hay que definir antes de codificar.

### 3.7 🟡 Los timestamps mezclan tres relojes

El `timestamp` de cada lectura lo pone **el hub al recibirla** (`strftime` con su hora local, zona CET/CEST). La app compara ese texto contra `LocalDateTime.now()` **del teléfono** para decidir si hubo riego en la ventana ([AlertLogic.kt:160-177](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/home/AlertLogic.kt)). Y la esfera, que es quien mide, no aporta hora ninguna.

Funciona mientras el teléfono esté en CET. Con el teléfono en otra zona horaria, la ventana de comparación se desplaza y las alertas de riego se vuelven aleatorias. Además, en modo offline sin `set_time`, el reloj del hub arranca en 1970 y los timestamps quedan inservibles.

**Qué haría falta:** guardar epoch UTC (entero) en vez de texto local, y formatear solo al mostrar. Es un cambio de esquema en Room (ver §5.9 sobre migraciones) y un cambio de formato en el hub.

---

## 4. HUB — cabos sueltos

### 4.1 ⚪ Código que no llama nadie

| Elemento | Dónde | Estado |
|---|---|---|
| `mqtt_manager_publicar_datos()` | [mqtt_manager.c:700](components/mqtt_manager/mqtt_manager.c) | Publicaba en formato line-protocol de InfluxDB. Sin llamadas: quedó de una arquitectura anterior |
| `mqtt_manager_obtener_cliente()` | [mqtt_manager.c:750](components/mqtt_manager/mqtt_manager.c) | Sin llamadas |
| `esfera_manager_clear()` / `esfera_manager_count()` | [esfera_manager.c:251,260](components/esfera_manager/esfera_manager.c) | `clear` no se llama desde ningún sitio; `count` solo lo usa `clear` |
| `semaforo_wifi_listo` | [hub_station.c:34,240](main/hub_station.c) | Se crea y se libera (`xSemaphoreGive` en el evento GOT_IP), pero **nadie hace `Take`**. Sincronización muerta |
| Bloque SoftAP de BluFi | [blufi_manager.c:640-688](components/blufi_manager/blufi_manager.c) | El hub trabaja en `WIFI_MODE_STA`; toda la rama de configuración de SoftAP es boilerplate del ejemplo de Espressif |
| `WIFI_LIST_NUM` | [blufi_manager.c:45](components/blufi_manager/blufi_manager.c) | Definido, sin usar |
| Globales comentadas | [hub_station.c:28-30](main/hub_station.c) | `float humedad, temperatura, voltaje; int riego; char mac_dato[32];` |

### 4.2 🟡 Botón: la pulsación corta no hace nada

[button_manager.c:117](components/button_manager/button_manager.c): `// TODO: tu acción (ej. publicar 'S' por MQTT)`. Las pulsaciones larga (borrar esferas+config) y muy larga (reset de fábrica) sí funcionan. La corta registra el evento y no hace nada — el usuario pulsa y no pasa nada.

Candidatos naturales: forzar un ciclo de descubrimiento, alternar modo online/offline, o publicar un "ping" de diagnóstico.

### 4.3 🟡 Copia-pega en los handlers de SoftAP (y un desbordamiento latente)

Los cuatro handlers `ESP_BLUFI_EVENT_RECV_SOFTAP_*` configuran el AP pero antes tocan **`sta_config`** (`sta_config.sta.scan_method` y `sta_config.sta.channel = 6`) — líneas 644-645, 652-653, 663-664, 685-686. Es un copia-pega del bloque STA que no pinta nada ahí.

Además, en [blufi_manager.c:641-643](components/blufi_manager/blufi_manager.c) se hace `strncpy(ap_config.ap.ssid, ..., ssid_len)` y luego `ap_config.ap.ssid[ssid_len] = '\0'`. Si `ssid_len` fuera 32 (el tamaño del buffer), esa escritura se sale del array. Hoy es inalcanzable porque el hub nunca opera como AP, pero si algún día activas SoftAP, ahí hay un desbordamiento esperando. El manejo de SSID de STA (líneas 610-624) sí valida la longitud correctamente — es el contraste que delata que ese bloque nunca se revisó.

### 4.4 🟠 NVS de 24 KB reescribiendo un blob de ~1,5 KB por lectura

La partición NVS son `0x6000` = **24 KB** ([partitions.csv](partitions.csv)). Ahí conviven: credenciales Wi-Fi, bonding BLE (`CONFIG_BT_BLE_SMP_BOND_NVS_FLASH=y`), modo de trabajo, lista de esferas, `config_store` (un JSON por esfera + su ACK) y el blob de telemetría.

El blob de telemetría es `4+2+2+32×sizeof(esfera_data_t)` ≈ **1,5 KB**, y `persist_entries_locked()` lo **reescribe entero en cada lectura recibida** ([esfera_manager.c:177](components/esfera_manager/esfera_manager.c)) y otra vez al confirmarse la entrega (`remove_oldest`). Son dos reescrituras completas por lectura.

NVS es log-estructurado: cada reescritura consume páginas nuevas y obliga a recolección de basura. En 24 KB compartidos, con dos escrituras de 1,5 KB por lectura, esto merece medirse: el riesgo es desgaste de flash y `ESP_ERR_NVS_NOT_ENOUGH_SPACE` a medio plazo. Y si amplías `MAX_ENTRADAS` para mitigar §3.2, el problema crece linealmente.

**Qué haría falta:** o bien agrandar la partición NVS en `partitions.csv` (hay sitio: la app ocupa 0x180000 de 4 MB), o mover la telemetría pendiente a una partición propia / SPIFFS con escritura incremental en vez de blob completo. Merece un banco de pruebas antes de decidir.

### 4.5 🟠 El volcado por BLE borra sin confirmación

En el camino MQTT, la telemetría se borra de NVS **solo tras el PUBACK** del broker (`MQTT_EVENT_PUBLISHED`) — entrega at-least-once bien hecha. En el camino BLE, `blufi_handle_custom_json` envía y **borra inmediatamente** ([blufi_manager.c:148-160](components/blufi_manager/blufi_manager.c)): si el BLE se corta a mitad, esas lecturas desaparecen.

Es la asimetría más peligrosa del modo offline, y conviene resolverla **antes** de construir el lado de la app (§3.4), porque condiciona el protocolo: haría falta un ACK explícito de la app antes de que el hub borre.

### 4.6 🟡 Un clon nuevo del repo no compila

`.gitignore` excluye — correctamente — `components/mqtt_manager/mqtt_secrets.h` y `components/mqtt_manager/certificates/`. El `README.md` **los lista como ignorados**, pero no dice **qué deben contener**: ni las claves que espera `mqtt_secrets.h` (`MQTT_URI`, `MQTT_USERNAME`, `MQTT_PASSWORD`), ni qué tres certificados hacen falta ni con qué nombres los busca el build (`ca_cert.pem`, `client_cert.pem`, `client_key.pem`, embebidos por símbolo). Un desarrollador nuevo —o tú mismo en otra máquina— clona y no puede construir.

**Qué haría falta:** un `mqtt_secrets.h.example` con las claves vacías y una sección en el README con los nombres y la ubicación exacta de los certificados.

⚪ El README también está **desactualizado en la estructura**: enumera `mqtt_manager`, `blufi_manager`, `esfera_manager`, `time_sync` y `CJSON`, pero faltan `button_manager`, `detector_manager` y `app_mode_manager`, que ya existen.

### 4.7 ⚪ `sdkconfig` versionado (e inconsistente con el otro repo)

El hub trackea `sdkconfig` (el archivo generado, 64 líneas cambiadas en el diff actual); el slave lo ignora y trackea `sdkconfig.defaults`. Lo habitual es versionar solo `sdkconfig.defaults`. Sobre esto, ver también §6.7.

---

## 5. Esfera — cabos sueltos

### 5.1 🟠 El modo TEST está activo por defecto y cuesta 3 segundos en cada arranque

En [components/test_manager/Kconfig](../solar-irrigator-slave/components/test_manager/Kconfig), `TEST_MANAGER` y `TEST_MANAGER_BOOT_AT_WINDOW` son **`default y`**, con `TEST_MANAGER_BOOT_AT_MS = 3000`. El `sdkconfig` actual lo confirma: `CONFIG_TEST_MANAGER=y`.

Consecuencia en un dispositivo a batería: **cada despertar del deep sleep instala el driver USB-Serial-JTAG y se queda 3 segundos escuchando comandos AT** ([test_manager.c:102-185](../solar-irrigator-slave/components/test_manager/test_manager.c)) **antes** de inicializar Wi-Fi, medir y reportar. Si la esfera despierta cada hora, son ~72 s diarios de consumo extra puro, más el coste del periférico USB.

**Qué haría falta:** un perfil de producción (`sdkconfig.prod` o similar) con `CONFIG_TEST_MANAGER_BOOT_AT_WINDOW=n`, y medir la diferencia de consumo. Es probablemente la mejora de autonomía más barata que tienes disponible.

### 5.2 ⚪ Dos versiones antiguas de funciones, comentadas y completas

- [solar_irrigator_slave.c:129-169](../solar-irrigator-slave/main/solar_irrigator_slave.c): versión previa de `wait_hub_first_link_blocking()` (sin rotación de canal), ~40 líneas.
- [solar_irrigator_slave.c:271-390](../solar-irrigator-slave/main/solar_irrigator_slave.c): versión previa de `send_data_to_hub()`, ~120 líneas.

Son 160 líneas comentadas en el archivo principal. El historial de git ya las conserva; en el archivo solo hacen ruido y obligan a leer dos veces para saber cuál está viva.

### 5.3 🟡 Una decisión a medias: ¿rotar canal si falla el envío?

[solar_irrigator_slave.c:261-267](../solar-irrigator-slave/main/solar_irrigator_slave.c): tras agotar los 3 reintentos de telemetría hay un comentario que se pregunta si conviene cambiar de canal, y la llamada `switch_channel_and_reboot(current_channel)` está **comentada**.

Hoy, si el router cambia de canal después del emparejamiento, la esfera **pierde el contacto para siempre**: reintenta 3 veces, falla, duerme, y repite el ciclo indefinidamente sin recuperarse (la rotación de canal solo existe en el primer emparejamiento). Es un modo de fallo silencioso y permanente.

**Qué haría falta:** decidirlo. Descomentar sin más es arriesgado (un fallo puntual provocaría reinicio y cambio de canal innecesario); lo razonable es un contador de ciclos fallidos consecutivos en NVS y rotar tras N fallos.

### 5.4 ⚪ Declarado y nunca definido

`peer_manager_receive_hub_mac(void)` está declarado en [peer_manager.h:35](../solar-irrigator-slave/components/peer_manager/peer_manager.h) y **no existe en el `.c`**. No rompe la compilación porque nadie lo llama, pero el día que alguien lo invoque será un error de enlazado.

### 5.5 ⚪ Más código muerto

| Elemento | Dónde | Estado |
|---|---|---|
| `led_manager_start_animation()`, `led_manager_stop_animation()`, `led_check()` | [led_manager.c:91,97,104](../solar-irrigator-slave/components/led_manager/led_manager.c) | Definidas; solo `led_check` llama a las otras dos, y a `led_check` no lo llama nadie. Solo se usan `set_rgb` y `start_animation_2` |
| `time_sync_check_irrigation_time()`, `time_sync_get_next_wakeup_time()` | [time_sync.c:22,30](../solar-irrigator-slave/components/time_sync/time_sync.c) | Versiones antiguas del cálculo de despertar (por día/hora sueltos). La viva es `..._from_mask()` |
| `test_mode_nvs_get/set()`, `test_mode_start_cli()` | [test_manager.c:332-334](../solar-irrigator-slave/components/test_manager/test_manager.c) | Shims "compatibilidad hacia atrás" que no están ni en el header |
| `tm_readline()` | [test_manager.c:59](../solar-irrigator-slave/components/test_manager/test_manager.c) | Comentado como "No se usa" en el propio código |
| `peer_data_t.hora_actual` / `.minuto_actual` | [peer_manager.h:22-23](../solar-irrigator-slave/components/peer_manager/peer_manager.h) | Campos de la estructura que nadie escribe ni lee (solo se usa `dia_actual`) |
| `sleep_mode_active` | [solar_irrigator_slave.c:47](../solar-irrigator-slave/main/solar_irrigator_slave.c) | Global `true` que nada modifica: hoy no hay forma de desactivar el deep sleep en caliente |

### 5.6 🟡 `time_sync_request_time()` es un stub que engaña

[time_sync.c:12-20](../solar-irrigator-slave/components/time_sync/time_sync.c): se llama en el ciclo normal como "[STATE 3] Sincronizando hora", pero **solo lee el reloj local y lo registra**. El comentario lo admite: *"Simula recepción de hora del hub"*. La sincronización real ocurre después, cuando llega el `ts` del hub. El log da una falsa sensación de que la hora se sincronizó ahí.

### 5.7 🟡 Sin `partitions.csv` propio

El slave no define tabla de particiones, así que usa la por defecto de ESP-IDF. Dado que guarda configuración, MAC del hub y canal en NVS, conviene fijar una tabla explícita para que un cambio de tamaño de la app no mueva la NVS y borre el emparejamiento de las esferas ya desplegadas.

---

## 6. App Android — cabos sueltos

### 6.1 🔴 Pantallas y rutas inalcanzables

- **`HomeScreen`** — huérfana (ver §3.3), y con ella las notificaciones.
- **Ruta `card_list`** — registrada en el `NavGraph` ([NavGraph.kt:269-288](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/navigation/NavGraph.kt)) con su argumento `hubId`, pero **nadie navega a ella**: el único punto de entrada a las tarjetas es `onCardClick` → `card_detail/{id}`. Eso deja muerto todo el paquete `feature/cards/`: `CardListScreen`, `CardViewModel`, el `CardDetailScreen` **antiguo** (que convive con el nuevo de `feature/carddetail/`) y sus componentes `StatRow` y `ChartView`.

Ojo con la duplicación: hay **dos `CardDetailScreen`** en el proyecto, en paquetes distintos. El vivo es `feature/carddetail/CardDetailScreen.kt` (885 líneas); el de `feature/cards/` es el viejo.

### 6.2 ⚪ Un paquete entero de componentes sin usar

En `feature/components/` están definidos y **nunca referenciados**: `EmptyState`, `PrimaryButton`, `TextFieldPassword`, `AddHubCard`, `HubCard`, `LoadingDialog`. Son restos de una iteración anterior de la UI (las pantallas actuales construyen sus propios controles). Sí se usan `DotsIndicator` y `GhostCardItem`.

También: `core/blufi/interop/StubBlufiFacadeold.kt` es un **archivo vacío**, y `BlufiLifecycleObserver` está definido pero nunca se instancia.

### 6.3 🟡 Se puede crear pero no borrar

`HubRepository.deleteById()` y `CardRepository.delete()` existen, con `ForeignKey.CASCADE` bien puesto en las entidades para que al borrar un hub caigan sus cards, samples y notas. **Ninguna pantalla los llama.** El usuario puede añadir hubs y plantas, pero no quitarlos: la única salida es borrar los datos de la app.

Junto con el botón físico del hub (pulsación larga borra el emparejamiento en el hub, pero no en el teléfono), esto deja los dos lados **desincronizables sin remedio**: si borras las esferas desde el hub, la app sigue mostrando tarjetas fantasma para siempre.

### 6.4 🟡 El botón "atrás" se traga la pulsación

[HubPagerRoute.kt:77-80](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/home/HubPagerRoute.kt): hay un `BackHandler(enabled = pagerState.currentPage > 0)` **con el cuerpo vacío** (solo un comentario sugiriendo animar el scroll). Estando en el hub 2 o posterior, el botón atrás **no hace nada**: ni retrocede de página, ni sale de la app.

### 6.5 🟡 `BlePermissionController` inconsistente con el manifiesto

`requiredPermissions` incluye `ACCESS_FINE_LOCATION` **en todas las versiones de Android** ([BlePermissionController.kt:21](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/blufi/interop/BlePermissionController.kt)), pero el manifiesto lo declara con `android:maxSdkVersion="30"`. En Android 12+ ese permiso no existe para la app, así que **`hasAllPermissions()` devolvería siempre `false`**.

Hoy no explota porque `hasAllPermissions()` **no se llama desde ningún sitio** y `PermissionsScreen` filtra el permiso a mano en Android 12+ ([PermissionsScreen.kt:44-48](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/onboarding/PermissionsScreen.kt)). Pero la lógica correcta está duplicada en la pantalla en vez de en el controller, que es donde debería vivir. Es una trampa para el próximo que use `hasAllPermissions()`.

### 6.6 🟡 Fallos silenciosos en la conexión MQTT

[MqttManager.kt:124](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/mqtt/MqttManager.kt): `socketFactory = runCatching { CertificateUtil.createSocketFactory(appContext) }.getOrNull()`. Si cargar los certificados falla (recurso corrupto, clave en formato inesperado), **se traga la excepción y deja `socketFactory` en `null`**, y la conexión se intenta igual sin la configuración TLS prevista. El síntoma sería "no llegan datos" sin ninguna pista en el log sobre la causa real.

Lo mismo con `MqttMessageParser.parseDump()`, que ante cualquier JSON no esperado devuelve lista vacía sin registrar nada — por eso el `cfgAck` de §3.5 desaparece sin rastro.

### 6.7 🟡 `ConfigCommand` no se usa (y el modelo del contrato se perdió)

[core/model/ConfigCommand.kt](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/model/ConfigCommand.kt) define con `@Serializable` exactamente el contrato de configuración (incluido `riegoAuto`)… y **nadie lo instancia**. `saveAndSendSchedule()` construye el JSON a mano con `JSONObject`, campo por campo.

Es justo el tipo de duplicación que causa desalineos: el contrato "oficial" está en una clase muerta mientras el contrato real vive en un método de un ViewModel. Si en algún momento cambia un nombre de campo, la clase no se entera.

### 6.8 🟠 `fallbackToDestructiveMigration()` con base en versión 5

[AppGraph.kt:48](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/AppGraph.kt). La base va por la **versión 5** (ya ha cambiado de esquema cuatro veces) y no hay ni una migración escrita: **cada cambio de esquema borra todo el histórico del usuario**. Durante el desarrollo es cómodo; en cuanto haya usuarios reales con meses de datos, es pérdida de datos garantizada. Y varias mejoras de esta lista (timestamps como epoch en §3.7, estados de configuración en §3.5) implican cambio de esquema.

### 6.9 ⚪ Dependencias declaradas y no usadas

En [app/build.gradle.kts](../../Users/jorge/Documents/smartgrowapp/app/build.gradle.kts): `androidx.work.runtime.ktx` (WorkManager), `accompanist.permissions` (la pantalla de permisos usa `ActivityResultContracts` directamente) y `androidx.startup.runtime` — sin una sola referencia en el código. Las dos primeras delatan intenciones que quedaron a medias (trabajo en segundo plano y gestión de permisos).

También: `POST_NOTIFICATIONS` declarado dos veces en el manifiesto, y permisos de *foreground service* para un servicio que no existe (§3.2).

### 6.10 ⚪ Restos de trabajo en curso en `CardDetailViewModel`

[CardDetailViewModel.kt:45-53](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/feature/carddetail/CardDetailViewModel.kt): hay un campo `private val cardFlow = cardRepository.observeCardsForHub(1)` — **con el `hubId` cableado a 1** — seguido de un `FIXME` y varios párrafos de comentarios razonando en voz alta sobre si hace falta un `observeCard(id)`. Ese método **ya existe** en el repositorio y es el que usa `loadData()`. El campo `cardFlow` nunca se recoge: es inofensivo, pero el bloque de comentarios describe un problema ya resuelto y confunde a quien lea.

Hay más comentarios del mismo tipo en las líneas 78-95 ("Risk accepted for Realtime requirement") documentando una decisión de diseño sobre pisar los campos mientras el usuario edita — ese sí es un tema real: **si llega un mensaje MQTT mientras el usuario está escribiendo, los campos se sobrescriben**.

---

## 7. Seguridad

### 7.1 🔴 La clave privada del cliente está en el repositorio y en el APK

`git ls-files` del proyecto Android confirma que están versionados:

```
app/src/main/res/raw/ca_cert.pem
app/src/main/res/raw/client_cert.pem
app/src/main/res/raw/client_key.pem
```

Y además la contraseña del broker está escrita en el código: [MqttConfig.kt:18](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/mqtt/MqttConfig.kt) (`username = "app-global"`, contraseña en claro), más `KEY_PASSWORD = "433603"` en [CertificateUtil.kt:20](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/mqtt/CertificateUtil.kt).

Cualquiera que descargue el APK puede extraer el certificado de cliente, la clave privada y la contraseña. Y como es una **credencial compartida por todos los usuarios** (`app-global`), no se puede revocar a un solo usuario: rotarla obliga a actualizar a todos.

Contrasta con el firmware, donde esto **sí está bien resuelto**: `mqtt_secrets.h` y `certificates/` están en `.gitignore`.

**Qué haría falta**, por orden de urgencia:
1. Sacar las claves del repo y **rotarlas** (asumir que están comprometidas; el historial de git las conserva aunque las borres ahora).
2. Credencial por usuario o por dispositivo en vez de una global.
3. A medio plazo: que la app no hable directamente con el broker con credenciales de larga vida, sino contra un backend que emita tokens de corta duración.

### 7.2 🟠 La autorización en MQTT es "saber la MAC"

Los topics son `ismart/hub/<MACHUB>` e `ismart/app/<MACHUB>`. Con la credencial global de §7.1, **cualquiera puede publicar configuración en el hub de cualquier otro** si conoce (o adivina, o escanea) su MAC: encender la bomba con `ml` arbitrarios, cambiar horarios, etc. No hay noción de propiedad de un hub.

**Qué haría falta:** ACLs por cliente en el broker (que cada credencial solo pueda publicar/suscribirse a los topics de sus hubs), lo cual presupone resolver §7.1.

### 7.3 🟡 ESP-NOW sin cifrar

Todos los peers se registran con `.encrypt = false` (hub y esfera). El tráfico va en claro y es suplantable: la defensa actual es la lista blanca de MACs en NVS del hub más la comprobación de que la MAC declarada coincide con la del remitente — pero **la MAC de origen de ESP-NOW se puede falsificar**. Un atacante en rango que conozca la MAC de una esfera registrada puede inyectar telemetría falsa (o, al revés, hacerse pasar por el hub y enviar configuración a una esfera).

Es un riesgo acotado (requiere proximidad física) y ESP-NOW soporta cifrado con LMK/PMK. Merece decidirse conscientemente, no por omisión.

### 7.4 🟡 BluFi sin negociación de seguridad

[AndroidBlufiFacade.kt:45](../../Users/jorge/Documents/smartgrowapp/app/src/main/java/com/smartgrow/smartgrowapp/core/blufi/interop/AndroidBlufiFacade.kt): `NEGOTIATE_SECURITY = false`. Significa que **la contraseña del Wi-Fi doméstico viaja por BLE sin el cifrado de aplicación de BluFi**, durante el aprovisionamiento. El firmware tiene el soporte listo (`blufi_security.c` con DH/AES/CRC está compilado y registrado en los callbacks); es la app la que decide no usarlo.

Esto gana importancia con el modo offline (§3.4), porque entonces **todos** los datos y comandos pasarían por ese mismo canal.

---

## 8. Higiene de repos y build

| | Hallazgo |
|---|---|
| 🟡 | **Deriva de versiones de ESP-IDF**: el `sdkconfig` del hub declara IDF **5.5.5**, su `README.md` pide instalar la **5.4**, el commit más reciente dice "Migración a ESP-IDF **5.5.3**" y el `sdkconfig.defaults` del slave declara **5.4.0**. Cuatro versiones distintas según dónde mires: conviene fijar una y dejarla escrita en un solo sitio |
| 🟡 | El `sdkconfig.defaults` del slave son **2025 líneas**: es un `sdkconfig` completo autogenerado, no un archivo de overrides. Así pierde su función (fijar solo lo que te importa) y genera conflictos enormes en cada cambio |
| 🟡 | Criterios opuestos entre repos: el hub versiona `sdkconfig`, el slave lo ignora |
| ⚪ | Avisos de fin de línea LF↔CRLF en todos los archivos del hub: falta un `.gitattributes` |
| 🟡 | Sin README de puesta en marcha: qué IDF, qué targets (`esp32` vs `esp32c3`), cómo generar `mqtt_secrets.h`, dónde van los certificados. El `README.md` del hub existe pero no cubre esto |

---

## 9. Orden sugerido para retomar

Una secuencia que evita rehacer trabajo — cada fase se apoya en la anterior:

**Fase 0 — Asegurar lo que hay (una tarde)**
1. Commitear los 6 archivos del hub, decidiendo antes qué pasa con el `set_mode(ONLINE)` forzado (§2).
2. Consolidar ramas locales/remotas.
3. Añadir `.gitattributes` y `mqtt_secrets.h.example`.

**Fase 1 — Seguridad (antes de que haya usuarios reales)**
4. Sacar y rotar las claves del APK (§7.1). Es lo único de esta lista que no se puede arreglar "después": el historial ya las tiene.

**Fase 2 — Cerrar las cadenas rotas (lo que más nota el usuario)**
5. `riego` con mililitros reales de punta a punta (§3.1). Arregla la alerta falsa y el depósito de una vez.
6. Reconectar las notificaciones (§3.3): mover el `collect`, pedir `POST_NOTIFICATIONS`, unificar el canal.
7. Procesar `cfgAck` y separar "enviado" de "aplicado" (§3.5).
8. Rotación del buffer del hub al llenarse, en vez de descartar lo nuevo (§3.2, paso 1).

**Fase 3 — Autonomía y robustez**
9. Perfil de producción del slave sin ventana AT (§5.1) y medir consumo.
10. Decidir la política de recuperación de canal ante fallos repetidos (§5.3).
11. Medir el desgaste de NVS y dimensionar la partición (§4.4).
12. Recolección en segundo plano en la app (§3.2, paso 3).

**Fase 4 — Completar el modo offline**
13. Resolver antes el borrado sin confirmación del volcado BLE (§4.5).
14. Custom data JSON en `BlufiFacade` + pantalla de modo/hora/volcado (§3.4).

**Fase 5 — Limpieza** (cuando lo anterior esté estable)
15. Borrar código muerto (§4.1, §5.2, §5.5, §6.1, §6.2), migraciones de Room reales (§6.8), timestamps en epoch (§3.7).

---

## 10. Preguntas abiertas (decisiones de producto, no de código)

Estas no las puede resolver el código; hacen falta para saber qué construir:

1. **`riegoAuto`**: ¿iba a ser riego por umbral de humedad en lugar de por horario? Si sí, es una feature nueva en la esfera (que ya mide humedad ambiente, aunque no de suelo). Si no, hay que eliminarlo de los tres contratos.
2. **`colorLED`**: ¿el usuario debe poder fijar el color del LED de la esfera, o el LED es solo indicador de estado? Hoy la app lo deja elegir y no sirve para nada.
3. **Nivel del depósito**: hoy es una simulación por resta. ¿Se quiere un sensor real, o basta la estimación con un botón de "he rellenado"?
4. **Varias esferas por hub**: el diseño lo soporta, pero la UI muestra las tarjetas en un pager por hub. ¿Cuál es el máximo realista? Condiciona `MAX_ENTRADAS` y el tamaño de la NVS.
5. **Multiusuario / propiedad**: ¿un hub pertenece a una cuenta? Es la pregunta que hay detrás de §7.1 y §7.2; sin responderla no se puede diseñar bien la autenticación.
6. **Desemparejar**: ¿cómo se quita una esfera o un hub desde la app, y cómo se propaga al firmware para que los dos lados queden coherentes (§6.3)?
