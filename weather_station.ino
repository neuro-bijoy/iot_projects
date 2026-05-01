#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <DHT.h>
#define WIFI_LED    2 

//  Pin Definitions
#define DHTPIN      13
#define DHTTYPE     DHT11
#define LDR_PIN     35
#define RAIN_PIN    32
#define MQ135_PIN   34

// MQ135 Constants
#define RL          10.0
#define VCC         3.3

float R0 = 0;

// WiFi & Firebase
#define WIFI_SSID     "A06"
#define WIFI_PASSWORD "12345678"
#define API_KEY       "AIzaSyBd4zHR4FIAQiip0DKHskPjVsrV49RQYcs"
#define DATABASE_URL  "https://getsms-6308e-default-rtdb.firebaseio.com"

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

//  LDR Functions-----
String getLightLabel(int ldrValue) {
  if (ldrValue > 3050) return "Bright Sunlight";
  if (ldrValue > 2600) return "Moderate Sunlight";
  if (ldrValue > 250)  return "Low Sunlight";
  return "Night";
}

String getLightEmoji(int ldrValue) {
  if (ldrValue > 3050) return " ☀️";
  if (ldrValue > 2600) return " 🌤️";
  if (ldrValue > 250)  return " 🌥️";
  return " 🌙";
}


//  Rain Sensor Functions-----

String getRainLabel(int rainValue) {
  if (rainValue > 3800) return "No Rain";
  if (rainValue > 2500) return "Light Rain";
  if (rainValue > 1500) return "Moderate Rain";
  return "Heavy Rain";
}

String getRainEmoji(int rainValue) {
  if (rainValue > 3800) return " ☀️";
  if (rainValue > 2500) return " 🌦️";
  if (rainValue > 1500) return " 🌧️";
  return " ⛈️";
}


//  MQ135 Functions-----

float getMQ135Ratio() {
  int adc = analogRead(MQ135_PIN);
  float voltage = adc * (VCC / 4095.0);
  if (voltage <= 0) voltage = 0.001; // avoid division by zero
  float Rs = ((VCC - voltage) / voltage) * RL;
  return Rs / R0;
}

String getAirLabel(float ratio) {
  if (ratio < 1.0) return "Clean Air";
  if (ratio < 2.0) return "Moderate Air";
  return "Polluted Air";
}

String getAirEmoji(float ratio) {
  if (ratio < 1.0) return " 😊";
  if (ratio < 2.0) return " 😐";
  return " 😷";
}


//  MQ135 Calibration-----
void calibrateMQ135() {
  Serial.println("🔥 Warming up MQ135 sensor (60s)...");
  delay(30000);

  Serial.println("📐 Calibrating MQ135 in clean air...");
  float sum = 0;
  for (int i = 0; i < 50; i++) {
    int adc = analogRead(MQ135_PIN);
    float voltage = adc * (VCC / 4095.0);
    if (voltage <= 0) voltage = 0.001;
    float Rs = ((VCC - voltage) / voltage) * RL;
    sum += Rs;
    delay(100);
  }

  R0 = sum / 50.0;
  Serial.print("✅ Calibration Done! R0 = ");
  Serial.println(R0);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  // MQ135 Calibration-----
  calibrateMQ135();

  // WiFi----
  pinMode(WIFI_LED, OUTPUT);
  digitalWrite(WIFI_LED, LOW); 
  Serial.print("📶 Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  digitalWrite(WIFI_LED, HIGH);  
  Serial.println("\n✅ WiFi Connected!");
  Serial.println();

  // Firebase-----
  config.api_key        = API_KEY;
  config.database_url   = DATABASE_URL;
  Firebase.signUp(&config, &auth, "", "");
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}


//  LOOP----

void loop() {

  // ── Read Sensors-----
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  int    ldrValue   = analogRead(LDR_PIN);
  String lightLabel = getLightLabel(ldrValue);
  String lightEmoji = getLightEmoji(ldrValue);

  int    rainValue = analogRead(RAIN_PIN);
  String rainLabel = getRainLabel(rainValue);
  String rainEmoji = getRainEmoji(rainValue);

  float  aqiRatio  = getMQ135Ratio();
  String airLabel  = getAirLabel(aqiRatio);
  String airEmoji  = getAirEmoji(aqiRatio);

  // ── DHT Sanity Check ────────────────────────────────────
  if (isnan(temp) || isnan(hum)) {
    Serial.println("❌ DHT11 Read Failed! Skipping...");
    delay(5000);
    return;
  }

  // ── Serial Monitor ──────────────────────────────────────
  Serial.println("╔══════════════════════════════╗");
  Serial.println("      WEATHER STATION v3.0      ");
  Serial.println("╚══════════════════════════════╝");

  Serial.println("🌡️  Temperature");
  Serial.print("    → "); Serial.print(temp); Serial.println(" °C");

  Serial.println("💧 Humidity");
  Serial.print("    → "); Serial.print(hum); Serial.println(" %");

  Serial.println("💡 Light Level");
  Serial.print("    → ADC: "); Serial.println(ldrValue);
  Serial.print("    → "); Serial.print(lightLabel); Serial.println(lightEmoji);

  Serial.println("🌧️  Rainfall");
  Serial.print("    → ADC: "); Serial.println(rainValue);
  Serial.print("    → "); Serial.print(rainLabel); Serial.println(rainEmoji);

  Serial.println("🌫️  Air Quality (MQ135)");
  Serial.print("    → Rs/R0 Ratio: "); Serial.println(aqiRatio, 3);
  Serial.print("    → "); Serial.print(airLabel); Serial.println(airEmoji);

  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println();

  // ── Firebase Upload ─────────────────────────────────────
  if (Firebase.ready()) {
    Firebase.RTDB.setFloat(&fbdo,  "/sen_data/",    temp);
    Firebase.RTDB.setFloat(&fbdo,  "/sens_data/",   hum);
    Firebase.RTDB.setString(&fbdo, "/light_label/", lightLabel);
    Firebase.RTDB.setString(&fbdo, "/rain_label/",  rainLabel);
    Firebase.RTDB.setString(&fbdo, "/air_label/",   airLabel);

    Serial.println("📡 Firebase Updated!");
    Serial.println();
  }

  delay(5000);
}