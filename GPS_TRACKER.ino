
#include <TinyGPS++.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// ================= GPS =================
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
#define GPS_RX_PIN   16
#define GPS_TX_PIN   17
#define GPS_BAUD     9600

// ================= WiFi =================
const char* ssid     = "A06";
const char* password = "12345678";

// ================= Telegram =================
#define BOTtoken  ""
#define CHAT_ID   ""

// ROOT CAUSE FIX 1:
// Reduce client timeout from default (~10s) to 3s
// This makes getUpdates() return fast instead of blocking the loop
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// ================= LocationIQ =================
#define LOCATIONIQ_TOKEN "pk.199c338beef531fc5bb0dcd342c551e5"

// ================= Deep Sleep =================
#define SLEEP_CHECK_INTERVAL_US  30000000ULL   // 30 seconds

// ================= RTC Memory =================
// All 3 survive deep sleep
RTC_DATA_ATTR bool     sleepRequested      = false;
RTC_DATA_ATTR int      bootCount           = 0;
RTC_DATA_ATTR int32_t  lastMsgIdSaved      = -1;

// ================= Runtime flags =================
bool trackingActive = false;
bool stopSignal     = false;

// ================= Timing =================
unsigned long lastSendTime  = 0;
unsigned long lastBotCheck  = 0;

// ROOT CAUSE FIX 1: check Telegram every 800ms (was 1500ms)
const unsigned long SEND_INTERVAL      = 60000;
const unsigned long BOT_CHECK_INTERVAL = 800;

// ============================================================
//  Connect WiFi
// ============================================================
bool connectWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setInsecure();

 
  client.setTimeout(3);

  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected ✓  IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\nWiFi FAILED");
  return false;
}

void flushOldMessages() {
  Serial.println("Flushing old Telegram messages...");


  if (lastMsgIdSaved > 0) {
    bot.last_message_received = lastMsgIdSaved;
    Serial.println("Restored lastMsgId from RTC: " + String(lastMsgIdSaved));
    return;   // no need to flush — we already know where we left off
  }

  
  int count = 0;
  int msgs = bot.getUpdates(0);
  while (msgs > 0) {
    count += msgs;
    msgs = bot.getUpdates(bot.last_message_received + 1);
  }
  Serial.println("Flushed " + String(count) + " old message(s). "
                 "Now at msgId: " + String(bot.last_message_received));

  
  lastMsgIdSaved = bot.last_message_received;
}

String reverseGeocode(double lat, double lon) {
  if (WiFi.status() != WL_CONNECTED) return "WiFi not connected";

  String url = "http://us1.locationiq.com/v1/reverse.php?key=";
  url += LOCATIONIQ_TOKEN;
  url += "&lat=" + String(lat, 7);
  url += "&lon=" + String(lon, 7);
  url += "&format=json";

  String address = "Location unavailable";
  int httpCode   = -1;
  String payload = "";

  
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.println("LocationIQ attempt " + String(attempt) + "/3");

    
    WiFiClient  plainClient;
    HTTPClient  http;

    http.setReuse(false);          // FIX: disable connection reuse
    http.setTimeout(10000);        // 10s timeout per attempt
    http.begin(plainClient, url);
    http.addHeader("Connection", "close");   // FIX: force close after response

    httpCode = http.GET();

    if (httpCode == 200) {
      payload = http.getString();
      http.end();
      break;                       // success — exit retry loop
    } else {
      Serial.println("LocationIQ error " + String(httpCode) +
                     " on attempt " + String(attempt));
      http.end();
      if (attempt < 3) delay(1500);  // wait 1.5s before retry
    }
  }

  // --- Parse result ---
  if (httpCode == 200 && payload.length() > 0) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      String shortLabel = "";
      JsonObject addrObj = doc["address"];
      if (!addrObj.isNull()) {
        const char* fields[] = {
          "amenity", "road", "suburb", "city_district",
          "city", "town", "village", "county", "state", "country"
        };
        int count = 0;
        for (const char* f : fields) {
          if (addrObj.containsKey(f) && count < 4) {
            if (shortLabel.length() > 0) shortLabel += ", ";
            shortLabel += addrObj[f].as<String>();
            count++;
          }
        }
      }
      address = (shortLabel.length() > 0)
                  ? shortLabel
                  : doc["display_name"].as<String>();
    } else {
      address = "JSON parse error";
      Serial.println("JSON err: " + String(err.c_str()));
    }
  } else {
    address = "Geocode failed (err " + String(httpCode) + ")";
    Serial.println("LocationIQ final failure: " + String(httpCode));
  }

  return address;
}

void sendTelegramLocation() {
  if (!gps.location.isValid()) {
    bot.sendMessage(CHAT_ID,
      "No GPS fix yet.\nMake sure the antenna has a clear view of the sky.",
      "");
    Serial.println("GPS not valid - notified user");
    return;
  }

  double lat = gps.location.lat();
  double lon = gps.location.lng();

  Serial.println("Reverse geocoding...");
  String locationName = reverseGeocode(lat, lon);

  
  String message = "*GPS Location Update*\n\n";


  if (locationName.startsWith("Geocode failed") ||
      locationName == "Location unavailable" ||
      locationName == "WiFi not connected") {
    message += "Place: Coordinates only (geocode unavailable)\n\n";
  } else {
    message += "Place: " + locationName + "\n\n";
  }

  message += "Coordinates:\n";
  message += "  Latitude  : " + String(lat, 7) + "\n";
  message += "  Longitude : " + String(lon, 7) + "\n";

  if (gps.altitude.isValid()) {
    message += "  Altitude  : " + String(gps.altitude.meters(), 2) + " m\n";
  }

  if (gps.speed.isValid()) {
    message += "\nSpeed: " + String(gps.speed.kmph(), 2) + " km/h\n";
  }

  // Satellites — always shown
  if (gps.satellites.isValid()) {
    message += "Satellites: " + String(gps.satellites.value()) + " connected\n";
  } else {
    message += "Satellites: Acquiring...\n";
  }

  if (gps.date.isValid() && gps.time.isValid()) {
    char dtBuf[32];
    snprintf(dtBuf, sizeof(dtBuf), "%04d-%02d-%02d %02d:%02d:%02d UTC",
             gps.date.year(),   gps.date.month(),  gps.date.day(),
             gps.time.hour(),   gps.time.minute(), gps.time.second());
    message += "\nTime: " + String(dtBuf) + "\n";
  }

  message += "\nhttps://maps.google.com/?q=";
  message += String(lat, 7) + "," + String(lon, 7);

  // FIX: Retry Telegram send up to 3 times
  bool ok = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    ok = bot.sendMessage(CHAT_ID, message, "");
    if (ok) {
      Serial.println("Telegram sent OK (attempt " + String(attempt) + ")");
      break;
    }
    Serial.println("Telegram attempt " + String(attempt) + " failed, retrying...");
    delay(1000);
  }
  if (!ok) {
    Serial.println("Telegram FAILED after 3 attempts");
  }
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep on /stop command...");

 
  lastMsgIdSaved = bot.last_message_received;  // ROOT CAUSE FIX 3
  sleepRequested = true;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  gpsSerial.end();

  esp_sleep_enable_timer_wakeup(SLEEP_CHECK_INTERVAL_US);
  delay(300);
  esp_deep_sleep_start();
}


void handleTelegramCommands() {

  int numMessages = bot.getUpdates(bot.last_message_received + 1);

  while (numMessages) {
    for (int i = 0; i < numMessages; i++) {
      String chat_id  = bot.messages[i].chat_id;
      String text     = bot.messages[i].text;
      text.trim();

      String textLower = text;
      textLower.toLowerCase();

      Serial.println("MSG [" + chat_id + "]: " + text);

     
      lastMsgIdSaved = bot.last_message_received;

      // Security check
      if (chat_id != String(CHAT_ID)) {
        bot.sendMessage(chat_id, "⛔ Unauthorised user.", "");
        continue;
      }

      // ======== /start ========
      if (textLower == "/start") {
        if (!trackingActive) {
          trackingActive = true;
          sleepRequested = false;
          stopSignal     = false;
          lastSendTime   = millis();   // reset 60s timer

          String reply = "✅ *Tracker Started!*\n\n";
          reply += "📡 GPS tracking is now *ON*\n";
          reply += "📨 Location sent every *60 seconds*\n";
          reply += "🛰 Satellite count included in every update\n\n";
          reply += "Commands:\n";
          reply += "/stop   → Pause & enter deep sleep\n";
          reply += "/status → Get location right now\n";
          reply += "/help   → Show all commands";
          bot.sendMessage(CHAT_ID, reply, "Markdown");
          Serial.println("✓ Tracking STARTED");
        } else {
          bot.sendMessage(CHAT_ID,
            "ℹ️ Tracker is already *running*.\nSend /stop to pause it.",
            "Markdown");
        }
      }

      // ======== /stop ========
      else if (textLower == "/stop") {
        trackingActive = false;
        stopSignal     = true;   
        String reply = "⏸ *Tracker Stopped*\n\n";
        reply += "💤 ESP32 entering *deep sleep*\n";
        reply += "🔋 Power draw: ~10µA (near zero)\n\n";
        reply += "📲 Send */start* anytime to wake it up\n";
        reply += "_(Wakes every 30s to check for your command)_";
        bot.sendMessage(CHAT_ID, reply, "Markdown");
        Serial.println("✓ STOP command received");
      }

      // ======== /status ========
      else if (textLower == "/status") {
        // Completely isolated — stopSignal is never touched here
        if (trackingActive) {
          bot.sendMessage(CHAT_ID,
            "🟢 Tracker is *RUNNING*\nFetching location now...", "Markdown");
          sendTelegramLocation();
        } else {
          bot.sendMessage(CHAT_ID,
            "🔴 Tracker is *STOPPED*\nSend /start to begin tracking.",
            "Markdown");
        }
      }

      // ======== /help ========
      else if (textLower == "/help") {
        String reply = "📋 *Available Commands:*\n\n";
        reply += "/start  → Start GPS tracking\n";
        reply += "/stop   → Stop & enter deep sleep\n";
        reply += "/status → Get current location instantly\n";
        reply += "/help   → Show this menu";
        bot.sendMessage(CHAT_ID, reply, "Markdown");
      }

      // ======== Unknown ========
      else {
        bot.sendMessage(CHAT_ID,
          "❓ Unknown command.\nSend /help to see available commands.", "");
      }
    }

    numMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  bootCount++;

  Serial.println("\n==================================");
  Serial.println("GPS Tracker");
  Serial.println("  Boot #"        + String(bootCount));
  Serial.println("  sleepRequested = " + String(sleepRequested));
  Serial.println("  lastMsgIdSaved = " + String(lastMsgIdSaved));
  Serial.println("==================================");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Connect WiFi — restart on failure (never sleep on failure)
  if (!connectWiFi()) {
    Serial.println("WiFi failed — restarting in 10s");
    delay(10000);
    ESP.restart();
    return;
  }

  // ROOT CAUSE FIX 2 + 3:
  // Flush/sync message state BEFORE any command is processed.
  // No delay(2000) here anymore — flushOldMessages() handles sync properly.
  flushOldMessages();

  // -------------------------------------------------------
  //  SLEEP-POLL MODE — only if user previously sent /stop
  // -------------------------------------------------------
  if (sleepRequested) {
    Serial.println("--- Sleep-poll: checking for /start ---");

    handleTelegramCommands();

    if (!trackingActive) {
      // /start not received yet — back to sleep immediately
      Serial.println("No /start found — returning to sleep");
      lastMsgIdSaved = bot.last_message_received;   // save before sleeping
      esp_sleep_enable_timer_wakeup(SLEEP_CHECK_INTERVAL_US);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(200);
      esp_deep_sleep_start();
      return;
    }

    // /start received during poll — continue to main loop
    Serial.println("✓ /start received — starting tracking!");

  // -------------------------------------------------------
  //  FRESH BOOT — power on or manual reset
  // -------------------------------------------------------
  } else {
    Serial.println("Fresh boot — ready for commands");

    String bootMsg = "🔌 *ESP32 GPS Tracker Online*\n\n";
    bootMsg += "Boot #`" + String(bootCount) + "`\n";
    bootMsg += "Status: *Waiting for command*\n\n";
    bootMsg += "Send /start to begin GPS tracking\n";
    bootMsg += "Send /help for all commands";
    bot.sendMessage(CHAT_ID, bootMsg, "Markdown");

    // Save position after sending boot message
    lastMsgIdSaved = bot.last_message_received;
  }

  lastSendTime = millis();
  lastBotCheck = millis();
  stopSignal   = false;

  Serial.println("Setup complete — entering main loop");
}

void loop() {
  unsigned long now = millis();

  // ---- Feed GPS parser continuously ----
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // ---- Check Telegram every 800ms ----
  if (now - lastBotCheck >= BOT_CHECK_INTERVAL) {
    lastBotCheck = now;

    handleTelegramCommands();

    
    if (stopSignal) {
      delay(600);        
      enterDeepSleep();
      return;
    }
  }

  // ---- GPS wiring check every 5s ----
  static unsigned long lastWireCheck = 0;
  if (now - lastWireCheck >= 5000) {
    lastWireCheck = now;
    if (gps.charsProcessed() < 10) {
      Serial.println("⚠ No GPS data! Check: GPS TX→Pin16, GPS RX→Pin17");
    }
  }

  // ---- Serial debug every 2s ----
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 2000) {
    lastPrint = now;
    Serial.print("Track:" + String(trackingActive ? "ON" : "OFF"));
    Serial.print("  GPS:" + String(gps.location.isValid() ? "FIX" : "NO FIX"));
    if (gps.location.isValid()) {
      Serial.print("  Lat:" + String(gps.location.lat(), 5));
      Serial.print("  Lon:" + String(gps.location.lng(), 5));
    }
    if (gps.satellites.isValid()) {
      Serial.print("  Sats:" + String(gps.satellites.value()));
    }
    Serial.println();
  }

  // ---- Auto-send every 60s (only when tracking ON and GPS fixed) ----
  if (trackingActive && (now - lastSendTime >= SEND_INTERVAL)) {
    lastSendTime = now;
    if (gps.location.isValid()) {
      sendTelegramLocation();
    } else {
      Serial.println("60s tick — still waiting for GPS fix");
    }
  }
}
