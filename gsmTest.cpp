#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "base64.h"

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

// ---------------- CONFIG ---------------- //
#define RFID_SS 5
#define RFID_RST 22
#define SERVO_ENTRANCE_PIN 13
#define SERVO_EXIT_PIN 14
#define TRIG_ENTRANCE 26
#define ECHO_ENTRANCE 25
#define TRIG_EXIT 33
#define ECHO_EXIT 32

#define MODEM_TX 17
#define MODEM_RX 16
#define MODEM_BAUD 9600

// ---- Network ----
const char* WIFI_SSID = "Imaxeuno";
const char* WIFI_PASS = "97password";

// ---- Twilio ----
String TWILIO_SID   = "ACf004842ec41b4aaa4f7fda39b112dec6";
String TWILIO_AUTH  = "730cbd4ff39e337d7c5a8d2873505562";
String TWILIO_FROM  = "+17753678887";
String TWILIO_TO    = "+2349042796521";

// ---- Files ----
const char* authorizedFile = "/authorized.json";

// ---- Globals ----
MFRC522 mfrc522(RFID_SS, RFID_RST);
Servo entranceGateServo;
Servo exitGateServo;

bool entranceGateOpen = false;
bool exitGateOpen = false;
unsigned long entranceGateOpenTime = 0;
unsigned long exitGateOpenTime = 0;
const unsigned long GATE_OPEN_DURATION = 5000; // 5 seconds

// ---- GSM Setup ----
TinyGsm modem(Serial2);

// -------------------------------------------------- //
void sendTwilioSMS(String to, String body);
void sendGsmSMS(String to, String body);
bool isUIDAuthorized(String uid);
float readDistance(int trigPin, int echoPin);
void openEntranceGate();
void closeEntranceGate();
void openExitGate();
void closeExitGate();
void checkEntranceGateTimeout();
void checkExitGateTimeout();
String urlEncode(String str);

// -------------------------------------------------- //
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(1000);
  Serial.println("System starting...");

  // Allow allocation of all timers for ESP32Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  // Initialize SPI for RFID
  SPI.begin();
  mfrc522.PCD_Init();

  // Initialize Entrance Servo
  entranceGateServo.setPeriodHertz(50);
  entranceGateServo.attach(SERVO_ENTRANCE_PIN, 500, 2400);
  entranceGateServo.write(90);

  // Initialize Exit Servo
  exitGateServo.setPeriodHertz(50);
  exitGateServo.attach(SERVO_EXIT_PIN, 500, 2400);
  exitGateServo.write(90);

  // Ultrasonic setup
  pinMode(TRIG_ENTRANCE, OUTPUT);
  pinMode(ECHO_ENTRANCE, INPUT);
  pinMode(TRIG_EXIT, OUTPUT);
  pinMode(ECHO_EXIT, INPUT);

  // SPIFFS Init
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS Mount Failed!");
    return;
  }
  Serial.println("✅ SPIFFS mounted");

  // Check if authorized.json exists
  if (!SPIFFS.exists(authorizedFile)) {
    Serial.println("authorized.json not found! Creating default...");
    File file = SPIFFS.open(authorizedFile, FILE_WRITE);
    if (file) {
      DynamicJsonDocument doc(1024);
      JsonArray uids = doc.createNestedArray("uids");
      uids.add("A1B2C3D4"); // example UID
      uids.add("12345678"); // another example UID
      serializeJson(doc, file);
      file.close();
      Serial.println("✅ Default authorized.json created");
    } else {
      Serial.println("❌ Failed to create authorized.json");
    }
  }

  // WiFi connect
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✅ Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" ⚠️ Failed to connect to WiFi");
  }

  // GSM initialization
  Serial.println("Initializing GSM...");
  Serial2.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  if (!modem.restart()) {
    Serial.println("⚠️ Modem restart failed");
  } else {
    Serial.println("✅ Modem restarted");
  }

  if (!modem.waitForNetwork(15000)) {
    Serial.println("⚠️ GSM network not found");
  } else {
    Serial.println("✅ GSM network ready");
  }

  Serial.println("✅ Setup complete");
}

// -------------------------------------------------- //
void loop() {
  checkEntranceGateTimeout();
  checkExitGateTimeout();

  // RFID check
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uidStr = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
      uidStr += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();
    Serial.println("RFID Detected - UID: " + uidStr);

    if (isUIDAuthorized(uidStr)) {
      Serial.println("✅ Authorized — Opening entrance gate");
      openEntranceGate();
    } else {
      Serial.println("🚫 Unauthorized — Sending alert");
      String message = "Unauthorized vehicle detected. UID: " + uidStr;
      sendTwilioSMS(TWILIO_TO, message);
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1000);
  }

  // Entrance ultrasonic
  float entranceDist = readDistance(TRIG_ENTRANCE, ECHO_ENTRANCE);
  if (entranceDist > 0 && entranceDist < 20) {
    Serial.println("Vehicle detected at entrance: " + String(entranceDist) + " cm");
  }

  // Exit ultrasonic
  float exitDist = readDistance(TRIG_EXIT, ECHO_EXIT);
  if (exitDist > 0 && exitDist < 20) {
    Serial.println("Vehicle detected at exit — Opening exit gate");
    openExitGate();
  }

  delay(100);
}

// -------------------------------------------------- //
bool isUIDAuthorized(String uid) {
  File file = SPIFFS.open(authorizedFile, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open authorized.json");
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("❌ JSON deserialization failed: ");
    Serial.println(error.c_str());
    return false;
  }

  JsonArray uids = doc["uids"].as<JsonArray>();
  for (JsonVariant value : uids) {
    String storedUID = value.as<String>();
    storedUID.toUpperCase();
    if (storedUID == uid) return true;
  }
  return false;
}

// -------------------------------------------------- //
void sendTwilioSMS(String to, String body) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://api.twilio.com/2010-04-01/Accounts/" + TWILIO_SID + "/Messages.json";
    String auth = base64::encode(TWILIO_SID + ":" + TWILIO_AUTH);

    http.begin(url);
    http.addHeader("Authorization", "Basic " + auth);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String payload = "To=" + urlEncode(to) + "&From=" + urlEncode(TWILIO_FROM) + "&Body=" + urlEncode(body);
    Serial.println("📨 Sending Twilio SMS...");
    
    int httpCode = http.POST(payload);
    String response = http.getString();
    http.end();

    if (httpCode == 201) {
      Serial.println("✅ Twilio SMS sent successfully!");
      return;
    } else {
      Serial.printf("⚠️ Twilio failed (HTTP %d): %s\n", httpCode, response.c_str());
    }
  } else {
    Serial.println("⚠️ WiFi not connected — skipping Twilio");
  }

  sendGsmSMS(to, body);
}

// -------------------------------------------------- //
void sendGsmSMS(String to, String body) {
  Serial.println("📡 Sending GSM SMS...");
  if (!modem.isNetworkConnected()) {
    if (!modem.waitForNetwork(10000)) {
      Serial.println("❌ GSM network not available");
      return;
    }
  }

  bool sent = modem.sendSMS(to, body);
  if (sent) Serial.println("✅ GSM SMS sent successfully!");
  else Serial.println("❌ GSM SMS failed!");
}

// -------------------------------------------------- //
// Entrance gate control
void openEntranceGate() {
  if (!entranceGateOpen) {
    entranceGateServo.write(0);
    entranceGateOpen = true;
    entranceGateOpenTime = millis();
    Serial.println("🔓 Entrance gate opened");
  }
}

void closeEntranceGate() {
  if (entranceGateOpen) {
    entranceGateServo.write(90);
    entranceGateOpen = false;
    Serial.println("🔒 Entrance gate closed");
  }
}

void checkEntranceGateTimeout() {
  if (entranceGateOpen && (millis() - entranceGateOpenTime >= GATE_OPEN_DURATION)) {
    closeEntranceGate();
  }
}

// Exit gate control
void openExitGate() {
  if (!exitGateOpen) {
    exitGateServo.write(0);
    exitGateOpen = true;
    exitGateOpenTime = millis();
    Serial.println("🔓 Exit gate opened");
  }
}

void closeExitGate() {
  if (exitGateOpen) {
    exitGateServo.write(90);
    exitGateOpen = false;
    Serial.println("🔒 Exit gate closed");
  }
}

void checkExitGateTimeout() {
  if (exitGateOpen && (millis() - exitGateOpenTime >= GATE_OPEN_DURATION)) {
    closeExitGate();
  }
}

// -------------------------------------------------- //
float readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}

// -------------------------------------------------- //
String urlEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') encodedString += '+';
    else if (isalnum(c)) encodedString += c;
    else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}
