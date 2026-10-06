#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID     "A06"
#define WIFI_PASSWORD "12345678"
#define API_KEY       ""
#define DATABASE_URL  ""

#define ALERT_LED_PIN  2   // Alert LED — glows when payment received
#define WIFI_LED_PIN   5   // Green WiFi status LED
#define RELAY_PIN      16  // Relay / Motor
#define BUTTON_PIN     18  // Push button

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String lastTimestamp = "";
bool paymentPending  = false;  // Flag — payment waiting for button press
float pendingAmount  = 0;      // Stores extracted amount
bool motorRunning    = false;  // Flag — prevents double trigger

float extractAmount(String title) {
    int rupeeIndex = title.indexOf("₹");

    if (rupeeIndex == -1) {
        rupeeIndex = title.indexOf("Rs");
        if (rupeeIndex == -1) return -1;
        rupeeIndex += 2;
    } else {
        rupeeIndex += 3;
    }

    String amountStr = title.substring(rupeeIndex);
    amountStr.trim();

    Serial.println("Amount string extracted: '" + amountStr + "'");

    float amount = amountStr.toFloat();
    return amount;
}

bool isInteger(float amount) {
    return (amount > 0) && ((int)amount == amount);
}

void runMotor(int durationMs) {
    Serial.println(" Motor ON for "
                   + String(durationMs / 1000) + " seconds!");
    digitalWrite(RELAY_PIN, LOW);   // LOW = ON
    delay(durationMs);
    digitalWrite(RELAY_PIN, HIGH);  // HIGH = OFF
    Serial.println(" Motor OFF.");
}

void setup() {
    Serial.begin(115200);

    pinMode(ALERT_LED_PIN, OUTPUT);
    digitalWrite(ALERT_LED_PIN, LOW);

    pinMode(WIFI_LED_PIN, OUTPUT);
    digitalWrite(WIFI_LED_PIN, LOW);

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH); // Relay OFF by default

    pinMode(BUTTON_PIN, INPUT_PULLUP); // Button with internal pull-up

    // Connect WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi Connected!");
    digitalWrite(WIFI_LED_PIN, HIGH); // Green LED ON — WiFi connected

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;

    if (Firebase.signUp(&config, &auth, "", "")) {
        Serial.println("Firebase Connected!");
    }

    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
}

void loop() {

    //  WiFi LED status check
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(WIFI_LED_PIN, HIGH); // Green LED ON
    } else {
        digitalWrite(WIFI_LED_PIN, LOW);  // Green LED OFF
        Serial.println(" WiFi Disconnected!");
    }

    //  Button check — payment pending + button pressed + motor not running
    if (paymentPending &&
        digitalRead(BUTTON_PIN) == LOW &&
        !motorRunning) {

        Serial.println(" Button Pressed!");
        digitalWrite(ALERT_LED_PIN, LOW); // Turn OFF alert LED

        motorRunning = true; //  Lock button immediately

        if (isInteger(pendingAmount)) {
            Serial.println(" Integer amount — running motor!");
            runMotor(5000); // Run motor for 5 seconds
        } else {
            Serial.println(" Decimal amount — motor stays off.");
        }

        motorRunning   = false; //  Unlock after motor done
        paymentPending = false; // Reset payment flag
        pendingAmount  = 0;     // Reset amount
        delay(500);             // Debounce
    }

    //  Firebase check — only if no payment is pending
    if (!paymentPending && Firebase.ready()) {

        if (Firebase.RTDB.getJSON(&fbdo, "/upi_notifications/latest")) {

            String rawJSON = fbdo.jsonString();

            if (rawJSON == "null" || rawJSON == "" || rawJSON == "{}") {
                Serial.println(" No new data.");
                return;
            }

            // Parse JSON
            FirebaseJson json;
            json.setJsonData(rawJSON);
            FirebaseJsonData result;

            String appVal       = "";
            String titleVal     = "";
            String msgVal       = "";
            String timestampVal = "";

            json.get(result, "app");
            if (result.success) appVal = result.stringValue;

            json.get(result, "title");
            if (result.success) titleVal = result.stringValue;

            json.get(result, "message");
            if (result.success) msgVal = result.stringValue;

            json.get(result, "timestamp");
            if (result.success) timestampVal = result.stringValue;

            json.clear();

            // Skip already processed
            if (timestampVal == "" || timestampVal == lastTimestamp) {
                Serial.println(" Already processed.");
                return;
            }

            lastTimestamp = timestampVal;

            Serial.println("━━━━━━━━━━━━━━━━━━━━");
            Serial.println(" New UPI Payment!");
            Serial.println("App:     " + appVal);
            Serial.println("Title:   " + titleVal);
            Serial.println("Message: " + msgVal);

            // Extract amount
            float amount = extractAmount(titleVal);

            //  DELETE FIRST — retry up to 5 times
            bool deleted = false;
            for (int attempt = 1; attempt <= 5; attempt++) {
                if (Firebase.RTDB.deleteNode(&fbdo,
                        "/upi_notifications/latest")) {
                    Serial.println(" Deleted from Firebase! (attempt "
                                   + String(attempt) + ")");
                    deleted = true;
                    break;
                } else {
                    Serial.println(" Delete attempt " + String(attempt)
                                   + " failed: " + fbdo.errorReason());
                    delay(500);
                }
            }

            if (!deleted) {
                Serial.println(" Could not delete. Skipping.");
                Serial.println("━━━━━━━━━━━━━━━━━━━━");
                return;
            }

            //  After delete — glow alert LED and wait for button
            if (amount < 0) {
                Serial.println(" Could not extract amount.");
            } else {
                Serial.println("Amount:  ₹" + String(amount));
                Serial.println(" Alert LED ON — press button to start motor!");
                pendingAmount  = amount;
                paymentPending = true;
                digitalWrite(ALERT_LED_PIN, HIGH); // Glow alert LED
            }

            Serial.println("━━━━━━━━━━━━━━━━━━━━");

        } else {
            String err = fbdo.errorReason();
            if (err != "path not found") {
                Serial.println("Read error: " + err);
            } else {
                Serial.println(" Nothing to read.");
            }
        }
    }

    delay(100); // Small delay for button responsiveness
}
