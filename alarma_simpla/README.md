# Alarma Simpla — ESP8266

Proiect minimal pentru o alarmă pe ESP8266 (NodeMCU / ESP-12E): 4 senzori PIR, 2 relee (sirene), MQTT pentru stare/comenzi.

Pași rapizi pentru dezvoltare pe PC (PlatformIO recomandat):

1. Instalează VS Code + PlatformIO extension.
2. Deschide folderul `alarma_simpla` în VS Code.
3. Build:

```bash
# în terminal (VS Code) folosind PlatformIO
pio run
```

4. Upload (placă conectată via USB):

```bash
pio run -t upload -e nodemcuv2
```

5. Monitor serial:

```bash
pio device monitor -b 115200
```

Alternativ — Arduino IDE:
- Deschide `src/main.cpp` în Arduino IDE.
- Selectează placa `NodeMCU 1.0 (ESP-12E Module)` și portul serial.
- Instalează librăria `PubSubClient` din Library Manager.
- Compilează și urcă folosind butonul `Upload`.

Configurare:
- Editați `include/config.h` pentru `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_SERVER`, `MQTT_PORT`, `MQTT_TOPIC`.

Limitări:
- Nu am rulat build-ul sau upload-ul local — pașii și fișierele sunt pregătite pentru tine.
