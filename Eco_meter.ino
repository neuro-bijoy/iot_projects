#include <Wire.h>
#include <DHT.h>
#include <Preferences.h>
#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ================= PIN DEFINITIONS =================
#define DHTPIN           4       // GPIO 4 - DHT11
#define DHTTYPE          DHT11
#define LED_PIN          2       // GPIO 2 - Status LED
#define MQ135_PIN        34      // GPIO 34 - MQ-135 analog

// ================= MQ-135 CONSTANTS =================
#define RL               10000.0
#define CLEAN_AIR_RATIO  3.6
#define ADC_SAMPLES      10
#define WARMUP_SECONDS   180

// ================= DHT11 =================
DHT dht(DHTPIN, DHTTYPE);

// ================= WiFi =================
const char* ssid     = "name of wifi source";
const char* password = "password";

// ================= Telegram =================
#define BOTtoken  ""
#define CHAT_ID   ""

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// ================= Preferences (NVS) =================
Preferences prefs;
float R0 = 10.0;
const float UNSET_TEMP = -100.0;

// ================= Timing =================
const unsigned long BOT_CHECK_INTERVAL = 1000;
const unsigned long REPORT_INTERVAL    = 60000;
const unsigned long SAMPLE_INTERVAL    = 2000;

unsigned long lastBotCheck = 0;
unsigned long lastReport   = 0;
unsigned long lastSample   = 0;

// ================= State =================
bool  isRunning   = false;
float currentTemp = 0.0;
float currentHum  = 0.0;
float maxTemp     = UNSET_TEMP;
float currentRs   = 0.0;
float currentRatio = 0.0;
int   currentAQI  = 0;
const char* currentQuality = "N/A";

// ================= LED Helper =================
void setLED(bool state) {
  digitalWrite(LED_PIN, state ? HIGH : LOW);
}

// ================= MQ-135 Functions =================
float readRs() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(MQ135_PIN);
    delay(5);
  }
  float vout = (sum / (float)ADC_SAMPLES) * (3.3f / 4095.0f);
  if (vout <= 0.01f) vout = 0.01f;
  return ((3.3f - vout) / vout) * RL;
}

float calibrateR0(int samples = 50) {
  Serial.println("Calibrating R0 in clean air...");
  float rsSum = 0;
  for (int i = 0; i < samples; i++) {
    rsSum += readRs();
    delay(100);
    if (i % 10 == 0) Serial.printf("  Sample %d/%d\n", i, samples);
  }
  float r0 = (rsSum / samples) / CLEAN_AIR_RATIO;
  Serial.printf("Calibration complete. R0 = %.2f Ohm\n", r0);
  return r0;
}

const char* getAirQuality(float ratio) {
  if (ratio >= 2.5f) return "GOOD";
  if (ratio >= 1.5f) return "MODERATE";
  if (ratio >= 0.9f) return "POOR";
  return "VERY POOR";
}

int getEstimatedAQI(float ratio) {
  const float ratios[] = { 0.0f, 0.9f, 1.5f, 2.5f, 4.0f };
  const int   aqis[]   = {  400,  200,  100,   50,   25  };
  const int   count    = 5;

  if (ratio <= ratios[0]) return aqis[0];
  if (ratio >= ratios[count - 1]) return aqis[count - 1];

  for (int i = 1; i < count; i++) {
    if (ratio <= ratios[i]) {
      float t = (ratio - ratios[i - 1]) / (ratios[i] - ratios[i - 1]);
      return (int)(aqis[i - 1] + t * (aqis[i] - aqis[i - 1]));
    }
  }
  return 50;
}

// ================= AQI Emoji Helper =================
String getAQIEmoji(const char* quality) {
  if (strcmp(quality, "GOOD") == 0)      return "🟢";
  if (strcmp(quality, "MODERATE") == 0)  return "🟡";
  if (strcmp(quality, "POOR") == 0)      return "🟠";
  return "🔴";
}

// ================= Send Periodic Report =================
void sendReport() {
  if (currentTemp == 0.0 && currentHum == 0.0) return;

  String emoji = getAQIEmoji(currentQuality);

  String message = "📊 *1-Minute Reading*\n\n";
  message += "🌡 *Temperature:* " + String(currentTemp, 1) + " °C\n";
  message += "💧 *Humidity:* " + String(currentHum, 0) + " %\n";
  message += "📈 *Max Temp (session):* " + String(maxTemp > UNSET_TEMP ? maxTemp : currentTemp, 1) + " °C\n\n";
  message += "💨 *Air Quality:* " + emoji + " " + String(currentQuality) + "\n";
  message += "📉 *Rs/R0 Ratio:* " + String(currentRatio, 2) + "\n";
  message += "🏭 *Est. AQI:* " + String(currentAQI);

  bot.sendMessage(CHAT_ID, message, "Markdown");

  Serial.printf("[REPORT] Temp: %.1fC | Hum: %.0f%% | Max: %.1fC | Quality: %s | AQI: %d\n",
    currentTemp, currentHum,
    maxTemp > UNSET_TEMP ? maxTemp : currentTemp,
    currentQuality, currentAQI);
}

// ================= Telegram Message Handler =================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text    = bot.messages[i].text;

    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "⛔ Unauthorized user.", "");
      continue;
    }

    // ---------- /start ----------
    if (text == "/start") {
      if (isRunning) {
        bot.sendMessage(CHAT_ID, "✅ Already running! Readings every 1 min.", "Markdown");
      } else {
        isRunning  = true;
        maxTemp    = UNSET_TEMP;
        lastReport = millis();
        lastSample = millis();
        setLED(true);
        bot.sendMessage(CHAT_ID,
          "▶️ *ESP32 Started!*\nReadings will be sent every *1 minute*.\nIncludes temperature, humidity & air quality.\nSend /stop to sleep.",
          "Markdown");
        Serial.println("[INFO] Started via Telegram");
      }
    }

    // ---------- /stop ----------
    else if (text == "/stop") {
      if (!isRunning) {
        bot.sendMessage(CHAT_ID, "💤 Already in sleep mode.", "Markdown");
      } else {
        isRunning = false;
        setLED(false);
        bot.sendMessage(CHAT_ID,
          "⏹️ *ESP32 Stopped!*\nEntering sleep mode 💤\nSend /start to wake up.",
          "Markdown");
        Serial.println("[INFO] Stopped via Telegram — entering sleep mode");
      }
    }

    // ---------- /status ----------
    else if (text == "/status") {
      String statusMsg = isRunning
        ? "🟢 *Status: Running*\n\n"
        : "🔴 *Status: Sleep Mode*\n";

      if (isRunning && currentTemp != 0.0) {
        String emoji = getAQIEmoji(currentQuality);
        statusMsg += "🌡 *Temp:* " + String(currentTemp, 1) + " °C\n";
        statusMsg += "💧 *Humidity:* " + String(currentHum, 0) + " %\n";
        statusMsg += "📈 *Max Temp:* " + String(maxTemp > UNSET_TEMP ? maxTemp : currentTemp, 1) + " °C\n\n";
        statusMsg += "💨 *Air Quality:* " + emoji + " " + String(currentQuality) + "\n";
        statusMsg += "📉 *Rs/R0 Ratio:* " + String(currentRatio, 2) + "\n";
        statusMsg += "🏭 *Est. AQI:* " + String(currentAQI);
      } else if (!isRunning) {
        statusMsg += "Send /start to begin readings.";
      }
      bot.sendMessage(CHAT_ID, statusMsg, "Markdown");
    }

    // ---------- /cal ----------
    else if (text == "/cal") {
      bot.sendMessage(CHAT_ID,
        "🔧 *MQ-135 Recalibration Started*\nEnsure sensor is in *clean outdoor air*.\nThis takes ~10 seconds...",
        "Markdown");
      R0 = calibrateR0();
      prefs.putFloat("r0", R0);
      bot.sendMessage(CHAT_ID,
        "✅ *Calibration Complete!*\nNew R0 = " + String(R0, 2) + " Ω\nSaved to memory.",
        "Markdown");
    }

    // ---------- /help ----------
    else if (text == "/help") {
      String helpMsg =
        "🤖 *ESP32 DHT11 + MQ-135 Bot*\n\n"
        "/start  — Wake up & start sending readings\n"
        "/stop   — Stop & enter sleep mode\n"
        "/status — Get current readings instantly\n"
        "/cal    — Recalibrate MQ-135 air sensor\n"
        "/help   — Show this menu";
      bot.sendMessage(CHAT_ID, helpMsg, "Markdown");
    }

    // ---------- Unknown ----------
    else {
      bot.sendMessage(CHAT_ID,
        "❓ Unknown command. Send /help for available commands.", "");
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  setLED(false);

  // ADC setup for MQ-135
  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);

  dht.begin();

  // --- MQ-135 Warm-Up ---
  Serial.println("\nMQ-135 + DHT11 Monitor");
  Serial.println("Warming up MQ-135 sensor...");
  for (int i = WARMUP_SECONDS; i > 0; i--) {
    Serial.printf("\r  %3d seconds remaining...", i);
    delay(1000);
  }
  Serial.println("\nWarm-up complete.");

  // --- Load or Calibrate R0 ---
  prefs.begin("mq135", false);
  if (prefs.isKey("r0")) {
    R0 = prefs.getFloat("r0", 10.0f);
    Serial.printf("Loaded R0 from storage: %.2f Ohm\n", R0);
    Serial.println("  (Send /cal via Telegram to recalibrate)");
  } else {
    Serial.println("No stored R0. Running initial calibration.");
    Serial.println("  >> Ensure sensor is in CLEAN OUTDOOR AIR <<");
    delay(3000);
    R0 = calibrateR0();
    prefs.putFloat("r0", R0);
    Serial.println("R0 saved to storage.");
  }

  // --- Connect WiFi ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setInsecure();

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());

  isRunning = false;

  bot.sendMessage(CHAT_ID,
    "🔌 *ESP32 Online!*\n🌡 DHT11 + 💨 MQ-135 ready.\nSend /start to begin readings.\nSend /help for all commands.",
    "Markdown");

  Serial.println("[INFO] Boot complete — waiting for /start");
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // ---- Always check Telegram ----
  if (now - lastBotCheck >= BOT_CHECK_INTERVAL) {
    lastBotCheck = now;
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    if (numNewMessages > 0) {
      handleNewMessages(numNewMessages);
    }
  }

  // ---- Only run when active ----
  if (isRunning) {

    // Sample both sensors
    if (now - lastSample >= SAMPLE_INTERVAL) {
      lastSample = now;

      // DHT11
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      if (!isnan(t) && !isnan(h)) {
        currentTemp = t;
        currentHum  = h;
        if (currentTemp > maxTemp) maxTemp = currentTemp;
      } else {
        Serial.println("[WARN] DHT read failed");
      }

      // MQ-135
      currentRs    = readRs();
      currentRatio = currentRs / R0;
      currentAQI   = getEstimatedAQI(currentRatio);
      currentQuality = getAirQuality(currentRatio);

      Serial.printf("[SAMPLE] Temp: %.1fC | Hum: %.0f%% | Rs/R0: %.2f | Quality: %s | AQI: %d\n",
        currentTemp, currentHum, currentRatio, currentQuality, currentAQI);
    }

    // Send 1-minute report
    if (now - lastReport >= REPORT_INTERVAL) {
      lastReport = now;
      sendReport();
    }
  }
}