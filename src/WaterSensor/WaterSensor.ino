#include "camera.h"
#include "config.h"
#include <WiFi.h>

size_t CHUNK_SIZE = 512;
const uint16_t DELAY_BETWEEN_CHUNKS_MS = 20;
const uint16_t CLIENT_TIMEOUT_S = 30;

//Interval snímání (5 minut)
const unsigned long interval = 5 * 60 * 1000;
unsigned long lastCaptureTime = 0;

int readHttpStatus(WiFiClient& client) 
{
  uint32_t start = millis();
  String statusLine = "";
  while (millis() - start < (uint32_t)CLIENT_TIMEOUT_S * 1000) 
  {
    if (!client.connected() && client.available() == 0) 
    {
      return -1;
    }
    if (client.available()) 
    {
      statusLine = client.readStringUntil('\n');
      statusLine.trim();
      if (statusLine.length() > 0) 
      {
        int firstSpace = statusLine.indexOf(' ');
        if (firstSpace > 0) {
          int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
          String codeStr = (secondSpace > firstSpace) ? statusLine.substring(firstSpace + 1, secondSpace) : statusLine.substring(firstSpace + 1);
          int code = codeStr.toInt();
          return code;
        }
      }
    }
    delay(5);
  }
  return -1;
}

void captureAndSend() 
{
  initCamera();
  camera_fb_t* fb = capture();

  if (!fb) 
  {
    Serial.println("Chyba při pořizování obrázku");
    return;
  }
  size_t len = fb->len;
  Serial.printf("Pořízen obrázek (%d B)\n", len);
  int rssi = WiFi.RSSI();
  Serial.print("RSSI: ");
  Serial.println(rssi);
  int tryCount = 0;
  size_t sent = 0;
  uint32_t t0 = millis();
  WiFiClient client;
  do
  {
    client.stop();
    client.setNoDelay(true);
    client.setTimeout(CLIENT_TIMEOUT_S);
    if(tryCount > 0)
    {
      delay(5000);
    }
    if (!client.connect(Server, Port)) 
    {
      Serial.println("Nepodařilo se připojit ke službě.");
      if(tryCount == 5)
      {
        break;
      }
      tryCount++;
      continue;
    }
    client.printf("POST %s HTTP/1.0\r\n", endpoint);
    client.printf("Host: %s\r\n", Server);
    client.println("User-Agent: ESP32-CAM-Watermeter/1.0");
    client.println("Content-Type: application/octet-stream");
    client.printf("Content-Length: %u\r\n", (unsigned)len);
    client.printf("Authorization: %s\r\n", auth);
    client.println("Connection: close\r\n");

    sent = 0;
  
    t0 = millis();
    uint32_t tStart = millis();

    while (sent < len && client.connected()) 
    {
      size_t chunk = (len - sent) > CHUNK_SIZE ? CHUNK_SIZE : (len - sent);
      uint32_t t_chunk_start = millis();
      size_t wrote = client.write(fb->buf + sent, chunk);
      if (wrote == 0) 
      {
        if(millis() - tStart > 5000)
        {
          client.stop();
          break;
        }
        if (DELAY_BETWEEN_CHUNKS_MS) 
        {
          delay(DELAY_BETWEEN_CHUNKS_MS);
        }
      }
      else
      {
        tStart = millis();
      }
      uint32_t t_chunk_end = millis();
      uint32_t delta_ms = t_chunk_end - t_chunk_start;
      float speed_kB_s = (wrote / 1024.0) / (delta_ms / 1000.0); // rychlost v kB/s
      Serial.printf("TryCount %d  Chunk [%u, %u] odesláno %u bajtů, doba: %u ms, rychlost: %.2f kB/s\n",
                    tryCount,
                    (unsigned int)sent,
                    (unsigned int)chunk,
                    (unsigned int)wrote,
                   (unsigned int)delta_ms,
                   speed_kB_s);
      sent += wrote;
      if (DELAY_BETWEEN_CHUNKS_MS) 
      {
        delay(DELAY_BETWEEN_CHUNKS_MS);
      }
    }
    if (DELAY_BETWEEN_CHUNKS_MS) 
    {
      delay(DELAY_BETWEEN_CHUNKS_MS);
    }
    tryCount++;
  }
  while(sent < len && tryCount < 5);
  returnFb(fb);
  deInit();
  uint32_t duration = millis() - t0;

  int httpCode = readHttpStatus(client);
  client.stop();
  size_t freeHeap = ESP.getFreeHeap();
  rssi = WiFi.RSSI();

  Serial.printf("Trycount = %d  size=%u sent=%u dur=%u code=%d freeHeap=%u rssi=%d\n",
        tryCount, (unsigned)len, (unsigned)sent, (unsigned)duration, httpCode, (unsigned)freeHeap, rssi);  
}

void connectToWifi()
{
  WiFi.begin(WifiSSID, WifiPassword);
  Serial.print("Připojuji se na Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi připojeno");
}

void setup() 
{
  Serial.begin(115200);
  delay(1000);
  lastCaptureTime = millis() - interval; // pro okamžité první snímání
}

void loop() 
{
  if(WiFi.status() != WL_CONNECTED)
  {
    connectToWifi();
  }
  
  if (millis() - lastCaptureTime >= interval) 
  {
    Serial.println("Pořizuji snímek a odesílám...");
    captureAndSend();
    lastCaptureTime = millis();
  }

  delay(1000);
}
