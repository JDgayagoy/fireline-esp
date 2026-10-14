/**
 * Gas & Fire Monitor - ESP32 Firmware
 * 
 * Hardware:
 * - ESP32 NodeMCU Development Board
 * - MQ-2 Gas & Smoke Sensor (Analog Pin 34)
 * - LM35 or Analog Temperature Sensor (Analog Pin 35)
 * - CW-025 12V Relay driving LPG Solenoid Valve (GPIO 26)
 * - SIM800L GSM Module (Serial2: RX=16, TX=17)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Wi-Fi Credentials
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// API Base URL (Render deployment or local IP)
const char* API_BASE_URL = "http://192.168.1.100:3001/api";

// Pin Assignments
const int PIN_MQ2 = 34;          // ADC1_CH6
const int PIN_TEMP = 35;         // ADC1_CH7
const int PIN_RELAY_VALVE = 26;  // Relay CW-025 (Active LOW or HIGH depending on module)
const int PIN_BUZZER = 27;       // Optional local piezo buzzer

// SIM800L Hardware Serial 2
#define GSM_RX_PIN 16
#define GSM_TX_PIN 17
HardwareSerial sim800(2);

// Emergency SMS target number
const char* EMERGENCY_SMS_PHONE = "+639171234567";

// Local Safety Defaults (Overridden dynamically from /api/config)
float gasDangerThreshold = 300.0;
float tempDangerThreshold = 60.0;
float cleanAirBaseline = 85.0;

// Device State
bool isValveClosed = false;
unsigned long lastReadingTime = 0;
const unsigned long READING_INTERVAL_MS = 3000;

// Function Prototypes
void syncConfig();
void sendReadings();
void checkCommands();
void closeValveLocal(const char* reason);
void sendSMSAlert(const char* message);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n🔥 Gas & Fire Monitor ESP32 Firmware starting...");

  // Pin Modes
  pinMode(PIN_RELAY_VALVE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_RELAY_VALVE, LOW); // Solenoid valve open by default
  digitalWrite(PIN_BUZZER, LOW);

  // Initialize GSM Serial
  sim800.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(1000);
  sim800.println("AT"); // Test GSM

  // Connect to Wi-Fi
  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n✅ Wi-Fi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    syncConfig();
  } else {
    Serial.println("\n⚠️ Wi-Fi connection timed out. Running in standalone autonomous safety mode.");
  }
}

void loop() {
  unsigned long now = millis();

  // 1. Read Sensors
  int rawGas = analogRead(PIN_MQ2);
  int rawTemp = analogRead(PIN_TEMP);

  // Calibration curve approximation for MQ-2 and temperature
  float gasPPM = map(rawGas, 0, 4095, (long)cleanAirBaseline, 1000);
  float temperatureC = (rawTemp * (3.3 / 4095.0)) * 100.0; // e.g. LM35 10mV/°C
  float smokeLevel = rawGas / 10.0;

  // 2. CRITICAL AUTONOMOUS FAIL-SAFE:
  // Evaluates safety rules locally even without network connection!
  if ((gasPPM >= gasDangerThreshold || temperatureC >= tempDangerThreshold) && !isValveClosed) {
    closeValveLocal("Local Autonomous Rule: Danger threshold breached");
    sendSMSAlert("EMERGENCY: Gas/Fire danger threshold breached! Solenoid valve has been closed automatically.");
  }

  // 3. Periodic Cloud Telemetry & Command Polling
  if (now - lastReadingTime >= READING_INTERVAL_MS) {
    lastReadingTime = now;

    if (WiFi.status() == WL_CONNECTED) {
      sendReadings(gasPPM, smokeLevel, temperatureC);
      checkCommands();
    }
  }

  delay(50);
}

void closeValveLocal(const char* reason) {
  Serial.printf("🚨 [EMERGENCY SHUTOFF] %s\n", reason);
  digitalWrite(PIN_RELAY_VALVE, HIGH); // Trip relay to close solenoid
  digitalWrite(PIN_BUZZER, HIGH);
  isValveClosed = true;
}

void openValveLocal() {
  Serial.println("🔓 [VALVE REOPENED] Solenoid valve energized open.");
  digitalWrite(PIN_RELAY_VALVE, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  isValveClosed = false;
}

void sendSMSAlert(const char* message) {
  Serial.println("📱 Sending Emergency SMS via SIM800L...");
  sim800.println("AT+CMGF=1"); // Text mode
  delay(200);
  sim800.printf("AT+CMGS=\"%s\"\r\n", EMERGENCY_SMS_PHONE);
  delay(200);
  sim800.print(message);
  delay(100);
  sim800.write(26); // Ctrl+Z to send
  delay(3000);
  Serial.println("📱 SMS sent.");
}

void syncConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(API_BASE_URL) + "/config";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      gasDangerThreshold = doc["gas_danger_threshold"] | gasDangerThreshold;
      tempDangerThreshold = doc["temp_danger_threshold"] | tempDangerThreshold;
      cleanAirBaseline = doc["clean_air_baseline"] | cleanAirBaseline;
      Serial.printf("📋 Synced config: Gas Danger > %.1f PPM, Temp Danger > %.1f C\n", gasDangerThreshold, tempDangerThreshold);
    }
  }
  http.end();
}

void sendReadings(float gas, float smoke, float temp) {
  HTTPClient http;
  String url = String(API_BASE_URL) + "/readings";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["gas"] = gas;
  doc["smoke"] = smoke;
  doc["temperature"] = temp;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["firmware_version"] = "v1.0.0-esp32";

  String requestBody;
  serializeJson(doc, requestBody);

  int httpCode = http.POST(requestBody);
  if (httpCode > 0) {
    // Reading accepted
  }
  http.end();
}

void checkCommands() {
  HTTPClient http;
  String url = String(API_BASE_URL) + "/commands";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    if (!doc["command"].isNull()) {
      const char* cmdId = doc["command"]["id"];
      const char* commandName = doc["command"]["command"];
      Serial.printf("📥 Cloud Command: %s (ID: %s)\n", commandName, cmdId);

      bool executed = false;
      if (strcmp(commandName, "close_valve") == 0) {
        closeValveLocal("Remote cloud shutoff command");
        executed = true;
      } else if (strcmp(commandName, "open_valve") == 0) {
        openValveLocal();
        executed = true;
      }

      if (executed) {
        // Report result back
        HTTPClient reportHttp;
        String resultUrl = String(API_BASE_URL) + "/commands/" + String(cmdId) + "/result";
        reportHttp.begin(resultUrl);
        reportHttp.addHeader("Content-Type", "application/json");
        reportHttp.POST("{\"status\":\"executed\"}");
        reportHttp.end();
      }
    }
  }
  http.end();
}
