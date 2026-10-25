/**
 * Gas & Fire Monitor - ESP32 Firmware v1.2.0
 *
 * Hardware:
 *  - ESP32 NodeMCU
 *  - MQ-2 analog output : GPIO 34 (input-only pin)
 *  - DHT11 data         : GPIO 4
 *  - Relay (valve)      : GPIO 26
 *  - SIM800L            : ESP32 RX=25 (to SIM800L TX), ESP32 TX=33 (to SIM800L RX)
 *
 * Behaviour:
 *  - On gas/smoke/temperature danger: relay shuts the valve FIRST, then one SMS
 *    is queued. SMS is sent by the SIM800L directly (no API / Wi-Fi needed).
 *  - Safety loop (sensors + relay) never waits on Wi-Fi, HTTPS or SMS:
 *      loop()      -> sensors + safety logic          (core 1)
 *      networkTask -> telemetry, commands, config     (core 0)
 *      smsTask     -> SIM800L SMS with retry          (core 0)
 *  - Valve never reopens automatically. A remote open_valve is only accepted
 *    after readings have been safe for SAFE_CLEAR_MS.
 *
 * Serial monitor commands (115200 baud):
 *    phone=+63XXXXXXXXXX   save the SMS number to flash (survives reboot)
 *    testsms               queue a test SMS
 *
 * Libraries: ArduinoJson 7.x, DHT sensor library (Adafruit) + Adafruit Unified Sensor.
 * Copy secrets.h.example to secrets.h and fill it in (keep secrets.h out of Git).
 *
 * IMPORTANT:
 *  - Verify relay polarity with the gas line DISCONNECTED (see RELAY_*_LEVEL).
 *  - Gas threshold is a raw ADC value, NOT PPM. Calibrate the MQ-2.
 *  - This is not certified safety equipment.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

#include "FireApi.h"  // WIFI_SSID, WIFI_PASS, API_BASE_URL, DEFAULT_SMS_PHONE

// --------------------------------------------------
// PINS
// --------------------------------------------------
const int PIN_MQ2         = 34;
const int PIN_DHT         = 4;
const int PIN_RELAY_VALVE = 26;
#define GSM_RX_PIN 25
#define GSM_TX_PIN 33

#define DHTTYPE DHT11
DHT dht(PIN_DHT, DHTTYPE);
HardwareSerial sim800(2);

// --------------------------------------------------
// SAFETY CONFIGURATION
// --------------------------------------------------

// VERIFY physically with gas disconnected. CLOSE = valve power cut / valve shut.
// If your relay board is active-low, swap these two values.
const int RELAY_CLOSE_LEVEL = HIGH;
const int RELAY_OPEN_LEVEL  = LOW;

// Defaults; the backend can override both via GET /api/config.
volatile int   gasAdcDangerThreshold = 963;   // raw ADC (0-4095)
volatile float tempDangerThreshold   = 60.0;  // deg C

const float GAS_CLEAR_RATIO    = 0.90f;  // gas danger clears below 90% of threshold
const float TEMP_CLEAR_DELTA_C = 2.0f;   // temp danger clears 2 C below threshold

const unsigned long WARMUP_MS         = 180000;   // MQ-2 warm-up (a brand-new MQ-2 needs far longer once)
const unsigned long SAFE_CLEAR_MS     = 30000;   // safe time required before remote open
const unsigned long SMS_COOLDOWN_MS   = 300000;  // min gap between SMS messages (5 min)
const unsigned long FAULT_SMS_DELAY_MS = 60000;  // sensor fault must persist this long before an SMS

// Siren/alarm unit: sent on every danger event, ignores SMS cooldown
const char* SIREN_PHONE = "+639774719787";
const char* SIREN_CMD   = "1234#ON#";

const int      ADC_SAMPLES   = 10;
const uint8_t  DHT_MAX_FAILS = 5;     // consecutive failed DHT reads before "sensor fault"
const unsigned long DHT_INTERVAL_MS = 2000;

const int WDT_TIMEOUT_S = 10;

// --------------------------------------------------
// NETWORK / SMS CONFIGURATION
// --------------------------------------------------
const unsigned long READING_INTERVAL_MS = 3000;
const unsigned long CONFIG_INTERVAL_MS  = 60000;
const unsigned long HTTP_TIMEOUT_MS     = 5000;   // safe: runs in its own task

const int SMS_MAX_ATTEMPTS = 5;

const char* FIRMWARE_VERSION = "v1.2.0-esp32";

// --------------------------------------------------
// SHARED STATE (protect with stateMux)
// --------------------------------------------------
SemaphoreHandle_t stateMux;
QueueHandle_t     smsQueue;
Preferences       prefs;

#define LOCK()   xSemaphoreTake(stateMux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(stateMux)

bool  isValveClosed   = true;
int   gasRaw          = 0;
float temperatureC    = 0.0;
float humidity        = 0.0;
bool  gasSensorValid  = false;
bool  tempSensorValid = false;
bool  sensorsValid    = false;
bool  dangerDetected  = false;
bool  warmingUp       = true;
bool  gasInDanger     = false;
bool  tempInDanger    = false;
unsigned long safeSince = 0;
String smsPhone;

volatile bool urgentSend    = false;  // ask network task to send telemetry now
volatile int  smsLastStatus = -1;     // -1 none yet, 0 failed, 1 sent

struct SmsMsg { 
  char phone[32]; 
  char text[161]; 
};

// --------------------------------------------------
// RELAY HELPERS
// --------------------------------------------------

// Caller must hold the lock.
static void relayClose() {
  digitalWrite(PIN_RELAY_VALVE, RELAY_CLOSE_LEVEL);
  isValveClosed = true;
}

void closeValveLocal(const char* reason) {
  LOCK();
  bool wasOpen = !isValveClosed;
  relayClose();
  UNLOCK();
  if (wasOpen) Serial.printf("SHUTOFF: %s\n", reason);
}

// Returns nullptr on success, otherwise the reason it was refused.
const char* openValveIfSafe() {
  const unsigned long now = millis();
  const char* err = nullptr;

  LOCK();
  if (warmingUp)                                           err = "sensor warming up";
  else if (!sensorsValid)                                  err = "sensor fault";
  else if (dangerDetected)                                 err = "danger active";
  else if (safeSince == 0 || now - safeSince < SAFE_CLEAR_MS) err = "safe period not completed";

  if (!err) {
    digitalWrite(PIN_RELAY_VALVE, RELAY_OPEN_LEVEL);
    isValveClosed = false;
  }
  UNLOCK();

  if (err) Serial.printf("OPEN REFUSED: %s\n", err);
  else     Serial.println("Valve opened after safety checks.");
  return err;
}

// --------------------------------------------------
// SMS (SIM800L) - runs in its own task
// --------------------------------------------------

static void queueSms(const char* phone, const char* text) {
  SmsMsg m;
  m.phone[0] = 0;
  m.text[0] = 0;

  if (phone && strlen(phone) > 0) {
    strncpy(m.phone, phone, sizeof(m.phone) - 1);
  } else {
    LOCK();
    strncpy(m.phone, smsPhone.c_str(), sizeof(m.phone) - 1);
    UNLOCK();
  }
  m.phone[sizeof(m.phone) - 1] = 0;

  if (text) {
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
  }
  xQueueSend(smsQueue, &m, 0);
}

static void queueSms(const char* text) {
  queueSms(nullptr, text);
}

static void clearModemInput() {
  while (sim800.available()) sim800.read();
}

// Wait until `expect` appears in the modem output. Fails early on "ERROR".
static bool waitFor(const char* expect, uint32_t timeoutMs) {
  String buf;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (sim800.available()) buf += (char)sim800.read();
    if (buf.indexOf(expect) >= 0) return true;
    if (buf.indexOf("ERROR") >= 0) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

static bool modemCmd(const char* cmd, const char* expect, uint32_t timeoutMs) {
  clearModemInput();
  sim800.println(cmd);
  return waitFor(expect, timeoutMs);
}

static bool sendSmsOnce(const char* targetPhone, const char* text) {
  String phone;
  if (targetPhone && strlen(targetPhone) > 0) {
    phone = targetPhone;
  } else {
    LOCK();
    phone = smsPhone;
    UNLOCK();
  }

  phone.trim();
  if (phone.startsWith("09")) {
    phone = "+63" + phone.substring(1);
  }
  if (phone.length() < 8) return false;

  if (!modemCmd("AT", "OK", 1000))         return false;
  if (!modemCmd("AT+CMGF=1", "OK", 2000))  return false;

  clearModemInput();
  sim800.print("AT+CMGS=\"");
  sim800.print(phone);
  sim800.println("\"");
  if (!waitFor(">", 5000)) {
    sim800.write(27);  // ESC: cancel
    return false;
  }

  sim800.print(text);
  sim800.write(26);  // Ctrl+Z
  return waitFor("+CMGS:", 30000);  // modem confirms the message was accepted
}

void smsTask(void*) {
  SmsMsg msg;
  for (;;) {
    if (xQueueReceive(smsQueue, &msg, portMAX_DELAY) == pdTRUE) {
      bool ok = false;
      Serial.printf("SIM800L dispatching SMS to: %s\n", msg.phone);
      for (int attempt = 1; attempt <= SMS_MAX_ATTEMPTS && !ok; attempt++) {
        Serial.printf("SMS attempt %d to %s...\n", attempt, msg.phone);
        ok = sendSmsOnce(msg.phone, msg.text);
        if (!ok) vTaskDelay(pdMS_TO_TICKS(5000));
      }
      smsLastStatus = ok ? 1 : 0;
      Serial.println(ok ? "SMS sent successfully." : "SMS FAILED after all attempts.");
    }
  }
}

// --------------------------------------------------
// SENSORS
// --------------------------------------------------

void readSensors() {
  // Average several samples to reduce ADC noise.
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_MQ2);
    delayMicroseconds(500);
  }
  const int raw = sum / ADC_SAMPLES;

  // DHT11 is slow; read it every few seconds and debounce failures.
  static unsigned long lastDht = 0;
  static uint8_t dhtFails = 0;
  bool dhtRead = false, dhtOk = false;
  float t = 0, h = 0;

  const unsigned long now = millis();
  if (now - lastDht >= DHT_INTERVAL_MS) {
    lastDht = now;
    t = dht.readTemperature();
    h = dht.readHumidity();
    dhtRead = true;
    dhtOk = !isnan(t) && !isnan(h) && h >= 0.0 && h <= 100.0;
  }

  LOCK();
  gasRaw = raw;
  // Disconnected / shorted-to-ground check. A saturated ADC (~4095) is NOT a
  // fault: it is treated as extreme gas and trips the danger threshold.
  gasSensorValid = (raw > 5);

  if (dhtRead) {
    if (dhtOk) {
      temperatureC = t;
      humidity = h;
      dhtFails = 0;
      tempSensorValid = true;
    } else if (++dhtFails >= DHT_MAX_FAILS) {
      tempSensorValid = false;
    }
  }
  sensorsValid = gasSensorValid && tempSensorValid;
  UNLOCK();
}

// --------------------------------------------------
// SAFETY LOGIC (relay first, then SMS)
// --------------------------------------------------

void evaluateSafety() {
  const unsigned long now = millis();

  static bool warmDone = false;
  if (!warmDone && now >= WARMUP_MS) {
    warmDone = true;
    Serial.println("MQ-2 warm-up complete.");
  }

  static bool dangerAlertSent = false, sirenAlertSent = false, faultSmsSent = false, smsEverSent = false;
  static unsigned long faultSince = 0, lastSmsMs = 0;

  const int   gasThr  = gasAdcDangerThreshold;
  const float tempThr = tempDangerThreshold;

  bool gasDanger, tempDanger, fault, wasDanger, danger, wasOpen;
  unsigned long safeSinceCopy;

  LOCK();
  warmingUp = !warmDone;

  // Gas with hysteresis (ignored during warm-up).
  if (!warmDone) {
    gasInDanger = false;
  } else if (gasSensorValid) {
    if (!gasInDanger && gasRaw >= gasThr)                          gasInDanger = true;
    else if (gasInDanger && gasRaw < gasThr * GAS_CLEAR_RATIO)     gasInDanger = false;
  }

  // Temperature with hysteresis.
  if (tempSensorValid) {
    if (!tempInDanger && temperatureC >= tempThr)                  tempInDanger = true;
    else if (tempInDanger && temperatureC < tempThr - TEMP_CLEAR_DELTA_C) tempInDanger = false;
  }

  gasDanger  = gasInDanger;
  tempDanger = tempInDanger;
  fault      = warmDone && !sensorsValid;

  wasDanger      = dangerDetected;
  dangerDetected = gasDanger || tempDanger || fault;
  danger         = dangerDetected;

  if (!warmDone || dangerDetected) safeSince = 0;
  else if (safeSince == 0)         safeSince = now;
  safeSinceCopy = safeSince;

  // 1) RELAY FIRST: shut the valve (also held closed during warm-up).
  wasOpen = !isValveClosed;
  if (!warmDone || dangerDetected) relayClose();
  UNLOCK();

  if (wasOpen && danger) {
    Serial.printf("SHUTOFF: %s\n", fault ? "Sensor fault: fail-safe shutdown"
                                        : "Gas or temperature danger detected");
  }

  // Send telemetry right away instead of waiting for the next 3s slot.
  if (danger && !wasDanger) urgentSend = true;

  // 2) SMS (queued; sent by smsTask without blocking this loop).
  auto smsAllowed = [&]() { return !smsEverSent || (now - lastSmsMs) >= SMS_COOLDOWN_MS; };

  if (gasDanger || tempDanger) {
    // Siren/alarm unit: sent on every danger event, ignores SMS cooldown
    if (!sirenAlertSent) {
      queueSms(SIREN_PHONE, SIREN_CMD);
      sirenAlertSent = true;
      Serial.printf("Siren alarm trigger sent to %s: %s\n", SIREN_PHONE, SIREN_CMD);
    }

    if (!dangerAlertSent && smsAllowed()) {
      queueSms(gasDanger
        ? "ALERT: Gas/smoke detected. LPG valve has been shut off. Check the site safely."
        : "ALERT: High temperature detected. LPG valve has been shut off. Check the site safely.");
      dangerAlertSent = true;
      smsEverSent = true;
      lastSmsMs = now;
    }
  } else if (!danger && safeSinceCopy != 0 && (now - safeSinceCopy) >= SAFE_CLEAR_MS) {
    dangerAlertSent = false;  // re-arm after stable safe readings
    sirenAlertSent  = false;  // re-arm siren unit
  }

  // Sensor fault: valve already closed; only text if it persists.
  if (fault) {
    if (faultSince == 0) faultSince = now;
    if (!faultSmsSent && (now - faultSince) >= FAULT_SMS_DELAY_MS && smsAllowed()) {
      queueSms("WARNING: Sensor fault on gas monitor. LPG valve shut off as a precaution.");
      faultSmsSent = true;
      smsEverSent = true;
      lastSmsMs = now;
    }
  } else {
    faultSince = 0;
    faultSmsSent = false;
  }
}

// --------------------------------------------------
// HTTPS HELPERS (network task only)
// --------------------------------------------------

// GET when payload is empty, otherwise POST JSON. Returns HTTP code or -1.
static int httpRequest(WiFiClientSecure& client, const String& path,
                      const String& payload, String* response) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, String(API_BASE_URL) + path)) return -1;

  int code;
  if (payload.length()) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(payload);
  } else {
    code = http.GET();
  }

  if (response && code > 0) *response = http.getString();
  http.end();
  return code;
}

// --------------------------------------------------
// CLOUD CONFIG
// --------------------------------------------------

void syncConfig(WiFiClientSecure& client) {
  String body;
  if (httpRequest(client, "/config", "", &body) != 200) return;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return;

  if (!doc["gas_adc_danger_threshold"].isNull()) {
    int thr = doc["gas_adc_danger_threshold"].as<int>();
    if (thr > 5 && thr < 4090) gasAdcDangerThreshold = thr;
  } else if (!doc["gas_danger_threshold"].isNull()) {
    int thr = doc["gas_danger_threshold"].as<int>();
    if (thr > 5 && thr < 4090) gasAdcDangerThreshold = thr;
  }
  if (!doc["temp_danger_threshold"].isNull()) {
    float thr = doc["temp_danger_threshold"].as<float>();
    if (thr > 0 && thr <= 150) tempDangerThreshold = thr;
  }

  Serial.printf("Config: gas ADC >= %d, temperature >= %.1f C\n",
                (int)gasAdcDangerThreshold, (float)tempDangerThreshold);
}

// --------------------------------------------------
// TELEMETRY
// --------------------------------------------------

void sendReadings(WiFiClientSecure& client) {
  int   gas;
  float temp, hum;
  bool  gasValid, tempValid, danger, valveClosed, warming;

  LOCK();
  gas = gasRaw; temp = temperatureC; hum = humidity;
  gasValid = gasSensorValid; tempValid = tempSensorValid;
  danger = dangerDetected; valveClosed = isValveClosed; warming = warmingUp;
  UNLOCK();

  const int s = smsLastStatus;

  JsonDocument doc;
  doc["gas"]               = gas;            // raw ADC, NOT PPM
  doc["gas_adc"]           = gas;
  doc["smoke"]             = gas / 10.0;     // derived from the same MQ-2
  doc["temperature"]       = temp;
  doc["humidity"]          = hum;
  doc["gas_sensor_valid"]  = gasValid;
  doc["temp_sensor_valid"] = tempValid;
  doc["danger_detected"]   = danger;
  doc["valve_closed"]      = valveClosed;
  doc["warming_up"]        = warming;
  doc["sms_last_status"]   = (s == 1) ? "sent" : (s == 0) ? "failed" : "none";
  doc["rssi"]              = WiFi.RSSI();
  doc["uptime"]            = millis() / 1000;
  doc["firmware_version"]  = FIRMWARE_VERSION;

  String body;
  serializeJson(doc, body);

  int code = httpRequest(client, "/readings", body, nullptr);
  if (code < 200 || code >= 300) Serial.printf("Telemetry failed: HTTP %d\n", code);
}

// --------------------------------------------------
// REMOTE COMMANDS
// --------------------------------------------------

void checkCommands(WiFiClientSecure& client) {
  String body;
  if (httpRequest(client, "/commands", "", &body) != 200) return;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  if (doc["command"].isNull()) return;

  String id   = doc["command"]["id"]      | "";
  String name = doc["command"]["command"] | "";
  if (id.isEmpty()) return;

  // Remember the last result so a failed report can be re-sent
  // without executing the command twice.
  static String lastId, lastStatus, lastError;

  if (id != lastId) {
    lastId = id;
    lastError = "";

    if (name == "close_valve") {
      closeValveLocal("Remote shutdown command");
      lastStatus = "executed";
    } else if (name == "open_valve") {
      const char* err = openValveIfSafe();
      if (!err) {
        lastStatus = "executed";
      } else {
        lastStatus = "rejected";
        lastError = err;
      }
    } else if (name == "send_sms") {
      String msg = doc["command"]["payload"]["message"] | "ALERT: Emergency gas/fire safety alert.";
      String phone = doc["command"]["payload"]["phone"] | "";
      phone.trim();
      if (phone.startsWith("09")) {
        phone = "+63" + phone.substring(1);
      }
      const char* target = (phone.length() >= 8) ? phone.c_str() : nullptr;
      queueSms(target, msg.c_str());
      lastStatus = "executed";
      Serial.printf("Command send_sms queued for: %s\n", target ? target : "default");
    } else {
      lastStatus = "rejected";
      lastError = "unsupported command";
    }
  }

  JsonDocument res;
  res["status"] = lastStatus;
  if (lastError.length()) res["error_message"] = lastError;

  String resBody;
  serializeJson(res, resBody);
  httpRequest(client, "/commands/" + lastId + "/result", resBody, nullptr);
}

// --------------------------------------------------
// NETWORK TASK
// --------------------------------------------------

void networkTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  WiFiClientSecure client;
  client.setInsecure();  // prototype: skips certificate validation

  unsigned long lastReading = 0, lastConfig = 0, lastWifiTry = millis();
  bool firstConfig = true;

  for (;;) {
    const unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiTry >= 10000) {
        lastWifiTry = now;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (firstConfig || now - lastConfig >= CONFIG_INTERVAL_MS) {
      firstConfig = false;
      lastConfig = now;
      syncConfig(client);
    }

    if (urgentSend || now - lastReading >= READING_INTERVAL_MS) {
      urgentSend = false;
      lastReading = now;
      sendReadings(client);
      checkCommands(client);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --------------------------------------------------
// SERIAL COMMANDS + STATUS PRINT
// --------------------------------------------------

void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.startsWith("phone=")) {
        String p = line.substring(6);
        p.trim();
        if (p.length() >= 8 && p.length() <= 16 && p[0] == '+') {
          prefs.putString("sms_phone", p);
          LOCK();
          smsPhone = p;
          UNLOCK();
          Serial.println("SMS number saved.");
        } else {
          Serial.println("Invalid number. Use international format, e.g. +639XXXXXXXXX");
        }
      } else if (line == "testsms") {
        queueSms("TEST: Gas monitor SMS check.");
        Serial.println("Test SMS queued.");
      } else if (line == "testsiren") {
        queueSms(SIREN_PHONE, SIREN_CMD);
        Serial.printf("Test Siren SMS queued to %s: %s\n", SIREN_PHONE, SIREN_CMD);
      }
      line = "";
    } else if (line.length() < 40) {
      line += c;
    }
  }
}

void printStatus() {
  static unsigned long last = 0;
  const unsigned long now = millis();
  if (now - last < 1000) return;
  last = now;

  LOCK();
  Serial.printf("Gas ADC: %d | Temp: %.1f C | Hum: %.1f %% | Valid: %s | Danger: %s | Valve: %s%s\n",
                gasRaw, temperatureC, humidity,
                sensorsValid ? "YES" : "NO",
                dangerDetected ? "YES" : "no",
                isValveClosed ? "CLOSED" : "OPEN",
                warmingUp ? " | WARMING UP" : "");
  UNLOCK();
}

// --------------------------------------------------
// SETUP / LOOP
// --------------------------------------------------

void setup() {
  // Drive the relay to the closed state as early as possible.
  pinMode(PIN_RELAY_VALVE, OUTPUT);
  digitalWrite(PIN_RELAY_VALVE, RELAY_CLOSE_LEVEL);

  Serial.begin(115200);
  delay(500);
  Serial.println("\nGas & Fire Monitor starting...");

  stateMux = xSemaphoreCreateMutex();
  smsQueue = xQueueCreate(10, sizeof(SmsMsg));

  prefs.begin("fireline", false);
  smsPhone = prefs.getString("sms_phone", DEFAULT_SMS_PHONE);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);
  dht.begin();

  sim800.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

  // Watchdog on the safety loop: a hang causes a reset (valve boots closed).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtCfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);

  xTaskCreatePinnedToCore(smsTask,     "sms", 4096,  NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(networkTask, "net", 16384, NULL, 1, NULL, 0);

  Serial.println("Local monitoring active. Valve stays closed until a safe remote open.");
}

void loop() {
  esp_task_wdt_reset();

  handleSerial();
  readSensors();
  evaluateSafety();
  printStatus();

  delay(100);
}
