#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "A06";
const char* password = "12345678";
const char* serverURL = "https://mongo-api-f7it.onrender.com/data";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected!");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");

    // Send latitude and longitude
    float latitude = 24.8170;   // Replace with real GPS value
    float longitude = 91.2772;  // Replace with real GPS value

    String jsonData = "{\"latitude\": " + String(latitude, 6) + 
                      ", \"longitude\": " + String(longitude, 6) + "}";

    int responseCode = http.POST(jsonData);

    if (responseCode > 0) {
      String response = http.getString();
      Serial.println("✅ Sent! Response: " + response);
    } else {
      Serial.println("❌ Failed: " + String(responseCode));
    }

    http.end();
  }
  delay(5000); // Send every 5 seconds
}