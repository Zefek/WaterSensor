#include "camera.h"
#include "ota.h"
#include "diagnostics.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>
#include "config.h"

const size_t CHUNK_SIZE = 1400;
const uint16_t CLIENT_TIMEOUT_S = 30;
const uint32_t WIFI_RETRY_MIN_MS = 15000;
const uint32_t WIFI_RETRY_MAX_MS = 60000;
const uint32_t WIFI_DOWN_RESTART_MS = 10UL * 60UL * 1000UL;
const int BASE_DELAY_MS = 1000;
const int MAX_RETRIES = 5;
const uint8_t MAX_CAMERA_ERRORS = 3;
const uint32_t WDT_TIMEOUT_S = 90;

//Interval snímání (1 minut)
const unsigned long interval = 1 * 60 * 1000;
unsigned long lastCaptureTime = 0;
uint8_t consecutiveCameraErrors = 0;

bool wifiStarted = false;
bool wifiConnected = false;
uint32_t wifiLastAttempt = 0;
uint32_t wifiRetryDelay = 0;
uint32_t wifiDownSince = 0;

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
    esp_task_wdt_reset();
    delay(5);
  }
  return -1;
}

bool postBinary(const char* path, const uint8_t* data, size_t len)
{
  WiFiClientSecure client;
  client.setCACert(RootCA);
  client.setNoDelay(true);
  client.setConnectionTimeout(CLIENT_TIMEOUT_S * 1000);
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
    if (++consecutiveCameraErrors >= MAX_CAMERA_ERRORS)
    {
      Serial.printf("Kamera selhala %dx po sobě, reinicializuji senzor (bez restartu)...\n",
                    consecutiveCameraErrors);
      deInit();
      if (initCamera())
      {
        warmUp(3);
        Serial.println("Kamera reinicializována.");
      }
      else
      {
        Serial.println("Reinicializace kamery se nezdařila, zkusím to znovu později.");
      }
      consecutiveCameraErrors = 0;
    }
    return;
  }
  consecutiveCameraErrors = 0;
  diagCountCapture();
  size_t len = fb->len;
  Serial.printf("Pořízen obrázek (%d B)\n", len);

  FrameStats stats;
  if (measureFrame(fb, &stats))
  {
    noteFrameStats(&stats);
  }

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
    esp_task_wdt_reset();
    duration = 0;
    sent = 0;
    client.stop();
    client.setNoDelay(true);
    client.setConnectionTimeout(CLIENT_TIMEOUT_S * 1000);

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

    while (sent < len && client.connected())
    {
      size_t chunk = (len - sent) > CHUNK_SIZE ? CHUNK_SIZE : (len - sent);
      size_t wrote = client.write(fb->buf + sent, chunk);
      if (wrote == 0)
      {
        client.stop();
        break;
      }
      sent += wrote;
      esp_task_wdt_reset();
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

void wifiLoop()
{
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiConnected)
    {
      wifiConnected = true;
      wifiDownSince = 0;
      wifiRetryDelay = 0;
      WiFi.setSleep(false);
      Serial.printf("Wi-Fi připojeno, IP: %s\n", WiFi.localIP().toString().c_str());
      diagCountWifiReconnect();
      otaTimeReset();
    }
    return;
  }

  if (wifiConnected)
  {
    wifiConnected = false;
    Serial.println("Wi-Fi spojení ztraceno, obnovuji na pozadí...");
  }

  if (wifiDownSince == 0)
  {
    wifiDownSince = now;
  }
  else if (now - wifiDownSince >= WIFI_DOWN_RESTART_MS)
  {
    Serial.printf("Wi-Fi nedostupná %lu s, restartuji...\n",
                  (unsigned long)((now - wifiDownSince) / 1000));
    ESP.restart();
  }

  if (!wifiStarted)
  {
    WiFi.mode(WIFI_STA);
    esp_sntp_servermode_dhcp(true);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WifiSSID, WifiPassword);
    wifiStarted = true;
    wifiLastAttempt = now;
    wifiRetryDelay = WIFI_RETRY_MIN_MS;
    Serial.println("Připojuji se na Wi-Fi...");
    return;
  }

  if (now - wifiLastAttempt < wifiRetryDelay)
  {
    return;
  }

  wifiLastAttempt = now;
  wifiRetryDelay = wifiRetryDelay * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : wifiRetryDelay * 2;
  Serial.printf("Wi-Fi nepřipojena (status %d), nový pokus, další za %lu s\n",
                (int)WiFi.status(), (unsigned long)(wifiRetryDelay / 1000));
  WiFi.disconnect();
  WiFi.begin(WifiSSID, WifiPassword);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  esp_task_wdt_config_t wdtCfg = {
     .timeout_ms = WDT_TIMEOUT_S * 1000,
     .idle_core_mask = 0,
     .trigger_panic = true,
  };
  if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE)
  {
    esp_task_wdt_reconfigure(&wdtCfg);
  }
   esp_task_wdt_add(NULL);

  bool camReady = initCamera();
  for (uint8_t attempt = 1; !camReady && attempt <= MAX_CAMERA_ERRORS; attempt++)
  {
    Serial.printf("Kamera se nepodařila inicializovat (pokus %d), reinicializuji senzor...\n", attempt);
    deInit();
    delay(500);
    camReady = initCamera();
  }

  if (camReady)
  {
    warmUp(3);
  }
  else
  {
    Serial.println("Kamera se nenahodila ani po opakování, pokračuji bez ní (diagnostika poběží).");
  }

  lastCaptureTime = millis() - interval;
}

void loop()
{
  uint32_t loopStart = millis();
  esp_task_wdt_reset();

  wifiLoop();

  if(WiFi.status() == WL_CONNECTED && otaTimeLoop())
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

    if (exposureRecalibrationDue())
    {
      runRecalibration();
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
