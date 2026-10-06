#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30100_PulseOximeter.h"
#include <WiFi.h>
#include <FirebaseESP32.h>

#define WIFI_SSID "A06"
#define WIFI_PASSWORD "12345678"
#define FIREBASE_HOST "" 
#define FIREBASE_AUTH "AIzaSyBd4zHR4FIAQiip0DKHskPjVsrV49RQYcs" 

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

#define ONE_WIRE_BUS 4
#define REPORTING_PERIOD_MS 1000
#define FILTER_SIZE 10
#define SAMPLE_INTERVAL 800
#define MEASURE_WINDOW 100000
#define CONVERSION_DELAY_MS 800

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
PulseOximeter pox;

// --- DS18B20 state ---
unsigned long startTime = 0;
unsigned long lastRequestTime = 0;
bool conversionRequested = false;
float currentTemp = 0.0;

// --- MAX30100 state ---
float bpmBuffer[FILTER_SIZE] = {0};
uint8_t bufferIndex = 0;
uint32_t tsLastReport = 0;

// --- Display state ---
enum DisplayMode { SHOW_TEMP, SHOW_SPO2, SHOW_FINAL };
DisplayMode displayMode = SHOW_TEMP;
unsigned long finalDisplayStart = 0;

// --- Shared data between cores (volatile for safety) ---
volatile float sharedTemp = 0.0;
volatile float sharedBPM  = 0.0;
volatile float sharedSpo2 = 0.0;
volatile bool  firebasePending = false;

// --- FreeRTOS task handle ---
TaskHandle_t firebaseTaskHandle = NULL;

// ============================================================
// CORE 0 — Firebase Task (runs separately, never blocks Core 1)
// ============================================================
void firebaseTask(void * parameter) {
  for (;;) {
    if (firebasePending) {
     Firebase.setFloat(fbdo, "/temperature", (float)sharedTemp);
    Firebase.setFloat(fbdo, "/bpm",         (float)sharedBPM);
    Firebase.setFloat(fbdo, "/spo2",        (float)sharedSpo2);
    Serial.println("✅ Firebase sent!");
      firebasePending = false;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS); // yield to other tasks
  }
}

void onBeatDetected() {
  Serial.println("Beat detected!");
}

float getFilteredBPM(float newValue) {
  bpmBuffer[bufferIndex] = newValue;
  bufferIndex = (bufferIndex + 1) % FILTER_SIZE;

  float sum = 0;
  int count = 0;
  for (int i = 0; i < FILTER_SIZE; i++) {
    if (bpmBuffer[i] > 40 && bpmBuffer[i] < 180) {
      sum += bpmBuffer[i];
      count++;
    }
  }
  return (count == 0) ? 0 : sum / count;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  // --- WiFi ---
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // --- Firebase ---
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // --- OLED ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 not detected");
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Initializing...");
  display.display();
  delay(1000);
  display.clearDisplay();
  display.display();

  // --- DS18B20 ---
  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);

  // --- MAX30100 ---
  if (!pox.begin()) {
    Serial.println("MAX30100 not detected");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("MAX30100 Error!");
    display.display();
    while (1);
  }
  pox.setIRLedCurrent(MAX30100_LED_CURR_11MA);
  pox.setOnBeatDetectedCallback(onBeatDetected);

  // --- Start Firebase task on Core 0 ---
  xTaskCreatePinnedToCore(
    firebaseTask,        // function
    "FirebaseTask",      // name
    8192,                // stack size
    NULL,                // parameter
    1,                   // priority
    &firebaseTaskHandle, // handle
    0                    // Core 0
  );

  startTime = millis();
  lastRequestTime = millis();
  sensors.requestTemperatures();
  conversionRequested = true;
}

// ============================================================
// CORE 1 — Main Loop (pox, sensors, display — never blocked)
// ============================================================
void loop() {
  unsigned long now = millis();

  // Always first — MAX30100 needs this constantly
  pox.update();

  // --- Temperature ---
  if (conversionRequested && (now - lastRequestTime >= CONVERSION_DELAY_MS)) {
    currentTemp = sensors.getTempCByIndex(0);
    conversionRequested = false;

    Serial.print("Body Temp : ");
    if (currentTemp == DEVICE_DISCONNECTED_C || currentTemp < -50.0) {
      Serial.println("SENSOR ERROR");
    } else {
      Serial.print(currentTemp, 2);
      Serial.println(" C");
    }
  }

  if (!conversionRequested && (now - lastRequestTime >= SAMPLE_INTERVAL)) {
    sensors.requestTemperatures();
    lastRequestTime = now;
    conversionRequested = true;
  }

  // --- Final display ---
  if (displayMode == SHOW_FINAL) {
    if (now - finalDisplayStart >= 5000) {
      startTime = millis();
      displayMode = SHOW_TEMP;
      display.clearDisplay();
      display.display();
    }
    return;
  }

  if (now - startTime >= MEASURE_WINDOW) {
    displayMode = SHOW_FINAL;
    finalDisplayStart = now;
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0); display.print("Body Temp:");
    display.setTextSize(2);
    display.setCursor(0, 16); display.print(currentTemp, 2); display.print(" C");
    display.display();
    Serial.print("Body Temp = "); Serial.print(currentTemp, 2);
    Serial.println(" C");
    return;
  }

  // --- MAX30100 reporting every 1 second ---
  if (now - tsLastReport > REPORTING_PERIOD_MS) {
    float rawBPM = pox.getHeartRate();
    float filteredBPM = getFilteredBPM(rawBPM);
    float spo2 = pox.getSpO2();
    tsLastReport = now;

    Serial.println("======================");
    Serial.print("Body Temp : ");
    Serial.print(currentTemp, 2);
    Serial.println(" C");

    if (filteredBPM < 40 || spo2 < 80) {
      displayMode = SHOW_TEMP;
      Serial.println("BPM       : --");
      Serial.println("SpO2      : --");
      Serial.println("Place finger properly");
    } else {
      displayMode = SHOW_SPO2;
      Serial.print("BPM       : "); Serial.println(filteredBPM);
      Serial.print("SpO2      : "); Serial.print(spo2); Serial.println(" %");
    }
    Serial.println("======================");

    // Update shared data every 1 second but only TRIGGER Firebase every 10 seconds
    sharedTemp = currentTemp;
    sharedBPM  = filteredBPM;
    sharedSpo2 = spo2;
  }

  // --- OLED rendering ---
  display.clearDisplay();
  display.setTextSize(1);

  if (displayMode == SHOW_TEMP) {
    display.setCursor(0, 0);
    display.print("Body Temp:");
    display.setTextSize(2);
    display.setCursor(0, 16);
    display.print(currentTemp, 2);
    display.print(" C");
  } else if (displayMode == SHOW_SPO2) {
    float filteredBPM = getFilteredBPM(0);
    float spo2 = pox.getSpO2();
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print("BPM:  "); display.print(filteredBPM, 0);
    display.setTextSize(2);
    display.setCursor(0, 16);
    display.print("SpO2: "); display.print(spo2, 0); display.print("%");
  }

  display.display();

  // --- Trigger Firebase every 10 seconds ---
  static unsigned long lastFirebaseSend = 0;
  if (now - lastFirebaseSend >= 10000 && !firebasePending) {
    lastFirebaseSend = now;
    firebasePending = true; // Core 0 will pick this up
  }
}
