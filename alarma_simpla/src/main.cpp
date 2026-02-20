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
static const uint32_t CLOCK_REPORT_INTERVAL_MS = 300'000;
static const uint32_t INTERNET_RELAY_PULSE_MS = 5000;
static const uint32_t INTERNET_RELAY_COOLDOWN_MS = 300'000;
static const IPAddress GOOGLE_PING_IP(8, 8, 8, 8);

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
static uint32_t lastClockReportMs = 0;
static uint32_t pingCounter = 0;
static bool pingDownActive = false;
static time_t pingDownStartRealEpoch = 0;
static bool internetCutActive = false;  // D2 active LOW când cade ping
static uint32_t internetCutUntilMs = 0;
static uint32_t internetRelayNextAllowedMs = 0;
static bool ntpStarted = false;
static bool ntpReadyLogged = false;
static bool ntpWaitingLogged = false;
static uint32_t lastHttpTimeTryMs = 0;
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
static void pingGoogleIfNeeded();
static void runPingNow();
static void performPingAndReport();
static void ensureTimeSyncIfNeeded();
static void tryHttpTimeSyncIfNeeded();
static bool getLocalTimeSafe(struct tm* outTm);
static void formatDateTime(char* out, size_t outSize, bool includeSeconds);
static void printClockEvery5MinIfNeeded();
static void updateInternetCutRelayPulse();
static void handleSerialCommands();
static void processSerialCommand(String cmd);
static void printRuntimeStatus();
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
  Serial.print("State -> ");
  Serial.println(stateToString(state));
  Serial.println();
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

static void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to WiFi: '");
  Serial.print(WIFI_SSID);
  Serial.println("'...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15'000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();
  } else {
    Serial.println("WiFi connect failed");
    Serial.println();
  }
}

static void ensureTimeSyncIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!ntpStarted) {
    // Timezone: Sibiu, Romania
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.google.com");
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
        setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
        tzset();
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
  if (WiFi.status() == WL_CONNECTED) {
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
    }

    if (!internetCutActive && (int32_t)(nowMs - internetRelayNextAllowedMs) >= 0) {
      internetCutActive = true;
      internetCutUntilMs = nowMs + INTERNET_RELAY_PULSE_MS;
      internetRelayNextAllowedMs = nowMs + INTERNET_RELAY_PULSE_MS + INTERNET_RELAY_COOLDOWN_MS;
    }
  }
}

static void updateInternetCutRelayPulse() {
  if (!internetCutActive || internetCutUntilMs == 0) return;

  const uint32_t now = millis();
  if ((int32_t)(now - internetCutUntilMs) >= 0) {
    internetCutActive = false;
    internetCutUntilMs = 0;
  }
}

static void pingGoogleIfNeeded() {
  const uint32_t now = millis();
  if ((now - lastGooglePingMs) < GOOGLE_PING_INTERVAL_MS) return;
  lastGooglePingMs = now;
  performPingAndReport();
}

static void runPingNow() {
  performPingAndReport();
}

static void printClockEvery5MinIfNeeded() {
  struct tm tmNow;
  if (!getLocalTimeSafe(&tmNow)) return;

  const uint32_t now = millis();
  if ((now - lastClockReportMs) < CLOCK_REPORT_INTERVAL_MS) return;
  lastClockReportMs = now;

  char ts[24];
  formatDateTime(ts, sizeof(ts), false);
  Serial.println("=============================================");
  Serial.print("[real time @5min] ");
  Serial.println(ts);
  Serial.println("=============================================");
  Serial.println();
}

static void printRuntimeStatus() {
  char ts[24];
  formatDateTime(ts, sizeof(ts), true);
  Serial.println("=== STATUS ===");
  Serial.print("Time: ");
  Serial.println(ts);
  Serial.print("Alarm state: ");
  Serial.println(stateToString(state));
  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  Serial.print("Ping counter: ");
  Serial.println(pingCounter);
  Serial.println("==============");
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

  if (cmd == "HELP") {
    Serial.println("UART commands: ARM, DISARM, STATUS, PINGNOW, HELP");
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
  Serial.println();
  Serial.println("Alarma simpla starting...");
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
  ensureTimeSyncIfNeeded();
#if 0
  connectMqtt();
#endif
}

void loop() {
  const uint32_t now = millis();

  updateInternetCutRelayPulse();
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
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  ensureTimeSyncIfNeeded();
  pingGoogleIfNeeded();
  printClockEvery5MinIfNeeded();
}


