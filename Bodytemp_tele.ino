#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ================= DS18B20 =================
#define ONE_WIRE_BUS 5
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= WiFi =================
const char* ssid = "wifi device/ hostspot provider name";
const char* password = "passwoed";

// ================= Telegram =================
#define BOTtoken "bot token"
#define CHAT_ID "bot id"

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// ================= Timing =================
unsigned long startTime = 0;
unsigned long lastSampleTime = 0;

const unsigned long MEASURE_WINDOW = 100000;   // same as Code B
const unsigned long SAMPLE_INTERVAL = 800;     // same as Code B

float currentTemp = 0.0;
float maxTemp = -100.0;
bool telegramSent = false;

// ================= SETUP =================
void setup() {
  Serial.begin(9600);

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(true);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("DS18B20 Ready");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(2000);
  lcd.clear();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setInsecure();   // for simplicity

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting WiFi...");
  }

  Serial.println("WiFi Connected");
  startTime = millis();
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // ---- Sampling (UNCHANGED) ----
  if (now - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = now;

    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0);

    if (currentTemp != DEVICE_DISCONNECTED_C) {
      if (currentTemp > maxTemp) {
        maxTemp = currentTemp;
      }
    }

    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(currentTemp, 2);
    lcd.print(" C   ");

    lcd.setCursor(0, 1);
    lcd.print("Max:  ");
    lcd.print(maxTemp, 2);
    lcd.print(" C   ");

    Serial.print("Temp: ");
    Serial.print(currentTemp, 2);
    Serial.print("  Max: ");
    Serial.println(maxTemp, 2);
  }

  // ---- Final result window (UNCHANGED + Telegram ADD) ----
  if (now - startTime >= MEASURE_WINDOW) {

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Highest Temp:");
    lcd.setCursor(0, 1);
    lcd.print(maxTemp, 2);
    lcd.print(" C");

    Serial.println("---- FINAL ----");
    Serial.print("Highest Temp = ");
    Serial.print(maxTemp, 2);
    Serial.println(" C");

    // ---- SEND TO TELEGRAM (ONLY ADDITION) ----
    if (!telegramSent) {
      String message = "🌡 *Final Body Temperature*\n";
      message += "Highest Temp: ";
      message += String(maxTemp, 2);
      message += " °C";
      bot.sendMessage(CHAT_ID, message, "Markdown");
      telegramSent = true;
    }

    delay(5000);   // same as Code B

    // ---- Reset (UNCHANGED) ----
    maxTemp = -100.0;
    startTime = millis();
    telegramSent = false;
    lcd.clear();
  }
}
