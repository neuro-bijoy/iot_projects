#define BLYNK_TEMPLATE_ID "TMPL3497z4EJc"
#define BLYNK_TEMPLATE_NAME "Temperature and humidity monitor"
#define BLYNK_AUTH_TOKEN "k0xu5Vk-hKlF4jQoPlTPuP-xiVWuOb-8"
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
char ssid[] = "A06";
char pass[] = "12345678";

// DHT11 Settings
#define DHTPIN 4          
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

// Send data every 2 seconds
BlynkTimer timer;

void sendSensor()
{
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  // Check if reading failed
  if (isnan(humidity) || isnan(temperature))
  {
    Serial.println("Failed to read from DHT11 sensor!");
    return;
  }

  // Print on Serial Monitor
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C");

  Serial.print("   Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  // Send data to Blynk
  Blynk.virtualWrite(V1, temperature);
  Blynk.virtualWrite(V2, humidity);
}

void setup()
{
  Serial.begin(115200);

  dht.begin();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  timer.setInterval(2000L, sendSensor);
}

void loop()
{
  Blynk.run();
  timer.run();
}