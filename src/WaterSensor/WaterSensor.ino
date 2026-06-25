#include "camera.h"
#include "ota.h"
#include "diagnostics.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"

size_t CHUNK_SIZE = 1460;
const uint16_t DELAY_BETWEEN_CHUNKS_MS = 20;
const uint16_t CLIENT_TIMEOUT_S = 30;
const int BASE_DELAY_MS = 1000;
const int MAX_RETRIES = 5;

//Interval snímání (1 minut)
const unsigned long interval = 1 * 60 * 1000;
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

bool postBinary(const char* path, const uint8_t* data, size_t len)
{
  WiFiClientSecure client;
  client.setCACert(RootCA);
  client.setNoDelay(true);
  client.setTimeout(CLIENT_TIMEOUT_S * 1000);
  if (!client.connect(Server, Port))
  {
    client.stop();
    return false;
  }
  client.printf("POST %s HTTP/1.0\r\n", path);
  client.printf("Host: %s\r\n", Server);
  client.println("User-Agent: ESP32-CAM-Watermeter/1.0");
  client.println("Content-Type: application/octet-stream");
  client.printf("Content-Length: %u\r\n", (unsigned)len);
  client.printf("Authorization: %s\r\n", auth);
  client.println("Connection: close\r\n");

  size_t sent = 0;
  while (sent < len && client.connected())
  {
    size_t w = client.write(data + sent, len - sent);
    if (w == 0) break;
    sent += w;
  }
  int code = readHttpStatus(client);
  client.stop();
  return code >= 200 && code < 300;
}

void captureAndSend()
{
  camera_fb_t* fb = capture();

  if (!fb)
  {
    Serial.println("Chyba při pořizování obrázku");
    diagCountCameraError();
    return;
  }
  diagCountCapture();
  size_t len = fb->len;
  Serial.printf("Pořízen obrázek (%d B)\n", len);
  int rssi = WiFi.RSSI();
  Serial.print("RSSI: ");
  Serial.println(rssi);
  int tryCount = 0;
  size_t sent = 0;
  int httpCode = -1;
  bool success = false;
  uint32_t t0 = millis();
  WiFiClientSecure client;
  client.setCACert(RootCA);
  Serial.printf("Pred TLS (kamera aktivni): freeHeap=%u maxAlloc=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  uint64_t corrId = diagNextCorrelationId();
  char corrHex[DIAG_CORR_HEX_BUF];
  diagCorrelationHex(corrId, corrHex, sizeof(corrHex));
  char prevHex[DIAG_TRANSFER_HEX_BUF];
  bool hasPrev = diagPrevTransferHex(prevHex, sizeof(prevHex));
  uint32_t duration = 0;
  do
  {
    duration = 0;
    sent = 0;
    client.stop();
    client.setNoDelay(true);
    client.setTimeout(CLIENT_TIMEOUT_S * 1000);

    if(tryCount > 0)
    {
      delay(BASE_DELAY_MS << (tryCount - 1));
    }

    if (!client.connect(Server, Port))
    {
      char errBuf[120] = {0};
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("Nepodařilo se připojit ke službě %s:%d. sslErr=%d (%s) freeHeap=%u maxAlloc=%u time=%ld\n",
                    Server, (int)Port, sslErr, errBuf,
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMaxAllocHeap(),
                    (long)time(nullptr));
      diagCountTlsError();
      tryCount++;
      continue;
    }
    client.printf("POST %s HTTP/1.0\r\n", endpoint);
    client.printf("Host: %s\r\n", Server);
    client.println("User-Agent: ESP32-CAM-Watermeter/1.0");
    client.println("Content-Type: application/octet-stream");
    client.printf("Content-Length: %u\r\n", (unsigned)len);
    client.printf("Authorization: %s\r\n", auth);
    client.printf("X-Correlation-Id: %s\r\n", corrHex);
    if (hasPrev)
    {
      client.printf("X-Transfer: %s\r\n", prevHex);
    }
    client.println("Connection: close\r\n");
    
    t0 = millis();
    uint32_t tStart = millis();

    while (sent < len && client.connected())
    {
      size_t chunk = (len - sent) > CHUNK_SIZE ? CHUNK_SIZE : (len - sent);
      size_t wrote = client.write(fb->buf + sent, chunk);
      if (wrote == 0)
      {
        if (millis() - tStart > 5000)
        {
          client.stop();
          break;
        }
        delay(DELAY_BETWEEN_CHUNKS_MS);
        continue;
      }
      sent += wrote;
      tStart = millis();
    }
    duration = millis() - t0;
    if (sent == len)
    {
      httpCode = readHttpStatus(client);
      if (httpCode >= 200 && httpCode < 300)
      {
        success = true;
      }
      else
      {
        Serial.printf("Server odpověděl chybou %d, retry...\n", httpCode);
      }
    }
    tryCount++;
  }
  while(!success && tryCount < MAX_RETRIES);
  returnFb(fb);
  client.stop();

  size_t freeHeap = ESP.getFreeHeap();
  rssi = WiFi.RSSI();

  float avg_kB_s = (duration > 0) ? (sent / 1024.0f) / (duration / 1000.0f) : 0.0f;

  Serial.printf("Trycount=%d success=%d size=%u sent=%u dur=%u code=%d speed=%.2f kB/s freeHeap=%u rssi=%d\n",
        tryCount, success, (unsigned)len, (unsigned)sent, (unsigned)duration, httpCode, avg_kB_s, (unsigned)freeHeap, rssi);

  if (!success) diagCountSendFailure();

  diagRecordTransfer(corrId, (uint32_t)len, (uint32_t)sent, duration,
                     (uint8_t)tryCount, success, (int16_t)httpCode, (int8_t)rssi);
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
  diagCountWifiReconnect();
  otaBegin();
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  if (!initCamera())
  {
    Serial.println("Kamera se nepodařila inicializovat, restartuji...");
    delay(2000);
    ESP.restart();
  }

  warmUp(3);

  lastCaptureTime = millis() - interval;
}

void loop()
{
  uint32_t loopStart = millis();

  if(WiFi.status() != WL_CONNECTED)
  {
    connectToWifi();
  }
  if(WiFi.status() == WL_CONNECTED)
  {
    static uint32_t lastCfgAttempt = 0;
    if (diagConfigChanged() && (lastCfgAttempt == 0 || millis() - lastCfgAttempt >= 30000UL))
    {
      lastCfgAttempt = millis();
      uint8_t cfgBuf[32];
      size_t n = diagBuildConfigBlob(cfgBuf, sizeof(cfgBuf));
      if (n && postBinary(endpointConfig, cfgBuf, n))
      {
        diagMarkConfigSent();
      }
    }

    static uint32_t lastDiag = 0;
    if (lastDiag == 0 || millis() - lastDiag >= DIAG_INTERVAL_MS)
    {
      lastDiag = millis();
      uint8_t diagBuf[40];
      size_t n = diagBuildDeviceBlob(diagBuf, sizeof(diagBuf));
      if (n) postBinary(endpointDiag, diagBuf, n);
    }

    if (millis() - lastCaptureTime >= interval)
    {
      Serial.println("Pořizuji snímek a odesílám...");
      captureAndSend();
      lastCaptureTime = millis();
    }
    otaLoop();
  }

  diagNoteLoopMs(millis() - loopStart);
  delay(1000);
}
