# Alarmă simplă cu ESP8266 (ESP‑12E) + 4 PIR + 2 relee

Proiect: 4 senzori PIR → comandă 2 sirene printr-un modul de 2 relee (IN1, IN2). Armare/dezarmare cu buton (apasare lungă). Sirenele se alimentează separat, dar GND trebuie comun cu ESP.

## Componente
- **ESP8266 ESP‑12E / NodeMCU** (sau compatibil)
- **4× senzori PIR** (ex: HC-SR501)
- **1× modul relee 2 canale** cu intrări **IN1**, **IN2**
- **2× sirene** (ex. 12 V) + sursa lor separată
- **Buton** (momentary) pentru armare/dezarmare
- (Opțional) LED extern, dar folosim și LED-ul on-board pentru stare

## Conectare ESP8266 ↔ PIR-uri
> PIR-urile se alimentează de regulă la 5V (sau 3V3 după modul). GND comun cu ESP.

Folosim pinii de pe NodeMCU (în paranteză GPIO real):

- **PIR1**
  - VCC → 5V (sau 3V3, după modul)
  - GND → GND comun
  - OUT → **D1** (GPIO5)
- **PIR2**
  - OUT → **D2** (GPIO4)
- **PIR3**
  - OUT → **D7** (GPIO13)
- **PIR4**
  - OUT → **D0** (GPIO16)

Toate VCC-urile PIR merg la alimentare (+5V sau 3V3, după specificația lor), toate GND-urile la GND comun.

## Conectare ESP8266 ↔ modul 2× relee (IN1, IN2)
> Modulul de relee îl alimentezi din sursa lui (VCC și GND), dar **GND-ul sursei releelor trebuie legat la GND ESP8266** (masă comună).

- **IN1** modul relee → **D5** (GPIO14) = `PIN_RELAY1`
- **IN2** modul relee → **D6** (GPIO12) = `PIN_RELAY2`

Tipic, modulele de relee pentru Arduino/ESP sunt **active LOW**:
- IN la **LOW** → releu ON (sirenă alimentată)
- IN la **HIGH** → releu OFF

Codul este setat pentru modul **activ LOW**. Dacă al tău e activ HIGH, înlocuiești în `.ino`:
- `RELAY_ON_LEVEL = LOW` cu `HIGH`
- `RELAY_OFF_LEVEL = HIGH` cu `LOW`

## Conectare sirene
- **Sirena 1** pe contactele releului 1 (COM/NO sau COM/NC, după cum vrei să fie implicit oprită)
- **Sirena 2** pe contactele releului 2
- Sirenele se alimentează din **sursa lor** (ex. 12 V), complet separată de 3.3 V al ESP-ului; singura legătură logică este prin contacte de releu.

## Conectare buton și LED
- **Buton armare/dezarmare**
  - Un pin buton → **D3** (GPIO0) = `PIN_BUTTON`
  - Celălalt pin buton → GND
  - În cod, pinul e configurat cu `INPUT_PULLUP`
  - **Important**: nu ține butonul apăsat în timp ce pornește/plăcuța este în RESET, altfel poate intra în modul de flash.

- **LED de stare**
  - Folosim `LED_BUILTIN` de pe plăcuță (de obicei pe D4 / GPIO2, activ LOW)
  - LED-ul on-board clipește sau stă aprins în funcție de stare (dezarmat/arming/armat/alarmă)

## Instalare și încărcare (Arduino IDE, ESP8266)
1. Arduino IDE → **Boards Manager** → instalează **ESP8266 by ESP8266 Community**
2. Arduino IDE → **Library Manager** → instalează librăria **PubSubClient** (Nick O'Leary)
3. Selectează placa (ex: “NodeMCU 1.0 (ESP-12E Module)”) și portul
4. Deschide `alarma_simpla/alarma_simpla.ino`
5. Încarcă pe placă

## Conectare WiFi + MQTT
- Modulul se conectează la rețeaua WiFi:
  - **SSID**: `tzone`
  - **Parolă**: `19821981`
- Se conectează apoi la brokerul MQTT (trebuie să modifici IP-ul real în cod, constanta `MQTT_SERVER`):
  - `MQTT_SERVER` default în cod: `192.168.1.10`
  - `MQTT_PORT` default: `1883`
- Topic-ul folosit este **`alarma_simpla`**.

### Mesaje publicate (status → broker)
Pe topicul `alarma_simpla` dispozitivul publică mesaje de forma:
- `STATE:DISARMED`
- `STATE:ARMING`
- `STATE:ARMED`
- `STATE:ALARMING`

Mesajele sunt trimise:
- la fiecare schimbare de stare (dezarmat/arming/armat/alarmă)
- la reconectarea la broker.

### Mesaje primite (comenzi de la broker)
Pe același topic `alarma_simpla` dispozitivul ascultă comenzi:
- `CMD:ARM` → pornește procesul de armare (dacă era dezarmat)
- `CMD:DISARM` → dezarmează imediat (oprește și sirenele)

Poți trimite aceste mesaje din:
- un alt client MQTT (ex: MQTT Explorer, Node-RED, Home Assistant etc.)

## Comportament sistem
- **Apăsare lungă (~2 sec)** pe buton:
  - Comută între **DISARMED** și **ARMING** / **DISARMED** (dezarmare imediată din orice stare)
- La trecerea în **ARMING**:
  - LED-ul on-board clipește lent
  - Există un **delay de ieșire** (default 10 sec) ca să poți părăsi zona
- După `EXIT_DELAY_MS`:
  - Trecere în **ARMED**
  - LED-ul on-board rămâne aprins fix
- Când sistemul este **ARMAT** și **ORICE PIR detectează mișcare**:
  - Trecere în **ALARMING**
  - Se activează **ambele relee** (IN1 și IN2) → ambele sirene pornesc
  - LED-ul rămâne aprins
  - Alarma rămâne activă `ALARM_DURATION_MS` (default 30 sec)
- Poți opri alarma mai repede cu o **apăsare lungă** (dezarmare → DISARMED).

## Configurare pinii și timpii în cod
În fișierul `.ino` poți schimba:
- **Pini intrări PIR**: array-ul `PIN_PIR[4]` (GPIO5, 4, 13, 16)
- **Pini relee**: `PIN_RELAY1`, `PIN_RELAY2`
- **Pin buton**: `PIN_BUTTON`
- **Timpi**:
  - `EXIT_DELAY_MS` – delay după armare (ieșire din zonă)
  - `ALARM_DURATION_MS` – cât timp rămân sirenele active pe alarmă
  - `MOTION_RETRIGGER_MS` – anti-retrigger prea des de la PIR
  - `LONG_PRESS_MS` – durata unei apăsări lungi pentru arm/dezarm


