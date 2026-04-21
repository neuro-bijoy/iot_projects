#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <DHT.h>

#define DHTPIN 13
#define DHTTYPE DHT11
#define MQ7_PIN 34

#define WIFI_SSID     "A06"
#define WIFI_PASSWORD "12345678"
#define API_KEY       "AIzaSyBd4zHR4FIAQiip0DKHskPjVsrV49RQYcs"
#define DATABASE_URL  "https://getsms-6308e-default-rtdb.firebaseio.com"

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

void setup() {
  Serial.begin(115200);
  dht.begin();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi Connected!");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.signUp(&config, &auth, "", "");
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  int gasValue = analogRead(MQ7_PIN);
  float gasPercent = map(gasValue, 0, 4095, 0, 100);

  // ✅ Creative Serial Monitor Print
  Serial.println("╔══════════════════════════╗");
  Serial.println("         WEATHER        ");
  Serial.println("╚══════════════════════════╝");

  Serial.println("🌡️  Temperature");
  Serial.print("    → ");
  Serial.print(temp);
  Serial.println(" °C");

  Serial.println("💧 Humidity");
  Serial.print("    → ");
  Serial.print(hum);
  Serial.println(" %");

  Serial.println("💨 Carbon Emission (CO)");
  Serial.print("    → ");
  Serial.print(gasPercent);
  Serial.println(" %");

  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println();

  if (Firebase.ready()) {
    Firebase.RTDB.setFloat(&fbdo, "/sen_data/", temp);
    Firebase.RTDB.setFloat(&fbdo, "/sens_data/", hum);
    Firebase.RTDB.setFloat(&fbdo, "/senso_data/", gasPercent);
    Serial.println("📡 Firebase Updated!");
    Serial.println();
  }

  delay(30000);
}