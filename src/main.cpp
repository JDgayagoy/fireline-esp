
/**
 * Gas & Fire Monitor - ESP32 Firmware (Safer Prototype)
 *
 * Hardware:
 * - ESP32 NodeMCU
 * - MQ-2 analog output: GPIO 34
 * - DHT11 digital data: GPIO 33 (or GPIO 4, 14, 25, 32; GPIO 34-39 are input-only)
 * - Relay: GPIO 26
 * - Buzzer: GPIO 27
 * - SIM800L: RX=16, TX=17
 *
 * IMPORTANT:
 * - Verify relay polarity and valve behavior before use.
 * - Gas threshold is raw ADC, NOT PPM.
 * - DHT11 measures temperature (0-50 C) and relative humidity (20-90% RH).
 * - This is not certified safety equipment.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Wi-Fi
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// API URL; use your actual reachable backend URL.
const char* API_BASE_URL = "http://192.168.1.100:3001/api";

// Pins
const int PIN_MQ2 = 34;
const int PIN_DHT = 33; // DHT11 single-wire digital data pin (use GPIO 33, 4, 14, 25, or 32)
const int PIN_RELAY_VALVE = 26;
const int PIN_BUZZER = 27;

#define DHTTYPE DHT11
DHT dht(PIN_DHT, DHTTYPE);

#define GSM_RX_PIN 16
#define GSM_TX_PIN 17
HardwareSerial sim800(2);

const char* EMERGENCY_SMS_PHONE = "+639171234567";

// --------------------------------------------------
// SAFETY CONFIGURATION
// --------------------------------------------------

// Assumes HIGH closes the valve and LOW opens it.
// VERIFY this physically with gas disconnected.
const int RELAY_CLOSE_LEVEL = HIGH;
const int RELAY_OPEN_LEVEL = LOW;

// Initial prototype threshold only; calibrate the MQ-2.
int gasAdcDangerThreshold = 963;

// LM35 temperature threshold.
float tempDangerThreshold = 60.0;

// Must remain safe for this long before remote opening.
const unsigned long SAFE_CLEAR_MS = 30000;

// Send readings every 3 seconds.
const unsigned long READING_INTERVAL_MS = 3000;

// --------------------------------------------------
// DEVICE STATE
// --------------------------------------------------

bool isValveClosed = true;
bool dangerAlertSent = false;

unsigned long lastReadingTime = 0;
unsigned long safeSince = 0;

int gasRaw = 0;
float temperatureC = 0.0;
float humidity = 0.0;

bool gasSensorValid = false;
bool tempSensorValid = false;
bool sensorsValid = false;
bool dangerDetected = true;

// Function declarations
void readSensors();
void evaluateSafety();
void syncConfig();
void sendReadings();
void checkCommands();
void closeValveLocal(const char* reason);
bool openValveIfSafe();
void sendSMSAlert(const char* message);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\nGas & Fire Monitor starting...");

  // Configure ADC.
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);

  // Initialize DHT11 sensor.
  dht.begin();

  pinMode(PIN_RELAY_VALVE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Start in the configured CLOSED state.
  // Confirm relay polarity before connecting the valve.
  digitalWrite(PIN_RELAY_VALVE, RELAY_CLOSE_LEVEL);
  digitalWrite(PIN_BUZZER, LOW);
  isValveClosed = true;

  // Initialize GSM.
  sim800.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(1000);
  sim800.println("AT");

  // Connect to Wi-Fi without blocking indefinitely.
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: %s\n",
                  WiFi.localIP().toString().c_str());
    syncConfig();
  } else {
    Serial.println("Wi-Fi unavailable. Local monitoring continues.");
  }

  // The valve remains closed after startup.
  // A remote open command is required, and safety checks
  // must pass before it can be accepted.
}

void loop() {
  unsigned long now = millis();

  // Check safety locally, independently of Wi-Fi.
  readSensors();
  evaluateSafety();

  // Cloud telemetry and commands.
  if (now - lastReadingTime >= READING_INTERVAL_MS) {
    lastReadingTime = now;

    if (WiFi.status() == WL_CONNECTED) {
      sendReadings();
      checkCommands();
    }
  }

  delay(100);
}

// --------------------------------------------------
// SENSOR READING
// --------------------------------------------------

void readSensors() {
  gasRaw = analogRead(PIN_MQ2);

  // Read DHT11 Temperature and Humidity.
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h) && h >= 0.0 && h <= 100.0) {
    temperatureC = t;
    humidity = h;
    tempSensorValid = true;
  } else {
    // DHT11 read transient failure or disconnected.
    tempSensorValid = false;
  }

  // Basic disconnected/shorted-sensor checks for MQ-2.
  gasSensorValid = (gasRaw > 5 && gasRaw < 4090);

  sensorsValid = gasSensorValid && tempSensorValid;

  Serial.printf(
    "Gas ADC: %d | Temp: %.1f C | Humidity: %.1f %% | Sensors valid: %s\n",
    gasRaw,
    temperatureC,
    humidity,
    sensorsValid ? "YES" : "NO"
  );
}

// --------------------------------------------------
// LOCAL SAFETY LOGIC
// --------------------------------------------------

void evaluateSafety() {
  bool gasDanger = gasSensorValid &&
                   gasRaw >= gasAdcDangerThreshold;

  bool tempDanger = tempSensorValid &&
                    temperatureC >= tempDangerThreshold;

  // Sensor faults are treated as unsafe.
  dangerDetected = !sensorsValid || gasDanger || tempDanger;

  if (dangerDetected) {
    safeSince = 0;

    closeValveLocal(
      !sensorsValid
        ? "Sensor fault: fail-safe shutdown"
        : "Gas or temperature danger detected"
    );

    digitalWrite(PIN_BUZZER, HIGH);

    if (!dangerAlertSent) {
      sendSMSAlert(
        "ALERT: Gas/temperature danger or sensor fault. "
        "Valve shutdown commanded. Check the site safely."
      );
      dangerAlertSent = true;
    }
  } else {
    // Do not reopen automatically when readings recover.
    digitalWrite(PIN_BUZZER, LOW);

    if (safeSince == 0) {
      safeSince = millis();
    }

    // Permit a future reset attempt after stable safe readings.
    if (millis() - safeSince >= SAFE_CLEAR_MS) {
      dangerAlertSent = false;
    }
  }
}

void closeValveLocal(const char* reason) {
  Serial.printf("SHUTOFF: %s\n", reason);

  digitalWrite(PIN_RELAY_VALVE, RELAY_CLOSE_LEVEL);
  isValveClosed = true;
}

bool openValveIfSafe() {
  // Never open on a sensor fault or active danger.
  if (!sensorsValid || dangerDetected || safeSince == 0) {
    Serial.println("OPEN REFUSED: readings are unsafe or invalid.");
    return false;
  }

  if (millis() - safeSince < SAFE_CLEAR_MS) {
    Serial.println("OPEN REFUSED: safe period not completed.");
    return false;
  }

  // This command is still subject to physical valve/relay
  // verification and a validated reset procedure.
  digitalWrite(PIN_RELAY_VALVE, RELAY_OPEN_LEVEL);
  isValveClosed = false;

  Serial.println("Valve-open command accepted after safety checks.");
  return true;
}

// --------------------------------------------------
// SMS
// --------------------------------------------------

void sendSMSAlert(const char* message) {
  Serial.println("Attempting emergency SMS...");

  sim800.println("AT+CMGF=1");
  delay(300);

  sim800.print("AT+CMGS=\"");
  sim800.print(EMERGENCY_SMS_PHONE);
  sim800.println("\"");
  delay(500);

  sim800.print(message);
  sim800.write(26);  // Ctrl+Z
  delay(3000);

  // NOTE: A delay does not confirm SMS delivery.
  // Production code should parse modem responses.
  Serial.println("SMS submission attempted.");
}

// --------------------------------------------------
// CLOUD CONFIGURATION
// --------------------------------------------------

void syncConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(3000);

  String url = String(API_BASE_URL) + "/config";

  if (!http.begin(url)) return;

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError error =
        deserializeJson(doc, http.getString());

    if (!error) {
      // Use the new raw-ADC configuration key.
      // Do not interpret the old gas_danger_threshold
      // value as PPM or as a raw ADC threshold.
      if (doc["gas_adc_danger_threshold"].is<int>()) {
        int threshold = doc["gas_adc_danger_threshold"];

        if (threshold > 5 && threshold < 4090) {
          gasAdcDangerThreshold = threshold;
        }
      }

      if (doc["temp_danger_threshold"].is<float>() ||
          doc["temp_danger_threshold"].is<int>()) {
        float threshold = doc["temp_danger_threshold"];

        if (threshold > 0 && threshold <= 150) {
          tempDangerThreshold = threshold;
        }
      }

      Serial.printf(
        "Config: gas ADC >= %d, temperature >= %.1f C\n",
        gasAdcDangerThreshold,
        tempDangerThreshold
      );
    }
  }

  http.end();
}

// --------------------------------------------------
// TELEMETRY
// --------------------------------------------------

void sendReadings() {
  HTTPClient http;
  http.setTimeout(3000);

  String url = String(API_BASE_URL) + "/readings";

  if (!http.begin(url)) return;

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;

  // gas is now the raw ADC value, NOT PPM.
  // Update backend/dashboard labels accordingly.
  doc["gas"] = gasRaw;
  doc["gas_adc"] = gasRaw;
  doc["smoke"] = gasRaw / 10.0;
  doc["temperature"] = temperatureC;
  doc["humidity"] = humidity;
  doc["gas_sensor_valid"] = gasSensorValid;
  doc["temp_sensor_valid"] = tempSensorValid;
  doc["danger_detected"] = dangerDetected;
  doc["valve_closed"] = isValveClosed;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["firmware_version"] = "v1.1.0-esp32";

  String requestBody;
  serializeJson(doc, requestBody);

  int httpCode = http.POST(requestBody);

  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("Telemetry failed: HTTP %d\n", httpCode);
  }

  http.end();
}

// --------------------------------------------------
// REMOTE COMMANDS
// --------------------------------------------------

void checkCommands() {
  HTTPClient http;
  http.setTimeout(3000);

  String url = String(API_BASE_URL) + "/commands";

  if (!http.begin(url)) return;

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;

    DeserializationError error =
        deserializeJson(doc, payload);

    if (!error && !doc["command"].isNull()) {
      String cmdId = doc["command"]["id"] | "";
      String commandName = doc["command"]["command"] | "";

      bool executed = false;

      if (commandName == "close_valve") {
        closeValveLocal("Remote shutdown command");
        executed = true;
      } else if (commandName == "open_valve") {
        // Re-check current readings and safe duration.
        executed = openValveIfSafe();
      }

      if (!cmdId.isEmpty()) {
        HTTPClient reportHttp;
        reportHttp.setTimeout(3000);

        String resultUrl =
            String(API_BASE_URL) + "/commands/" +
            cmdId + "/result";

        if (reportHttp.begin(resultUrl)) {
          reportHttp.addHeader("Content-Type", "application/json");

          String result = executed
              ? "{\"status\":\"executed\"}"
              : "{\"status\":\"rejected\"}";

          reportHttp.POST(result);
          reportHttp.end();
        }
      }
    }
  }

  http.end();
}
