#include <SPI.h>
#include <MFRC522.h>
#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <time.h>

#define SS_PIN  D8
#define RST_PIN D3
#define GREEN_LED D0
#define RED_LED   D2
MFRC522 rfid(SS_PIN, RST_PIN);

// ---- WiFi ----
#define WIFI_SSID     "A06"
#define WIFI_PASSWORD "12345678"

// ---- Firebase ----
#define API_KEY      "AIzaSyBd4zHR4FIAQiip0DKHskPjVsrV49RQYcs"
#define DATABASE_URL "https://getsms-6308e-default-rtdb.firebaseio.com"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---- NTP settings (IST = UTC+5:30) ----
#define GMT_OFFSET_SEC      19800
#define DAYLIGHT_OFFSET_SEC 0
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";

// ---- Timeouts / retry limits (tune as needed) ----
const unsigned long WIFI_TIMEOUT_MS   = 15000;  // 15s per attempt
const int            WIFI_MAX_RETRIES = 4;      // ~1 min total before reboot
const unsigned long NTP_TIMEOUT_MS    = 15000;
const int            NTP_MAX_RETRIES  = 4;
const int            FIREBASE_SIGNUP_MAX_RETRIES = 3;
const int            RFID_INIT_MAX_RETRIES = 5;

// ---- Whitelist of allowed UIDs (uppercase, no spaces) ----
const char* allowedUIDs[] = {
  "C184C6AD",
  "B7D6D405",
  "51A5B3AD"
};
const int numAllowed = sizeof(allowedUIDs) / sizeof(allowedUIDs[0]);

bool firebaseReady = false;

// ---------------- Helper: UID ----------------
String getUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

bool isAllowed(const String &uid) {
  for (int i = 0; i < numAllowed; i++) {
    if (uid == allowedUIDs[i]) return true;
  }
  return false;
}

// ---------------- Helper: Date/Time ----------------
String getDate() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return String(buf);
}

String getTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[10];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buf);
}

// ---------------- LED helpers ----------------
void ledsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
}

// Blink both LEDs to show "something went wrong, restarting" before reboot
void blinkErrorAndRestart(const char* reason) {
  Serial.println(String("FATAL: ") + reason + " -> restarting ESP in 3s");
  for (int i = 0; i < 6; i++) {
    digitalWrite(RED_LED, !digitalRead(RED_LED));
    delay(250);
    ESP.wdtFeed(); // keep watchdog happy while we blink
  }
  ledsOff();
  delay(500);
  ESP.restart();
}

// ---------------- WiFi connect with timeout + retries ----------------
bool connectWiFi() {
  for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; attempt++) {
    Serial.printf("WiFi connect attempt %d/%d...\n", attempt, WIFI_MAX_RETRIES);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); // clear any stale state before retrying
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      ESP.wdtFeed();           // feed watchdog while waiting
      if (millis() - start > WIFI_TIMEOUT_MS) {
        Serial.println("\nWiFi attempt timed out.");
        break;
      }
      delay(300);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
      return true;
    }
  }
  return false; // exhausted retries
}

// ---------------- NTP sync with timeout + retries ----------------
bool syncTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);

  for (int attempt = 1; attempt <= NTP_MAX_RETRIES; attempt++) {
    Serial.printf("NTP sync attempt %d/%d...\n", attempt, NTP_MAX_RETRIES);
    unsigned long start = millis();
    time_t now = time(nullptr);

    while (now < 100000) {
      ESP.wdtFeed();
      if (millis() - start > NTP_TIMEOUT_MS) {
        Serial.println("\nNTP attempt timed out.");
        break;
      }
      delay(300);
      Serial.print(".");
      now = time(nullptr);
    }

    if (now >= 100000) {
      Serial.println("\nTime synced: " + getDate() + " " + getTime());
      return true;
    }
    // brief pause then retry the configTime call too, in case the
    // first attempt picked an unreachable NTP server
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);
  }
  return false;
}

// ---------------- RFID init with verification + retries ----------------
bool initRFID() {
  for (int attempt = 1; attempt <= RFID_INIT_MAX_RETRIES; attempt++) {
    SPI.begin();
    rfid.PCD_Init();
    delay(50); // let the module settle after init

    byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.printf("RC522 init attempt %d/%d, VersionReg = 0x%02X\n",
                  attempt, RFID_INIT_MAX_RETRIES, version);

    // 0x00 / 0xFF generally mean "no SPI response" i.e. not wired/powered yet
    if (version != 0x00 && version != 0xFF) {
      return true;
    }
    ESP.wdtFeed();
    delay(300);
  }
  return false;
}

// ---------------- Firebase init with retries ----------------
bool initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  for (int attempt = 1; attempt <= FIREBASE_SIGNUP_MAX_RETRIES; attempt++) {
    Serial.printf("Firebase signUp attempt %d/%d...\n", attempt, FIREBASE_SIGNUP_MAX_RETRIES);
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase signUp OK");
      Firebase.begin(&config, &auth);
      Firebase.reconnectWiFi(true);
      return true;
    }
    Serial.printf("Firebase signUp FAILED: %s\n", config.signer.signupError.message.c_str());
    ESP.wdtFeed();
    delay(1000);
  }
  return false;
}

// ---------------- setup() ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\nBooting...");

  // Enable a hardware watchdog as a last-resort safety net.
  // If anything hangs for >8s without feeding it, the chip reboots itself.
  ESP.wdtEnable(8000);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  ledsOff();

  // ---- RFID reader ----
  if (!initRFID()) {
    blinkErrorAndRestart("RC522 not responding after retries");
    // never reaches here, restart() reboots the chip
  }
  Serial.println("RC522 ready.");

  // ---- WiFi ----
  if (!connectWiFi()) {
    blinkErrorAndRestart("WiFi failed to connect after retries");
  }

  // ---- Time sync ----
  if (!syncTime()) {
    // Not fatal on its own, but timestamps will be wrong. We still
    // restart so the next boot gets a clean shot at a correct clock,
    // since Firebase log timestamps depend on it.
    blinkErrorAndRestart("NTP time sync failed after retries");
  }

  // ---- Firebase ----
  firebaseReady = initFirebase();
  if (!firebaseReady) {
    // Don't hard-restart forever here: keep the door/RFID side working
    // even if Firebase logging is temporarily unavailable, but make it
    // very visible and keep retrying in the background from loop().
    Serial.println("WARNING: Firebase not ready. Will keep retrying in loop().");
  }

  // Confirm successful boot with a quick green flash
  digitalWrite(GREEN_LED, HIGH);
  delay(300);
  digitalWrite(GREEN_LED, LOW);

  Serial.println("Setup complete. Scan a tag...");
}

// ---------------- loop() ----------------
unsigned long lastFirebaseRetry = 0;
const unsigned long FIREBASE_RETRY_INTERVAL_MS = 30000; // try every 30s if down

void loop() {
  ESP.wdtFeed(); // feed watchdog every loop iteration

  // Keep WiFi alive; ESP8266 + Firebase library can drop WiFi silently
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped — reconnecting...");
    if (!connectWiFi()) {
      blinkErrorAndRestart("Lost WiFi and could not reconnect");
    }
  }

  // Retry Firebase init periodically in the background if it never came up
  if (!firebaseReady && millis() - lastFirebaseRetry > FIREBASE_RETRY_INTERVAL_MS) {
    lastFirebaseRetry = millis();
    Serial.println("Retrying Firebase init...");
    firebaseReady = initFirebase();
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = getUID();
  Serial.println("Scanned UID: " + uid);

  if (isAllowed(uid)) {
    Serial.println("Authorized tag.");
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);

    String dateStr = getDate();
    String timeStr = getTime();
    String basePath = "/scans/" + uid;

    if (firebaseReady && Firebase.ready()) {
      bool ok1 = Firebase.RTDB.setString(&fbdo, (basePath + "/lastScanDate").c_str(), dateStr);
      bool ok2 = Firebase.RTDB.setString(&fbdo, (basePath + "/lastScanTime").c_str(), timeStr);

      // one retry on failure instead of silently dropping the log
      if (!ok1) ok1 = Firebase.RTDB.setString(&fbdo, (basePath + "/lastScanDate").c_str(), dateStr);
      if (!ok2) ok2 = Firebase.RTDB.setString(&fbdo, (basePath + "/lastScanTime").c_str(), timeStr);

      if (ok1 && ok2) {
        Serial.println("Sent successfully: Date=" + dateStr + " Time=" + timeStr);
      } else {
        Serial.println("Firebase write error: " + fbdo.errorReason());
      }
    } else {
      Serial.println("Firebase not ready — access granted locally, log skipped (will retry connection in background).");
    }
  } else {
    Serial.println("Unauthorized tag — ignored, nothing sent.");
    digitalWrite(RED_LED, HIGH);
    digitalWrite(GREEN_LED, LOW);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(1500);
  ledsOff();
  delay(1000);
}
