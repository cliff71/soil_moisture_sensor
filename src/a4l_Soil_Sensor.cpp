#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <EEPROM.h>
#include "OneButton.h"
#include <Ticker.h>
#include "blinker.h"
#include "a4lUtilities.h"
#include "a4l_secrets.h"
#include <ArduinoJson.h>
#include "BLESmartConfig.h"
#include "SPIFFS.h"

void btnPress();
void btnReset();
void setBLESmartConfig();
bool wifiRequest();
int batteryLevel();
bool sendRefreshMsg();
int readADC_Averaged(int pin, int samples);
float sensorValue();
void wifiSetRTC();
bool getTimeWithRetry();
bool updateDevice();
String sendToBackend(const String &data);

struct MyObject {
  char link_id[30];    // 16 bytes - Reduced string size
  uint8_t state;            // 1 byte - All state flags combined into one byte
  char ssid[40];            // 26 bytes - Reduced string size
  char password[16];        // 16 bytes - Reduced string size
  unsigned long soil_last_updated;
  unsigned long water_last_updated;
  uint8_t callWifiManager;  // 1 byte - combine callWifiManager and other flags
  unsigned long clientId;
  char initialised[4];      // 4 bytes - unchanged
  char endbite;             // 1 byte - unchanged
} customVar;

DeviceInfo deviceInfo = {
  "Smart Watering",         // Device type
  "MS.2.0.0",               // Device version
  "ESP32",                  // Device processor
  "SWITCH",                 // Device category
  "Smart Watering System",  // Device description
  "apps4leisure",           // Device manufacturer
  "MS"                      // Device type short
};

enum State{
  CONFIG_WIFI,
  WAIT_FOR_JSON,
  JSON_ERROR,
  CONNECT_WIFI,
  ADD_DEVICE,
  PROCESSING,
  IDEL
};
State state = IDEL;
State lastState = IDEL;

struct tm timeinfo;

#define RESET_BUTTON_PIN  0
#define LED_PIN          10
#define SENSOR_EN_PIN     5
#define SENSOR_LEVEL_PIN  4
#define BATTERY_LEVEL_PIN 2
#define ON_CHARGE_PIN     1

static int sleep_minutes = 30;


char uid[20] = "";

boolean wifiConnected = false;

OneButton button1(RESET_BUTTON_PIN, true);

unsigned long previousMillis = 0;  // last time update
unsigned long lastUpdateMillis = 0;

a4lUtilities utilities;

BLESmartConfig bleConfig;

void handleJson(const String& jsonPayload) {
  Serial.println("Callback triggered with JSON payload:");
  Serial.println(jsonPayload);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
      state = JSON_ERROR;
      return;
  }

  // Check and copy each key only if it exists
  if (!doc["wifi_ssid"].isNull()) {
      const char* tmpChar = doc["wifi_ssid"];
      strcpy(customVar.ssid, tmpChar);
      Serial.println(tmpChar);
  } else {
      Serial.println("Key 'wifi_ssid' is missing!");
  }

  if (!doc["wifi_password"].isNull()) {
      const char* tmpChar = doc["wifi_password"];
      strcpy(customVar.password, tmpChar);
      Serial.println(tmpChar);
  } else {
      Serial.println("Key 'wifi_password' is missing!");
  }

  if (!doc["link_id"].isNull()) {
      const char* tmpChar = doc["link_id"];
      strcpy(customVar.link_id, tmpChar);
      Serial.println(tmpChar);
  } else {
      Serial.println("Key 'link_id' is missing!");
  }

  if (!doc["clientId"].isNull()) {
      const char* tmpChar = doc["clientId"];
      customVar.clientId = atol(tmpChar);
      Serial.println(tmpChar);
  } else {
      Serial.println("Key 'clientId' is missing!");
  }
  strcpy(customVar.initialised, "YES");
  customVar.callWifiManager = false;
  EEPROM.put(0, customVar);
  EEPROM.commit();
  Serial.println("Config saved");

  state = ADD_DEVICE;
}


void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(SENSOR_EN_PIN, OUTPUT);
  pinMode(ON_CHARGE_PIN, INPUT);
  digitalWrite(LED_PIN, HIGH);

  button1.attachClick(btnPress);
  button1.attachLongPressStop(btnReset);

  Serial.begin(115200);
  delay(100);

  EEPROM.begin(512);
  EEPROM.get(0, customVar);

  String tmp_uid = WiFi.macAddress();
  tmp_uid.replace(":", "");      // Remove colons
  tmp_uid = "a4l-" + tmp_uid;    // Prepend "a4l-"
  strcat(uid, tmp_uid.c_str());

  if((strcmp(customVar.initialised, "YES") != 0)){
    Serial.println("Device ready to initialised");
    for(int i = 0; i< 512; i++){
      EEPROM.put(i , 0);
    }
    EEPROM.put(0 , customVar );
    EEPROM.commit();
    state = CONFIG_WIFI;
  }else{
    state = CONNECT_WIFI;
  }
}

void goToSleep() {
  //return;
  Serial.println("Going to sleep");
  digitalWrite(SENSOR_EN_PIN, HIGH);
  pinMode(SENSOR_EN_PIN, INPUT_PULLDOWN);
  delay(1000);
  esp_sleep_enable_timer_wakeup((uint64_t)sleep_minutes * 60ULL * 1000000ULL);
  // Button wake: GPIO0 pulled LOW.
  esp_deep_sleep_enable_gpio_wakeup(
      1ULL << 0,
      ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void loop() {
  switch (state) {
    case CONFIG_WIFI:
      if (state != lastState) {
        lastState = state;
        Serial.println("Config wifi");
        setBLESmartConfig();
        state = WAIT_FOR_JSON;
      }
      break;
    case ADD_DEVICE: {
      if (state != lastState) {
        state = lastState;
        bleConfig.stopBLESmartConfig();
        Serial.println("Update device");
        //updateDevice();
        //  registerDevice();
        state = IDEL;
      }
      break;
    }
    case CONNECT_WIFI: {
      if (state != lastState) {
        lastState = state;
        Serial.println("Connect wifi");
        if (wifiRequest()) {
          digitalWrite(SENSOR_EN_PIN, LOW);
          Serial.println(customVar.link_id);
          Serial.println(customVar.clientId);
          /*
          JsonDocument doc;
          doc["message_type"] = "get_device";
          doc["device_id"] = customVar.link_id;
          doc["client_id"] = customVar.clientId;
        
          String jsonString;
          serializeJson(doc, jsonString);
          // Print the JSON string for debugging
          String response = sendToBackend(jsonString);
          Serial.println("Generated JSON:");
          Serial.println(jsonString);
          JsonDocument inDoc;
          DeserializationError error = deserializeJson(inDoc, response);

          if (error) {
            Serial.println("JSON parse failed");
          } else {
            Serial.print("Temp (ideal moisture): ");
            Serial.println(inDoc["device"]["temp"].as<int>());
          }
          */

          //  updateDevice();
          state = IDEL;
        } else {
          state = CONFIG_WIFI;
        }
      }
      break;
    }
    case IDEL:
      //  Do stuff hear like load backend settings, test soil moisture level, battery level
      //  Then update the backend with the latest infomation then goto sleep
      if (state != lastState) {
        lastState = state;
        Serial.print("Battery = ");
        Serial.print(batteryLevel());
        Serial.println(" %");

        Serial.print("Soil moisture: ");
        Serial.print(sensorValue());
        Serial.println(" %");
        sendRefreshMsg();
        //  updateDevice();
        delay(1000);
        goToSleep();
      }
      break;
  }
  delay(100);
}

float sensorValue() {
  digitalWrite(SENSOR_EN_PIN, LOW);
  delay(1000);
  int raw = readADC_Averaged(SENSOR_LEVEL_PIN, 64);
  Serial.print("Raw sensor = ");
  Serial.println(raw);

  //const float DRY = 4095.0f;
  //const float WET = 1678.0f;

  // Keep the reading inside the calibrated range.
  //float constRaw = constrain(static_cast<float>(raw), WET, DRY);

  // 0% = DRY, 100% = WET.
  //float moisture = 0.0f;
  //if (DRY > WET) {
   // moisture = 100.0f * (DRY - constRaw) / (DRY - WET);
  ////}

  // Clamp to the valid percentage range.
  //moisture = constrain(moisture, 0.0f, 100.0f);

  return 4095 - raw;
}

/*
int batteryLevel(){

  int raw = readADC_Averaged(BATTERY_LEVEL_PIN, 64);

  float v_adc = (raw / 4095.0) * 3.3;   // ADC voltage
  float v_batt = v_adc * 1.806f;          // your measured divider ratio

  // constrain to valid range
  v_batt = constrain(v_batt, 3.3, 4.2);

  // percentage using floats
  return  (v_batt - 3.3) * (100.0 / (4.2 - 3.3));

  //Serial.print("Vadc = ");
  //Serial.println(v_adc, 3);

  //Serial.print("Vbatt = ");
  //Serial.println(v_batt, 3);
}
*/

int batteryLevel() {
  const int samples = 64;
  uint32_t totalMv = 0;

  for (int i = 0; i < samples; ++i) {
    totalMv += analogReadMilliVolts(BATTERY_LEVEL_PIN);
    delayMicroseconds(200);
  }

  const float v_adc = totalMv / (samples * 1000.0f);
  const float v_batt = v_adc * 1.792f;

  // Print BEFORE limiting, so measurement errors stay visible.
  Serial.printf("Battery ADC: %.3f V, Battery: %.3f V\n",
                v_adc, v_batt);

  // Temporary approximate percentage.
  float percentage =
      (v_batt - 3.3f) * 100.0f / (4.2f - 3.3f);

  percentage = constrain(percentage, 0.0f, 100.0f);
  return static_cast<int>(percentage + 0.5f);
}

int batteryPercentFromVoltage(float voltage) {
  // Approximate starting curve, not measured for your battery.
  static const float volts[] = {
    3.30f, 3.60f, 3.70f, 3.75f, 3.80f, 3.85f,
    3.90f, 3.95f, 4.00f, 4.10f, 4.20f
  };

  static const int percent[] = {
    0, 5, 10, 20, 35, 50,
    60, 70, 80, 90, 100
  };

  const int count = sizeof(volts) / sizeof(volts[0]);

  if (voltage <= volts[0]) return 0;
  if (voltage >= volts[count - 1]) return 100;

  for (int i = 1; i < count; ++i) {
    if (voltage <= volts[i]) {
      float fraction =
          (voltage - volts[i - 1]) /
          (volts[i] - volts[i - 1]);

      float result = percent[i - 1] +
          fraction * (percent[i] - percent[i - 1]);

      return static_cast<int>(result + 0.5f);
    }
  }

  return 100;
}

int readADC_Averaged(int pin, int samples) {
  uint32_t total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delayMicroseconds(200);  // small settling delay
  }
  return total / samples;
}

bool wifiRequest() {
  WiFi.disconnect(true);     // full NVS + WiFi reset
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);

  WiFi.begin(customVar.ssid, customVar.password);
  uint32_t startAttemptTime = millis();
  Serial.println("Requesting WiFi connection...");
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    wifiSetRTC();
    getTimeWithRetry();
    return true;
  }
  return false;
}

void wifiSetRTC() {
  configTime(0, 0, "time.google.com", "time.cloudflare.com");
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
  tzset();
}

bool getTimeWithRetry() {
  const int maxRetries = 20;     // 20 × 250ms = 5 seconds
  const int retryDelay = 250;

  for (int i = 0; i < maxRetries; i++) {
    if (getLocalTime(&timeinfo)) {
      return true;           // success
    }
    delay(retryDelay);
  }

  return false;                  // failed after retries
}

bool updateDevice() {
  JsonDocument doc;
  const char* online = "online";
  doc["message_type"] = "insert_update_device";
  doc["device_id"] = customVar.link_id;
  doc["mqtt"] = true;

  JsonObject jsonConfig = doc["json_config"].to<JsonObject>();
  jsonConfig["device_type"] = "SM";
  jsonConfig["updated"] = time(nullptr);
  jsonConfig["battery_level"] = batteryLevel();
  jsonConfig["value"] = sensorValue();

  String jsonString;
  serializeJson(doc, jsonString);
  // Print the JSON string for debugging
  Serial.println("Generated JSON:");
  Serial.println(jsonString);

  // Send the JSON to your backend (use your preferred method: HTTP POST, MQTT, etc.)
  sendToBackend(jsonString);
  return true;
}

String sendToBackend(const String &data) {

HTTPClient http;

  http.begin("http://apps4leisure.com/smart_home/smart_home_endpoint.php");  // URL passed as an argument
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST(data);
  String responseData = "";
  // If the request was successful, read the response
  if (httpResponseCode > 0) {
    responseData = http.getString();  // Get the response body
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    Serial.println("Response: " + responseData);  // Print the response data
  } else {
    Serial.printf("Error on sending %s: %s\n",  http.errorToString(httpResponseCode).c_str());
  }

  http.end();

  return responseData;

}
 
bool sendRefreshMsg() {
  JsonDocument doc;
  const char* online = "online";
  doc["message_type"] = "send_mqtt_message";
  doc["device_id"] = uid;
  doc["client_id"] = customVar.clientId;
  doc["mqtt"] = true;

  JsonObject jsonConfig = doc["json_config"].to<JsonObject>();
  jsonConfig["device_type"] = "SM";
  jsonConfig["updated"] = time(nullptr);
  jsonConfig["battery_level"] = batteryLevel();
  jsonConfig["value"] = sensorValue();

  String jsonString;
  serializeJson(doc, jsonString);
  // Print the JSON string for debugging
  Serial.println("Generated JSON:");
  Serial.println(jsonString);

  // Send the JSON to your backend (use your preferred method: HTTP POST, MQTT, etc.)
  sendToBackend(jsonString);
  return true;
}

void longPressStop1() {
  customVar.callWifiManager = true;
  EEPROM.put(0 , customVar);
  EEPROM.commit();

  WiFi.disconnect();
  while (WiFi.status() == WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  delay(1000);
  ESP.restart();
  delay(1000);
}

void tick() {
  static int lastLedMode;
  if (cur_led_mode() == flash_mode::ON) Blinker(0, 0, 0, 0, LED_PIN, ledOn);
  if (cur_led_mode() == flash_mode::BLINK) Blinker(100, 3000, 0, 0, LED_PIN, !ledOn);
  if (cur_led_mode() == flash_mode::WIFI) Blinker(200, 400, 3, 1000, LED_PIN, !ledOn);
  if (cur_led_mode() == flash_mode::REGISTER) Blinker(200, 400, 4, 1000, LED_PIN, !ledOn);
}

void setBLESmartConfig() {
  String deviceName = "a4l-";
  deviceName += deviceInfo.type_short;
  bleConfig.begin(deviceName.c_str());
  bleConfig.setServiceUUID("12345678-1234-5678-1234-56789abcdef0");
  bleConfig.setCharacteristicUUID("abcdef01-1234-5678-1234-56789abcdef1");
  bleConfig.setJsonCallback(handleJson);
}

void btnPress() {
  Serial.println("Button click");
}

void btnReset() {
  Serial.println("Button long press - resetting WiFi config");
}

/*
void messageReceived(char *topic, uint8_t *payload, unsigned int length){
  Serial.println("Message");
  // Parse JSON payload
  JsonDocument doc; // Set JSON document size according to your payload size
  DeserializationError error = deserializeJson(doc, payload);

  // Check for parsing errors
  if (error) {
    Serial.print("Parsing failed: ");
    Serial.println(error.c_str());
    return;
  }
  String endpoint = doc["device_id"].as<String>();
  char lastChar = endpoint.charAt(endpoint.length() - 1);
  Serial.println(lastChar);
  // Convert the character to an integer ('1' for 'a', '2' for 'b', etc.)
  int index = lastChar - 'A';

  // Extract state value
  String state = doc["state"];
  Serial.println(state);

  Serial.println("Full JSON payload:");
  String jsonConfig = doc["json_config"];
  Serial.println(jsonConfig);
  if (state) {
    bool st = state == "ON" ? true:false;
    //bool newState = (payload == "ON"); // Assuming payload is "ON" or "OFF"
    //controlValve(index, st);
    //device_changed(st, number);
  } else {
    Serial.println("State field not found in payload.");
  }
}
*/

/*
bool registerDevice(){

  if(WiFi.status() != WL_CONNECTED) return false;
return false;
// Create a JSON document
  JsonDocument doc;

  // Add root level properties
  doc["message_type"] = "insert_update_device";
  doc["device_type"] = deviceInfo.type;
  doc["position"] = deviceInfo.position;
  //doc[""] = customVar.friendlyName;
  doc["type_short"] = deviceInfo.type_short;
  doc["device_id"] = uid;
  doc["client_id"] = customVar.clientId;
  doc["version"] = deviceInfo.version;
  doc["ip_address"] = WiFi.localIP().toString(); // Get IP address as String
  doc["device_processor"] = deviceInfo.processor;

  // Serialize the JSON document to a String
  String jsonString;
  serializeJson(doc, jsonString);
 
  Serial.println("Generated JSON:");
  Serial.println(jsonString);
  // Send the JSON to your backend (use your preferred method: HTTP POST, MQTT, etc.)
  String response = sendToBackend(jsonString);
  // Parse the JSON response
  DeserializationError error = deserializeJson(doc, response);

  // Check for parsing errors
  if (error) {
      Serial.print("JSON Parsing failed: ");
      Serial.println(error.c_str());
      return false;
  }

  // Extract values
  bool success = doc["success"];  // Read success flag
  const char* action = doc["action"];  // Read action value

  // Output the results
  Serial.print("Success: ");
  Serial.println(success ? "Yes" : "No");
  
  Serial.print("Action: ");
  Serial.println(action);
  return true;
}
*/