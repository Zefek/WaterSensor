#include "ota.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include "config.h"

#ifndef OTA_NTP_SERVER
#define OTA_NTP_SERVER "pool.ntp.org"
#endif

#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS (60UL * 60UL * 1000UL)
#endif

#ifndef FW_VERSION
#define FW_VERSION 0
#endif

static bool syncTime()
{
  configTime(0, 0, OTA_NTP_SERVER);
  Serial.print("OTA: synchronizuji cas (NTP) ...");
  time_t now = 0;
  uint32_t start = millis();
  while (now < 1700000000)
  {
    if (millis() - start > 15000)
    {
      Serial.println(" timeout (cert se nemusi overit).");
      return false;
    }
    delay(250);
    Serial.print(".");
    time(&now);
  }
  Serial.printf(" OK (%ld)\n", (long)now);
  return true;
}

static void doOTA()
{
  Serial.printf("OTA: kontrola z %s (aktualni verze %d)\n", OtaUrl, (int)FW_VERSION);
  WiFiClientSecure client;
  client.setCACert(RootCA);
  httpUpdate.setAuthorization(OtaUser, OtaPassword);

  httpUpdate.onStart([]() { Serial.println("OTA: start"); });
  httpUpdate.onEnd([]() { Serial.println("OTA: hotovo"); });
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("OTA: prubeh %d/%d B (%d%%)\r", cur, total,
                  total ? (cur * 100 / total) : 0);
  });
  httpUpdate.onError([](int err)  { Serial.printf("\nOTA: chyba %d\n", err); });

  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = httpUpdate.update(client, OtaUrl, String((int)FW_VERSION));

  switch (ret)
  {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA: SELHALA (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: zadna nova aktualizace (firmware je aktualni).");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: OK.");
      break;
  }
}

void otaBegin()
{
  static bool timeSynced = false;
  if (!timeSynced && WiFi.status() == WL_CONNECTED)
  {
    timeSynced = syncTime();
  }
}

void otaLoop()
{
  static uint32_t lastCheck = 0;
  static bool firstRun = true;
  if (firstRun || (millis() - lastCheck >= OTA_CHECK_INTERVAL_MS))
  {
    firstRun = false;
    lastCheck = millis();
    if (WiFi.status() == WL_CONNECTED)
    {
      doOTA();
    }
  }
}
