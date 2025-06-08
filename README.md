# Solar Irrigator Hub

Este proyecto es el firmware del **hub** del sistema de riego inteligente **Solar Irrigator**, desarrollado para ESP32 usando ESP-IDF v5.4. El hub recibe datos de las esferas (slaves) mediante ESP-NOW, envía datos a una aplicación móvil a través de MQTT, y utiliza BLUFI para la configuración Wi-Fi.

---

## 📦 Estructura del proyecto

```text
.
├── components
│   ├── mqtt_manager
│   ├── blufi_manager
│   ├── esfera_manager
│   ├── time_sync
│   └── CJSON
├── main
├── build/ ← ignorado por git
├── .vscode/ ← ignorado por git
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── README.md


---

## 🚀 Funcionalidades

- Comunicación ESP-NOW con esferas
- Comunicación MQTT con la app móvil
- Configuración Wi-Fi por BLUFI
- Manejo de credenciales y certificados seguros
- Sincronización de hora mediante NTP
- Almacenamiento temporal de configuraciones de esferas

---

## ⚙️ Configuración

1. Clona el proyecto:

```bash
git clone https://github.com/cristcorrea/solar-irrigator-hub.git
cd solar-irrigator-hub
```

2. Instala ESP-IDF 5.4 siguiendo la guía oficial:
<https://docs.espressif.com/projects/esp-idf/en/v5.4/>

3. Configura el proyecto:

```bash
idf.py menuconfig
```

4. Compila y flashea:

```bash
idf.py build
idf.py flash monitor
```


🔒 Exclusiones en .gitignore
El repositorio ignora:

- /build/
- /.vscode/
- /components/mqtt_manager/certificates/
- /components/mqtt_manager/mqtt_secrets.h


📄 Licencia
Este proyecto es privado y no tiene licencia explícita. Si deseas usarlo, consulta primero.

🤝 Autor
Cristian Correa
[GitHub](https://github.com/cristcorrea)

