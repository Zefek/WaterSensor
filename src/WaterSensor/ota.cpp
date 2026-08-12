#include "ota.h"
#include "diagnostics.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>
#include "config.h"

#ifndef OTA_NTP_SERVER
#define OTA_NTP_SERVER "pool.ntp.org"
#endif

char ntpFromDhcp[16] = "";

#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS (60UL * 60UL * 1000UL)
#endif

#ifndef FW_VERSION
#define FW_VERSION 0
#endif

const time_t TIME_VALID_THRESHOLD = 1700000000;
const uint32_t TIME_SYNC_WAIT_MS = 15000;
const uint32_t TIME_RETRY_MIN_MS = 30000;
const uint32_t TIME_RETRY_MAX_MS = 300000;

uint32_t timeLastTry = 0;
uint32_t timeRetryDelay = 0;

static bool syncTime()
{
  const ip_addr_t* dhcpServer = esp_sntp_getserver(0);
  if (dhcpServer != NULL && !ip_addr_isany_val(*dhcpServer))
  {
    snprintf(ntpFromDhcp, sizeof(ntpFromDhcp), "%s", ipaddr_ntoa(dhcpServer));
  }
  if (ntpFromDhcp[0] != '\0')
  {
    Serial.printf("OTA: NTP z DHCP: %s\n", ntpFromDhcp);
    configTime(0, 0, ntpFromDhcp, OTA_NTP_SERVER);
  }
  else
  {
    configTime(0, 0, OTA_NTP_SERVER);
  }
  Serial.print("OTA: synchronizuji cas (NTP) ...");
  time_t now = 0;
  uint32_t start = millis();
  while (now < TIME_VALID_THRESHOLD)
  {
    if (millis() - start > TIME_SYNC_WAIT_MS)
    {
      Serial.println(" timeout (cert se nemusi overit).");
      return false;
    }
    esp_task_wdt_reset();
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

  HTTPUpdate updater(30000);
  updater.setAuthorization(OtaUser, OtaPassword);

  updater.onStart([]() { esp_task_wdt_reset(); Serial.println("OTA: start"); });
  updater.onEnd([]() { esp_task_wdt_reset(); Serial.println("OTA: hotovo"); });
  updater.onProgress([](int cur, int total) {
    esp_task_wdt_reset();
    Serial.printf("OTA: prubeh %d/%d B (%d%%)\r", cur, total,
                  total ? (cur * 100 / total) : 0);
  });
  updater.onError([](int err)  { Serial.printf("\nOTA: chyba %d\n", err); });

  updater.rebootOnUpdate(true);

  t_httpUpdate_return ret = updater.update(client, OtaUrl, String((int)FW_VERSION));

  switch (ret)
  {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA: SELHALA (%d): %s\n",
                    updater.getLastError(),
                    updater.getLastErrorString().c_str());
      diagCountOtaFailure();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: zadna nova aktualizace (firmware je aktualni).");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: OK.");
      break;
  }
}

void otaTimeReset()
{
  timeRetryDelay = 0;
}

bool otaTimeLoop()
{
  time_t now = 0;
  time(&now);
  if (now >= TIME_VALID_THRESHOLD)
  {
    timeRetryDelay = 0;
    return true;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  uint32_t millisNow = millis();
  if (timeRetryDelay != 0 && millisNow - timeLastTry < timeRetryDelay)
  {
    return false;
  }
  timeLastTry = millisNow;

  if (syncTime())
  {
    timeRetryDelay = 0;
    return true;
  }

  timeRetryDelay = timeRetryDelay == 0
    ? TIME_RETRY_MIN_MS
    : (timeRetryDelay * 2 > TIME_RETRY_MAX_MS ? TIME_RETRY_MAX_MS : timeRetryDelay * 2);
  Serial.printf("OTA: cas nesynchronizovan, odesilani pozastaveno, dalsi pokus za %lu s\n",
                (unsigned long)(timeRetryDelay / 1000));
  return false;
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
