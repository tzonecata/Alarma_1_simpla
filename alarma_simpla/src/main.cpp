#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#if 0
#include <PubSubClient.h>
#endif

// ----------------------------
// Config hardware ESP8266 (ESP-12E / NodeMCU)
// 4 senzori PIR + 2 relee (2 sirene)
// ----------------------------

// ----------------------------
// Config WiFi + MQTT
// ----------------------------
// ----------------------------
// Config WiFi + MQTT (în `config.h`)
// ----------------------------
#include "config.h"

#if 0
WiFiClient espClient;
PubSubClient mqttClient(espClient);
#endif

// PIR-uri (4 zone). Numerotare: PIR1..PIR4
// Aici folosim pini „D1, D2, D7, D0” de pe NodeMCU ca exemplu.
static const int PIN_PIR[4] = {
  14,  // PIR1  -> GPIO14 (D5)
  12,  // PIR2  -> GPIO12 (D6)
  13,  // PIR3  -> GPIO13 (D7)
  16   // PIR4  -> GPIO16 (D0)
};

// Relee (2 canale) pentru Siren1 și Siren2
// Majoritatea modulelor de releu sunt ACTIVE LOW:
//  - IN la LOW => releu ON
//  - IN la HIGH => releu OFF
// Dacă modulul tău este activ HIGH, inversează nivelurile mai jos.
static const int PIN_RELAY1_INTERNET = 4;  // IN1 -> GPIO4 (D2)  (internet cut)
static const int PIN_RELAY2 = 5;  // IN2 -> GPIO5 (D1)  (siren)

// LED de stare: folosim LED-ul on-board
static const int PIN_LED = LED_BUILTIN;  // pe ESP8266 este de obicei GPIO2 (D4), activ LOW

// Buton armare/dezarmare – către GND, folosim INPUT_PULLUP
// ATENȚIE: GPIO0 (D3) influențează boot-ul; nu ține butonul apăsat la PORNIRE.
static const int PIN_BUTTON = 0;  // GPIO0 (D3)

// Niveluri pentru relee (adaptează la modulul tău)
static const int RELAY_ON_LEVEL = LOW;    // releu activ la LOW (modul activ LOW)
static const int RELAY_OFF_LEVEL = HIGH;  // releu inactiv la HIGH

// ----------------------------
// Timpi (ms)
// ----------------------------
static const uint32_t EXIT_DELAY_MS = 10'000;       // delay după armare (ieșire din zonă)
static const uint32_t ALARM_DURATION_MS = 30'000;   // cât ține sirena la o alarmă
static const uint32_t MOTION_RETRIGGER_MS = 2'000;  // ignoră re-trigger-uri PIR prea dese
static const uint32_t LONG_PRESS_MS = 2'000;        // apăsare lungă pentru arm/dezarm
static const uint32_t DEBOUNCE_MS = 40;
static const uint32_t GOOGLE_PING_INTERVAL_MS = 60'000;
static const uint32_t STATUS_AUTO_INTERVAL_MS = 30'000;
static const uint32_t FAST_STARTUP_WINDOW_MS = 60'000;
static const uint32_t FAST_STARTUP_PING_INTERVAL_MS = 10'000;
static const uint32_t INTERNET_RELAY_PULSE_MS = 10'000;
static const uint32_t INTERNET_RELAY_COOLDOWN_MS = 300'000;
static const uint32_t WIFI_DISCONNECT_RELAY_RETRY_MS = 120'000;
static const uint32_t EMULATE_WIFI_RETRY_CONNECT_MS = 10'000;
static const IPAddress GOOGLE_PING_IP(8, 8, 8, 8);
static const char* ROMANIA_TZ = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// ----------------------------
// Stare
// ----------------------------
enum class AlarmState : uint8_t { DISARMED,
                                  ARMING,
                                  ARMED,
                                  ALARMING };
static AlarmState state = AlarmState::DISARMED;

static uint32_t stateSinceMs = 0;
static uint32_t alarmUntilMs = 0;
static uint32_t lastMotionMs[4] = { 0, 0, 0, 0 };
static uint32_t lastGooglePingMs = 0;
static uint32_t pingCounter = 0;
static bool pingDownActive = false;
static time_t pingDownStartRealEpoch = 0;
static time_t lastWifiDisconnectRealEpoch = 0;
static bool internetCutActive = false;  // D2 active LOW când cade ping
static uint32_t internetCutUntilMs = 0;
static uint32_t internetRelayNextAllowedMs = 0;
static uint32_t wifiDisconnectedSinceMs = 0;
static bool prevWifiConnected = false;
static bool emulateWifiOff = false;
static bool emulateWifiOffLogged = false;
static uint32_t emulateWifiLastConnectTryMs = 0;
static bool ntpStarted = false;
static bool ntpReadyLogged = false;
static bool ntpWaitingLogged = false;
static uint32_t lastHttpTimeTryMs = 0;
static uint32_t lastAutoStatusMs = 0;
static uint32_t statusPrintCounter = 0;
static String serialRxLine;

// Forward declarations pentru MQTT
static const char* stateToString(AlarmState s);
#if 0
static void publishState();
static void enterState(AlarmState s);
static void connectWifi();
static void connectMqtt();
static void pingGoogleIfNeeded();
static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);
#else
static void enterState(AlarmState s);
static void connectWifi();
static bool isRealWifiConnected();
static bool isWifiConnected();
static void pingGoogleIfNeeded();
static void runPingNow();
static void performPingAndReport();
static void applyRomaniaTimezone();
static void ensureTimeSyncIfNeeded();
static void tryHttpTimeSyncIfNeeded();
static uint32_t currentPingIntervalMs();
static bool getLocalTimeSafe(struct tm* outTm);
static void formatDurationMs(uint32_t durationMs, char* out, size_t outSize);
static void formatEpochDateTime(time_t epoch, char* out, size_t outSize, bool includeSeconds);
static void formatDateTime(char* out, size_t outSize, bool includeSeconds);
static void logEvent(const char* message);
static void updateInternetCutRelayPulse();
static void handleWifiReconnectRelayRetry();
static void enforceInternetRelayOffWhenWifiConnected();
static void updateWifiDisconnectTracking();
static void printStatusEvery30SecIfNeeded();
static void handleSerialCommands();
static void processSerialCommand(String cmd);
static void printRuntimeStatus();
static void printBootInfo();
static void startupRelayStartupTest();
#endif

// Buton
static bool buttonStable = true;  // true = HIGH (neapasat, pullup)
static bool buttonLastRead = true;
static uint32_t buttonLastChangeMs = 0;
static uint32_t buttonPressStartMs = 0;
static bool longPressHandled = false;

static void setOutputs(bool relay1On, bool relay2On, bool ledOn) {
  digitalWrite(PIN_RELAY1_INTERNET, relay1On ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  digitalWrite(PIN_RELAY2, relay2On ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);

#if defined(ESP8266)
  // LED on-board activ LOW
  digitalWrite(PIN_LED, ledOn ? LOW : HIGH);
#else
  // LED extern activ HIGH (fallback)
  digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
#endif
}

static void enterState(AlarmState s) {
  state = s;
  stateSinceMs = millis();

  switch (state) {
    case AlarmState::DISARMED:
      setOutputs(internetCutActive, false, false);
      break;
    case AlarmState::ARMING:
      // clipire lentă cât timp se armează
      setOutputs(internetCutActive, false, true);
      break;
    case AlarmState::ARMED:
      setOutputs(internetCutActive, false, true);
      break;
    case AlarmState::ALARMING:
      // ambele sirene ON + LED ON
      setOutputs(internetCutActive, true, true);
      alarmUntilMs = millis() + ALARM_DURATION_MS;
      break;
  }

#if 0
  publishState();
#endif
  char eventMsg[48];
  snprintf(eventMsg, sizeof(eventMsg), "Alarm state -> %s", stateToString(state));
  logEvent(eventMsg);
}

static void toggleArmDisarm() {
  if (state == AlarmState::DISARMED) {
    enterState(AlarmState::ARMING);
  } else {
    // orice altă stare => dezarmare imediată
    enterState(AlarmState::DISARMED);
  }
}

static void updateButton() {
  const uint32_t now = millis();
  const bool reading = (digitalRead(PIN_BUTTON) == HIGH);  // HIGH = neapasat

  if (reading != buttonLastRead) {
    buttonLastRead = reading;
    buttonLastChangeMs = now;
  }

  // debounce
  if ((now - buttonLastChangeMs) > DEBOUNCE_MS && reading != buttonStable) {
    buttonStable = reading;

    // tranziție stabilă
    if (buttonStable == false) {  // a devenit LOW = apăsat
      buttonPressStartMs = now;
      longPressHandled = false;
    } else {  // eliberat
      buttonPressStartMs = 0;
      longPressHandled = false;
    }
  }

  // long press
  if (buttonStable == false && !longPressHandled && buttonPressStartMs != 0 && (now - buttonPressStartMs) >= LONG_PRESS_MS) {
    longPressHandled = true;
    toggleArmDisarm();
  }
}

static bool motionDetected() {
  const uint32_t now = millis();
  bool any = false;

  for (int i = 0; i < 4; ++i) {
    if (digitalRead(PIN_PIR[i]) == HIGH) {
      if (now - lastMotionMs[i] >= MOTION_RETRIGGER_MS) {
        lastMotionMs[i] = now;
        any = true;
      }
    }
  }

  return any;
}

// ----------------------------
// WiFi + MQTT helpers
// ----------------------------

static const char* stateToString(AlarmState s) {
  switch (s) {
    case AlarmState::DISARMED: return "DISARMED";
    case AlarmState::ARMING: return "ARMING";
    case AlarmState::ARMED: return "ARMED";
    case AlarmState::ALARMING: return "ALARMING";
  }
  return "UNKNOWN";
}

#if 0
static void publishState() {
  if (!mqttClient.connected()) return;

  char payload[64];
  snprintf(payload, sizeof(payload), "STATE:%s", stateToString(state));

  mqttClient.publish(MQTT_TOPIC, payload, true);  // retained
  Serial.print("MQTT publish: ");
  Serial.println(payload);
}

static void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  (void)topic;

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    msg += static_cast<char>(payload[i]);
  }

  msg.trim();

  Serial.print("MQTT recv: ");
  Serial.println(msg);

  // Comenzi așteptate: "CMD:ARM", "CMD:DISARM"
  if (!msg.startsWith("CMD:")) {
    return;  // ignoră mesaje de tip STATE: sau altele
  }

  String cmd = msg.substring(4);
  cmd.toUpperCase();

  if (cmd == "ARM") {
    if (state == AlarmState::DISARMED) {
      enterState(AlarmState::ARMING);
    }
  } else if (cmd == "DISARM") {
    enterState(AlarmState::DISARMED);
  }
}
#endif

static bool isWifiConnected() {
  if (emulateWifiOff) return false;
  return WiFi.status() == WL_CONNECTED;
}

static bool isRealWifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

static void connectWifi() {
  if (isRealWifiConnected()) {
    if (emulateWifiOff) {
      emulateWifiOff = false;
      emulateWifiOffLogged = false;
      wifiDisconnectedSinceMs = 0;
      logEvent("EMULATE_WIFI_OFF canceled -> REAL_WIFI_CONNECTED");
    }
    return;
  }

  if (emulateWifiOff) {
    if (!emulateWifiOffLogged) {
      logEvent("WiFi emulation active -> forcing DISCONNECTED state");
      emulateWifiOffLogged = true;
    }

    const uint32_t now = millis();
    if ((now - emulateWifiLastConnectTryMs) >= EMULATE_WIFI_RETRY_CONNECT_MS) {
      emulateWifiLastConnectTryMs = now;
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  emulateWifiOffLogged = false;

  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to WiFi: '");
  Serial.print(WIFI_SSID);
  Serial.println("'...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (!isWifiConnected() && millis() - start < 15'000) {
    delay(250);
  }

  if (isWifiConnected()) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();
    logEvent("WiFi connect success");
  } else {
    Serial.println("WiFi connect failed");
    Serial.println();
    logEvent("WiFi connect failed");
  }
}

static void applyRomaniaTimezone() {
  setenv("TZ", ROMANIA_TZ, 1);
  tzset();
}

static void ensureTimeSyncIfNeeded() {
  if (!isWifiConnected()) return;

  if (!ntpStarted) {
    // Force timezone Romania and start NTP using TZ-aware API.
    applyRomaniaTimezone();
#if defined(ESP8266)
    configTzTime(ROMANIA_TZ, "pool.ntp.org", "time.google.com");
#else
    configTime(0, 0, "pool.ntp.org", "time.google.com");
#endif
    ntpStarted = true;
    Serial.println("NTP sync started (Sibiu, RO)");
    Serial.println();
  }

  if (!ntpReadyLogged) {
    struct tm tmInfo;
    if (getLocalTimeSafe(&tmInfo)) {
      char ts[24];
      formatDateTime(ts, sizeof(ts), true);
      Serial.print("==== prima data citita : [");
      Serial.print(ts);
      Serial.println("] ====");
      Serial.println();
      ntpReadyLogged = true;
      ntpWaitingLogged = false;
      pingCounter = 0;
      lastGooglePingMs = millis();
      pingDownActive = false;
      pingDownStartRealEpoch = 0;
      internetCutActive = false;
    } else if (!ntpWaitingLogged) {
      Serial.println("Waiting for real internet time...");
      Serial.println();
      ntpWaitingLogged = true;
    }

    if (!ntpReadyLogged) {
      tryHttpTimeSyncIfNeeded();
    }
  }
}

static void tryHttpTimeSyncIfNeeded() {
  const uint32_t nowMs = millis();
  if ((nowMs - lastHttpTimeTryMs) < 15000) return;
  lastHttpTimeTryMs = nowMs;

  HTTPClient http;
  WiFiClient client;
  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);

  if (!http.begin(client, "http://google.com/generate_204")) return;

  const int code = http.GET();
  if (code > 0) {
    String dateHeader = http.header("Date");
    if (dateHeader.length() > 0) {
      struct tm tmUtc = {};
      char* parsed = strptime(dateHeader.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tmUtc);
      if (parsed != nullptr) {
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t utcEpoch = mktime(&tmUtc);
        applyRomaniaTimezone();
        if (utcEpoch >= 1700000000) {
          timeval tv = {utcEpoch, 0};
          settimeofday(&tv, nullptr);
          Serial.println("Internet time sync via HTTP Date header");
          Serial.println();
        }
      }
    }
  }
  http.end();
}

static bool getLocalTimeSafe(struct tm* outTm) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, outTm);
  return true;
}

static uint32_t currentPingIntervalMs() {
  if (millis() < FAST_STARTUP_WINDOW_MS) {
    return FAST_STARTUP_PING_INTERVAL_MS;
  }
  return GOOGLE_PING_INTERVAL_MS;
}

static void formatDurationMs(uint32_t durationMs, char* out, size_t outSize) {
  const uint32_t totalSec = durationMs / 1000;
  const uint32_t hours = totalSec / 3600;
  const uint32_t minutes = (totalSec % 3600) / 60;
  const uint32_t seconds = totalSec % 60;
  snprintf(out, outSize, "%lu:%02lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
}

static void formatDateTime(char* out, size_t outSize, bool includeSeconds) {
  struct tm tmInfo;
  if (!getLocalTimeSafe(&tmInfo)) {
    snprintf(out, outSize, "NO_REAL_TIME");
    return;
  }

  if (includeSeconds) {
    strftime(out, outSize, "%d/%m/%Y %H:%M:%S", &tmInfo);
  } else {
    strftime(out, outSize, "%d/%m/%Y %H:%M", &tmInfo);
  }
}

#if 0
static void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  if (!mqttClient.connected()) {
    String clientId = "alarma_simpla_";
    clientId += String(ESP.getChipId(), HEX);

    Serial.print("Connecting to MQTT: ");
    Serial.print(MQTT_SERVER);
    Serial.print(":" );
    Serial.println(MQTT_PORT);

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("MQTT connected");
      mqttClient.subscribe(MQTT_TOPIC);
      publishState();  // trimite starea curentă la conectare
    } else {
      Serial.println("MQTT connect failed");
    }
  }
}
#endif

static void performPingAndReport() {
  struct tm tmNow;
  if (!getLocalTimeSafe(&tmNow)) {
    return;
  }
  ++pingCounter;

  char ts[24];
  formatDateTime(ts, sizeof(ts), true);

  bool ok = false;
  if (isWifiConnected()) {
    ok = Ping.ping(GOOGLE_PING_IP, 1);
    Serial.println();
  }

  if (ok) {
    pingDownActive = false;
    pingDownStartRealEpoch = 0;
    internetCutActive = false;
    internetCutUntilMs = 0;

    Serial.println();
    Serial.print(pingCounter);
    Serial.print(") ");
    Serial.print("[");
    Serial.print(ts);
    Serial.print("] PING Google OK (");
    Serial.print(Ping.averageTime());
    Serial.println(" ms)");
  } else {
    const uint32_t nowMs = millis();

    if (!pingDownActive) {
      pingDownActive = true;
      pingDownStartRealEpoch = time(nullptr);
      char disconnectedAt[24];
      formatDateTime(disconnectedAt, sizeof(disconnectedAt), false);
      Serial.println("=============================================");
      Serial.print("[INTERNET a fost deconectat la: ");
      Serial.print(disconnectedAt);
      Serial.println("]  *FW by TONE :)*");
      Serial.println("=============================================");
      Serial.println();
      logEvent("internet_DOWN_detected");
      printRuntimeStatus();
    }

    if (!isWifiConnected() && !internetCutActive && (int32_t)(nowMs - internetRelayNextAllowedMs) >= 0) {
      internetCutActive = true;
      internetCutUntilMs = nowMs + INTERNET_RELAY_PULSE_MS;
      internetRelayNextAllowedMs = nowMs + INTERNET_RELAY_PULSE_MS + INTERNET_RELAY_COOLDOWN_MS;
      logEvent("relay_1_ACTIVATED");
    }
  }
}

static void logEvent(const char* message) {
  char ts[24];
  formatDateTime(ts, sizeof(ts), true);
  Serial.print("[ EVENT= ");
  Serial.print(message);
  Serial.print("] :    ");
  Serial.println(ts);
}

static void formatEpochDateTime(time_t epoch, char* out, size_t outSize, bool includeSeconds) {
  if (epoch < 1700000000) {
    snprintf(out, outSize, "NO_REAL_TIME");
    return;
  }

  struct tm tmInfo;
  localtime_r(&epoch, &tmInfo);
  if (includeSeconds) {
    strftime(out, outSize, "%d/%m/%Y %H:%M:%S", &tmInfo);
  } else {
    strftime(out, outSize, "%d/%m/%Y %H:%M", &tmInfo);
  }
}

static void updateInternetCutRelayPulse() {
  if (!internetCutActive || internetCutUntilMs == 0) return;

  const uint32_t now = millis();
  if ((int32_t)(now - internetCutUntilMs) >= 0) {
    internetCutActive = false;
    internetCutUntilMs = 0;
    logEvent("relay_1_DEACTIVATED");
  }
}

static void enforceInternetRelayOffWhenWifiConnected() {
  if (!isWifiConnected()) return;
  if (!internetCutActive && internetCutUntilMs == 0) return;

  internetCutActive = false;
  internetCutUntilMs = 0;
  logEvent("relay_1_FORCED_INACTIVE_wifi_connected");
}

static void handleWifiReconnectRelayRetry() {
  if (isWifiConnected()) {
    wifiDisconnectedSinceMs = 0;
    return;
  }

  const uint32_t now = millis();
  if (wifiDisconnectedSinceMs == 0) {
    wifiDisconnectedSinceMs = now;
    return;
  }

  if ((now - wifiDisconnectedSinceMs) < WIFI_DISCONNECT_RELAY_RETRY_MS) return;
  if (internetCutActive) return;

  internetCutActive = true;
  internetCutUntilMs = now + INTERNET_RELAY_PULSE_MS;

  logEvent("relay_1_ACTIVATED");
}

static void pingGoogleIfNeeded() {
  const uint32_t now = millis();
  if ((now - lastGooglePingMs) < currentPingIntervalMs()) return;
  lastGooglePingMs = now;
  performPingAndReport();
}

static void runPingNow() {
  performPingAndReport();
}

static void updateWifiDisconnectTracking() {
  const bool connected = isWifiConnected();
  if (prevWifiConnected && !connected) {
    lastWifiDisconnectRealEpoch = time(nullptr);
    if (wifiDisconnectedSinceMs == 0) {
      wifiDisconnectedSinceMs = millis();
    }
    logEvent("WiFi transition: CONNECTED -> DISCONNECTED");
    printRuntimeStatus();
  } else if (!prevWifiConnected && connected) {
    logEvent("WiFi transition: DISCONNECTED -> CONNECTED");
  }
  prevWifiConnected = connected;
}

static void printRuntimeStatus() {
  ++statusPrintCounter;
  char lastWifiDown[24];
  formatEpochDateTime(lastWifiDisconnectRealEpoch, lastWifiDown, sizeof(lastWifiDown), true);
  const uint32_t pingIntervalMs = currentPingIntervalMs();

  Serial.println();
  Serial.println();
  Serial.print("=====  ");
  Serial.print(statusPrintCounter);
  Serial.println("  ===== >>");

  Serial.println("🚨[ Stare alarmă]");
  Serial.print("Alarmă: ");
  Serial.println(stateToString(state));
  Serial.print("Internet relay: ");
  Serial.println(internetCutActive ? "ACTIVE" : "INACTIVE");
  Serial.print("Ping down activ: ");
  Serial.println(pingDownActive ? "YES" : "NO");
  Serial.print("Ping down start: ");
  char pingDownAt[24];
  formatEpochDateTime(pingDownStartRealEpoch, pingDownAt, sizeof(pingDownAt), true);
  Serial.println(pingDownAt);
  Serial.println();

  Serial.println("[🌐 Rețea WiFi]");
  Serial.print("Status WiFi: ");
  Serial.println(isWifiConnected() ? "CONNECTED" : "DISCONNECTED");
  Serial.print("WiFi emulation: ");
  Serial.println(emulateWifiOff ? "ON" : "OFF");
  if (isWifiConnected()) {
    Serial.print("IP local: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
    Serial.print("rssi=");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("IP local: -");
    Serial.println("Gateway: -");
    Serial.println("DNS: -");
    Serial.println("rssi=-");
  }
  Serial.println();

  Serial.println("[🌍 Conectivitate Internet]");
  Serial.print("Ultima deconectare: ");
  Serial.println(lastWifiDown);
  char wifiDisconnectedFor[16];
  if (wifiDisconnectedSinceMs == 0 || isWifiConnected()) {
    snprintf(wifiDisconnectedFor, sizeof(wifiDisconnectedFor), "0:00:00");
  } else {
    const uint32_t disconnectedForMs = millis() - wifiDisconnectedSinceMs;
    formatDurationMs(disconnectedForMs, wifiDisconnectedFor, sizeof(wifiDisconnectedFor));
  }
  Serial.print("WiFi disconnected since: ");
  Serial.println(wifiDisconnectedFor);
  Serial.print("Ping counter: ");
  Serial.println(pingCounter);
  Serial.print("Ping interval: ");
  Serial.print(pingIntervalMs);
  Serial.print(" ms (");
  Serial.print(pingIntervalMs / 1000);
  Serial.println(" sec)");
  Serial.println();

  Serial.println("⏱ Sistem");
  Serial.print("NTP started: ");
  Serial.println(ntpStarted ? "YES" : "NO");
  Serial.print("NTP ready: ");
  Serial.println(ntpReadyLogged ? "YES" : "NO");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.print("Chip ID: ");
  Serial.println(ESP.getChipId(), HEX);
  Serial.print("=====  ");
  Serial.print(statusPrintCounter);
  Serial.println("  =====  <<");
  Serial.println();
}

static void printStatusEvery30SecIfNeeded() {
  const uint32_t now = millis();
  if ((now - lastAutoStatusMs) < STATUS_AUTO_INTERVAL_MS) return;
  lastAutoStatusMs = now;
  printRuntimeStatus();
}

static void printBootInfo() {
  Serial.println("[BOOT] ===========================================");
  Serial.print("[BOOT] FW build: ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
  Serial.print("[BOOT] Chip ID: 0x");
  Serial.println(ESP.getChipId(), HEX);
  Serial.print("[BOOT] CPU freq (MHz): ");
  Serial.println(ESP.getCpuFreqMHz());
  Serial.print("[BOOT] Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.print("[BOOT] Reset reason: ");
  Serial.println(ESP.getResetReason());
  Serial.print("[BOOT] WiFi SSID: ");
  Serial.println(WIFI_SSID);
  Serial.print("[BOOT] Internet relay pin: GPIO");
  Serial.println(PIN_RELAY1_INTERNET);
  Serial.print("[BOOT] Relay pulse (ms): ");
  Serial.println(INTERNET_RELAY_PULSE_MS);
  Serial.print("[BOOT] WiFi retry trigger (ms): ");
  Serial.println(WIFI_DISCONNECT_RELAY_RETRY_MS);
  Serial.print("[BOOT] Ping interval normal (ms): ");
  Serial.println(GOOGLE_PING_INTERVAL_MS);
  Serial.print("[BOOT] Status auto interval (ms): ");
  Serial.println(STATUS_AUTO_INTERVAL_MS);
  Serial.println("[BOOT] ===========================================");
  Serial.println();
}

static void processSerialCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "ARM") {
    if (state == AlarmState::DISARMED) {
      enterState(AlarmState::ARMING);
      Serial.println("UART CMD: ARM -> OK");
    } else {
      Serial.println("UART CMD: ARM -> already armed/arming/alarming");
    }
    Serial.println();
    return;
  }

  if (cmd == "DISARM") {
    enterState(AlarmState::DISARMED);
    Serial.println("UART CMD: DISARM -> OK");
    Serial.println();
    return;
  }

  if (cmd == "STATUS") {
    printRuntimeStatus();
    return;
  }

  if (cmd == "PINGNOW") {
    Serial.println("UART CMD: PINGNOW");
    runPingNow();
    Serial.println();
    return;
  }

  if (cmd == "EMULATE_WIFI_OFF") {
    emulateWifiOff = true;
    emulateWifiOffLogged = false;
    emulateWifiLastConnectTryMs = 0;
    if (wifiDisconnectedSinceMs == 0) {
      wifiDisconnectedSinceMs = millis();
    }
    WiFi.disconnect();
    internetCutActive = true;
    internetCutUntilMs = millis() + INTERNET_RELAY_PULSE_MS;
    Serial.println("UART CMD: EMULATE_WIFI_OFF -> WiFi forced DISCONNECTED");
    Serial.println("UART CMD: relay INTERNET ON immediately");
    Serial.println();
    logEvent("UART command: EMULATE_WIFI_OFF (relay ON immediate)");
    return;
  }

  if (cmd == "EMULATE_WIFI_ON") {
    emulateWifiOff = false;
    emulateWifiOffLogged = false;
    wifiDisconnectedSinceMs = 0;
    internetCutActive = false;
    internetCutUntilMs = 0;
    Serial.println("UART CMD: EMULATE_WIFI_ON -> WiFi emulation disabled");
    Serial.println("UART CMD: relay INTERNET forced INACTIVE");
    connectWifi();
    Serial.println();
    logEvent("UART command: EMULATE_WIFI_ON");
    return;
  }

  if (cmd == "HELP") {
    Serial.println("UART commands: ARM, DISARM, STATUS, PINGNOW, EMULATE_WIFI_OFF, EMULATE_WIFI_ON, HELP");
    Serial.println();
    return;
  }

  Serial.print("UART CMD unknown: ");
  Serial.println(cmd);
  Serial.println("Try: HELP");
  Serial.println();
}

static void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      processSerialCommand(serialRxLine);
      serialRxLine = "";
      continue;
    }

    if (serialRxLine.length() < 80) {
      serialRxLine += c;
    }
  }
}

static void startupRelayStartupTest() {
  Serial.println("Startup test: RELAY1 x2, then RELAY2 x2");

  for (int i = 0; i < 2; ++i) {
    digitalWrite(PIN_RELAY1_INTERNET, RELAY_ON_LEVEL);
    delay(1000);
    digitalWrite(PIN_RELAY1_INTERNET, RELAY_OFF_LEVEL);
    delay(1000);
  }

  for (int i = 0; i < 2; ++i) {
    digitalWrite(PIN_RELAY2, RELAY_ON_LEVEL);
    delay(1000);
    digitalWrite(PIN_RELAY2, RELAY_OFF_LEVEL);
    delay(1000);
  }

  digitalWrite(PIN_RELAY1_INTERNET, RELAY_OFF_LEVEL);
  digitalWrite(PIN_RELAY2, RELAY_OFF_LEVEL);
  Serial.println("Startup test done.");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  serialRxLine.reserve(96);
  lastAutoStatusMs = millis();
  Serial.println();
  Serial.println("Alarma simpla starting...");
  printBootInfo();
  for (int i = 0; i < 4; ++i) {
    pinMode(PIN_PIR[i], INPUT);
  }
  pinMode(PIN_RELAY1_INTERNET, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  setOutputs(false, false, false);
  startupRelayStartupTest();
  enterState(AlarmState::DISARMED);

  connectWifi();
  prevWifiConnected = isWifiConnected();
  ensureTimeSyncIfNeeded();
  logEvent("Setup complete");
#if 0
  connectMqtt();
#endif
}

void loop() {
  const uint32_t now = millis();

  updateWifiDisconnectTracking();
  enforceInternetRelayOffWhenWifiConnected();
  printStatusEvery30SecIfNeeded();
  updateInternetCutRelayPulse();
  handleWifiReconnectRelayRetry();
  handleSerialCommands();
  updateButton();

  switch (state) {
    case AlarmState::DISARMED:
      // LED blink în starea dezarmată
      setOutputs(internetCutActive, false, ((now / 500) % 2) == 0);
      break;

    case AlarmState::ARMING:
      {
        // clipire lentă pe durata arming
        const bool blink = ((now / 500) % 2) == 0;
        setOutputs(internetCutActive, false, blink);

        if (now - stateSinceMs >= EXIT_DELAY_MS) {
          enterState(AlarmState::ARMED);
        }
        break;
      }

    case AlarmState::ARMED:
      {
        // LED ON constant ca indicator
        setOutputs(internetCutActive, false, true);

        if (motionDetected()) {
          enterState(AlarmState::ALARMING);
        }
        break;
      }

    case AlarmState::ALARMING:
      {
        // Sirenă continuă (simplu). Dacă vrei ton intermitent, schimbă aici.
        setOutputs(internetCutActive, true, true);

        if (now >= alarmUntilMs) {
          // rămâne armat după alarmă
          enterState(AlarmState::ARMED);
        }
        break;
      }
  }

  // WiFi menținut în viață din loop
  if (!isWifiConnected()) {
    connectWifi();
  }

  ensureTimeSyncIfNeeded();
  pingGoogleIfNeeded();
}


