# Alarma Simpla - ESP8266

Proiect pe ESP8266 (NodeMCU / ESP-12E) pentru:
- alarma simpla cu 4 senzori PIR si buton ARM/DISARM
- control 2 relee (releu Internet + releu sirena)
- monitorizare conectivitate Internet si reset automat de power la router

## Functionalitate principala (reset router prin releu)
Scopul principal este recover automat pentru router cand ramane fara Internet:
- dispozitivul verifica periodic conectivitatea la Internet prin ping catre `8.8.8.8`
- daca Internetul cade, activeaza releul de Internet pentru a intrerupe alimentarea routerului
- dupa un timp fix, dezactiveaza releul pentru a reconecta alimentarea (power restore)
- detaliu tehnic: timpul fix este dat de `INTERNET_RELAY_PULSE_MS = 10'000` (10 secunde)
- detaliu tehnic: la pornirea impulsului se seteaza `internetCutActive = true` si `internetCutUntilMs = millis() + INTERNET_RELAY_PULSE_MS`
- detaliu tehnic: in `loop()`, functia `updateInternetCutRelayPulse()` opreste impulsul cand `millis() >= internetCutUntilMs`
- detaliu tehnic: releul este `ACTIVE LOW` (`LOW = ON = power disconnect`, `HIGH = OFF = power connect`)

In codul actual:
- prag de retry Wi-Fi: `120 sec` (`WIFI_DISCONNECT_RELAY_RETRY_MS = 120'000`)
- durata impuls releu Internet: `10 sec` (`INTERNET_RELAY_PULSE_MS = 10'000`)
- pe pinul releului Internet (`PIN_RELAY1_INTERNET`, GPIO4 / D2):
  - releu ON = power disconnect
  - releu OFF = power connect

Practic: daca Wi-Fi ramane picat 2 minute, releul taie alimentarea routerului 10 secunde, apoi o reconecteaza.

## Comportament la pornire (fast startup)
In primul minut dupa boot:
- ping la fiecare `10 sec`
- raportare timp la fiecare `5 sec`

Dupa primul minut revine la intervalele normale:
- ping la fiecare `60 sec`
- raportare timp la fiecare `300 sec` (5 minute)

## Functionalitate alarma
Stari implementate:
- `DISARMED`
- `ARMING` (delay de iesire)
- `ARMED`
- `ALARMING`

Comportament:
- 4 intrari PIR pentru detectie miscare
- buton cu long-press pentru ARM/DISARM
- sirena comandata pe releul 2

## Hardware (conform codului)
- PIR1..PIR4 pe GPIO14, GPIO12, GPIO13, GPIO16
- Releu Internet pe GPIO4 (D2)
- Releu Sirena pe GPIO5 (D1)
- LED onboard pentru stare
- Buton pe GPIO0 (D3, cu INPUT_PULLUP)

## Configurare
Editeaza `include/config.h`:
- `WIFI_SSID`
- `WIFI_PASSWORD`

## Build / Flash / Monitor
Build:
```bash
python -m platformio run -j 1
```

Flash:
```bash
python -m platformio run -t upload --upload-port COM15
```

Monitor:
```bash
python -m platformio device monitor -b 115200 --port COM15
```
