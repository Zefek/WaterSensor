#include "camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"

size_t CHUNK_SIZE = 512;

// ⏱ Interval snímání (5 minut)
const unsigned long interval = 5 * 60 * 1000;
unsigned long lastCaptureTime = 0;

// 📸 Snímání a odeslání fotky
void captureAndSend() 
{
  camera_fb_t* fb = capture();
  if (!fb) 
  {
    Serial.println("❌ Chyba při pořizování obrázku");
    return;
  }
  Serial.printf("📸 Pořízen obrázek (%d B)\n", fb->len);
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("Připojení k serveru");
  if (!client.connect(Server, 443)) {
    Serial.println("❌ Nelze se připojit k serveru");
    returnFb(fb);
    return;
  }
  Serial.println("Připojeno");
  String boundary = "ESP32CAM";
  String bodyStart = "--" + boundary + "\r\n"
                     "Content-Disposition: form-data; name=\"file\"; filename=\"vodomer.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n\r\n";
  String bodyEnd   = "\r\n--" + boundary + "--\r\n";

  int contentLength = bodyStart.length() + fb->len + bodyEnd.length();
  Serial.print("Délka požadavku ");
  Serial.println(contentLength);
  Serial.println("Odesílám požadavek");
  // Vytvoření HTTP požadavku
  client.println("POST " + String(endpoint) + " HTTP/1.1");
  client.println("Host: " + String(Server));
  client.println("Authorization: Basic " + String(auth));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Content-Length: " + String(contentLength));
  client.println();  // konec hlaviček

  // Tělo požadavku
  client.print(bodyStart);
  size_t bytesSent = 0;
  while (bytesSent < fb->len) 
  {
    size_t bytesToSend = min(CHUNK_SIZE, fb->len - bytesSent);
    client.write(fb->buf + bytesSent, bytesToSend);
    bytesSent += bytesToSend;
  }
  client.print(bodyEnd);
  client.flush();
  Serial.println("Požadavek odeslán, čekám na odpověď");
  // Čtení odpovědi
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;  // konec hlaviček
  }

  String response = client.readString();
  Serial.println("✅ Odpověď serveru:");
  Serial.println(response);

  returnFb(fb);
}

void setup() 
{
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(WifiSSID, WifiPassword);
  Serial.print("🔌 Připojuji se na Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi připojeno");

  if (!initCamera()) 
  {
    Serial.println("💀 Chyba při inicializaci kamery");
    while (true);
  }

  lastCaptureTime = millis() - interval; // pro okamžité první snímání
}

void loop() 
{
  if(WiFi.status() != WL_CONNECTED)
  {
    WiFi.begin(WifiSSID, WifiPassword);
    Serial.print("🔌 Připojuji se na Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) 
    {
      delay(500);
      Serial.print(".");
    }
  }
  if (millis() - lastCaptureTime >= interval) 
  {
    Serial.println("📸 Pořizuji snímek a odesílám...");
    captureAndSend();
    lastCaptureTime = millis();
  }

  delay(1000); // šetřič CPU
}
