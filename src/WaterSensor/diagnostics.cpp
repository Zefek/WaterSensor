#include "diagnostics.h"
#include <esp_random.h>
#include <esp_system.h>
#include <WiFi.h>
#include "esp_camera.h"
#include <string.h>

#ifndef FW_VERSION
#define FW_VERSION 0
#endif

#define TRANSFER_SCHEMA_VER 1
#define DEVICE_SCHEMA_VER   1
#define CONFIG_SCHEMA_VER   1

typedef struct __attribute__((packed)) {
  uint8_t  schema_ver;
  uint64_t corr_id;
  uint32_t img_size;
  uint32_t bytes_sent;
  uint32_t duration_ms;
  uint8_t  try_count;
  uint8_t  success;
  int16_t  http_code;
  int8_t   rssi;
} transfer_t;                 // 26 B
static_assert(sizeof(transfer_t) == 26, "transfer_t layout");

typedef struct __attribute__((packed)) {
  uint8_t  schema_ver;
  uint32_t uptime_s;
  uint16_t fw_version;
  uint16_t free_heap_kb;
  uint16_t min_free_heap_kb;
  uint16_t max_alloc_kb;
  uint16_t wifi_reconnects;
  uint16_t capture_count;
  uint16_t send_failures;
  uint16_t tls_errors;
  uint16_t ota_failures;
  uint16_t loop_max_ms;
  uint8_t  camera_errors;
  uint8_t  reset_reason;
  int8_t   rssi;
  uint8_t  cfg_hash;
} device_t;                   // 29 B
static_assert(sizeof(device_t) == 29, "device_t layout");

typedef struct __attribute__((packed)) {
  uint8_t  schema_ver;
  uint16_t fw_version;
  uint8_t  framesize;
  uint8_t  quality;
  uint16_t aec_value;
  uint8_t  exposure_ctrl;   // status.aec
  uint8_t  gain_ctrl;       // status.agc
  uint8_t  agc_gain;
  int8_t   ae_level;
  int8_t   brightness;
  int8_t   contrast;
  int8_t   saturation;
  uint8_t  whitebal;        // status.awb
  uint8_t  awb_gain;
  uint8_t  wb_mode;
  uint8_t  special_effect;
  uint8_t  hmirror;
  uint8_t  vflip;
  uint8_t  cfg_hash;        // CRC8 přes bajty [0..19]
} config_t;                  // 21 B
static_assert(sizeof(config_t) == 21, "config_t layout");

static uint32_t   s_bootId = 0;
static uint32_t   s_seq = 0;
static bool       s_hasPrev = false;
static transfer_t s_prev;

static uint16_t s_wifiReconnects = 0, s_captureCount = 0, s_sendFailures = 0,
                s_tlsErrors = 0, s_otaFailures = 0, s_cameraErrors = 0;
static uint16_t s_loopMaxMs = 0;

static bool     s_cfgSent = false;
static uint8_t  s_cfgSentHash = 0;
static uint8_t  s_cfgHashCache = 0;

static const char HEXCHARS[] = "0123456789abcdef";

static uint8_t crc8(const uint8_t* data, size_t len)
{
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static uint32_t bootId()
{
  if (s_bootId == 0)
  {
    s_bootId = esp_random();
    if (s_bootId == 0) s_bootId = 1;   // 0 vyhrazená pro "neinicializováno"
  }
  return s_bootId;
}

static uint16_t toKb(uint32_t bytes) { return (uint16_t)(bytes / 1024); }

static uint8_t buildConfig(config_t* c)
{
  memset(c, 0, sizeof(config_t));
  c->schema_ver = CONFIG_SCHEMA_VER;
  c->fw_version = (uint16_t)FW_VERSION;
  sensor_t* s = esp_camera_sensor_get();
  if (s)
  {
    c->framesize      = (uint8_t)s->status.framesize;
    c->quality        = (uint8_t)s->status.quality;
    c->aec_value      = (uint16_t)s->status.aec_value;
    c->exposure_ctrl  = (uint8_t)s->status.aec;
    c->gain_ctrl      = (uint8_t)s->status.agc;
    c->agc_gain       = (uint8_t)s->status.agc_gain;
    c->ae_level       = (int8_t)s->status.ae_level;
    c->brightness     = (int8_t)s->status.brightness;
    c->contrast       = (int8_t)s->status.contrast;
    c->saturation     = (int8_t)s->status.saturation;
    c->whitebal       = (uint8_t)s->status.awb;
    c->awb_gain       = (uint8_t)s->status.awb_gain;
    c->wb_mode        = (uint8_t)s->status.wb_mode;
    c->special_effect = (uint8_t)s->status.special_effect;
    c->hmirror        = (uint8_t)s->status.hmirror;
    c->vflip          = (uint8_t)s->status.vflip;
  }
  c->cfg_hash = crc8((const uint8_t*)c, sizeof(config_t) - 1);
  s_cfgHashCache = c->cfg_hash;
  return c->cfg_hash;
}

uint64_t diagNextCorrelationId()
{
  uint32_t seq = ++s_seq;
  return ((uint64_t)bootId() << 32) | seq;
}

void diagRecordTransfer(uint64_t corrId, uint32_t imgSize, uint32_t bytesSent,
                        uint32_t durationMs, uint8_t tryCount, bool success,
                        int16_t httpCode, int8_t rssi)
{
  s_prev.schema_ver  = TRANSFER_SCHEMA_VER;
  s_prev.corr_id     = corrId;
  s_prev.img_size    = imgSize;
  s_prev.bytes_sent  = bytesSent;
  s_prev.duration_ms = durationMs;
  s_prev.try_count   = tryCount;
  s_prev.success     = success ? 1 : 0;
  s_prev.http_code   = httpCode;
  s_prev.rssi        = rssi;
  s_hasPrev = true;
}

bool diagPrevTransferHex(char* hexbuf, size_t buflen)
{
  if (!s_hasPrev) return false;
  if (buflen < sizeof(transfer_t) * 2 + 1) return false;
  const uint8_t* p = (const uint8_t*)&s_prev;
  for (size_t i = 0; i < sizeof(transfer_t); i++)
  {
    hexbuf[i * 2]     = HEXCHARS[p[i] >> 4];
    hexbuf[i * 2 + 1] = HEXCHARS[p[i] & 0x0F];
  }
  hexbuf[sizeof(transfer_t) * 2] = '\0';
  return true;
}

void diagCorrelationHex(uint64_t corrId, char* hexbuf, size_t buflen)
{
  if (buflen < DIAG_CORR_HEX_BUF)
  {
    if (buflen) hexbuf[0] = '\0';
    return;
  }
  for (int i = 0; i < 8; i++)
  {
    uint8_t b = (uint8_t)(corrId >> (56 - i * 8));   // big-endian hex hodnoty
    hexbuf[i * 2]     = HEXCHARS[b >> 4];
    hexbuf[i * 2 + 1] = HEXCHARS[b & 0x0F];
  }
  hexbuf[16] = '\0';
}

void diagCountWifiReconnect() { s_wifiReconnects++; }
void diagCountCapture()       { s_captureCount++; }
void diagCountSendFailure()   { s_sendFailures++; }
void diagCountTlsError()      { s_tlsErrors++; }
void diagCountOtaFailure()    { s_otaFailures++; }
void diagCountCameraError()   { s_cameraErrors++; }
void diagNoteLoopMs(uint32_t ms)
{
  if (ms > s_loopMaxMs) s_loopMaxMs = (ms > 65535) ? 65535 : (uint16_t)ms;
}

size_t diagBuildDeviceBlob(uint8_t* buf, size_t buflen)
{
  if (buflen < sizeof(device_t)) return 0;
  device_t d;
  d.schema_ver       = DEVICE_SCHEMA_VER;
  d.uptime_s         = (uint32_t)(millis() / 1000);
  d.fw_version       = (uint16_t)FW_VERSION;
  d.free_heap_kb     = toKb(ESP.getFreeHeap());
  d.min_free_heap_kb = toKb(ESP.getMinFreeHeap());
  d.max_alloc_kb     = toKb(ESP.getMaxAllocHeap());
  d.wifi_reconnects  = s_wifiReconnects;
  d.capture_count    = s_captureCount;
  d.send_failures    = s_sendFailures;
  d.tls_errors       = s_tlsErrors;
  d.ota_failures     = s_otaFailures;
  d.loop_max_ms      = s_loopMaxMs;
  d.camera_errors    = (uint8_t)(s_cameraErrors > 255 ? 255 : s_cameraErrors);
  d.reset_reason     = (uint8_t)esp_reset_reason();
  d.rssi             = (int8_t)WiFi.RSSI();
  config_t tmp; d.cfg_hash = buildConfig(&tmp);   // čerstvý cfg_hash
  memcpy(buf, &d, sizeof(device_t));
  return sizeof(device_t);
}

size_t diagBuildConfigBlob(uint8_t* buf, size_t buflen)
{
  if (buflen < sizeof(config_t)) return 0;
  config_t c;
  buildConfig(&c);
  memcpy(buf, &c, sizeof(config_t));
  return sizeof(config_t);
}

bool diagConfigChanged()
{
  config_t c;
  uint8_t h = buildConfig(&c);
  return !s_cfgSent || h != s_cfgSentHash;
}

void diagMarkConfigSent()
{
  s_cfgSent = true;
  s_cfgSentHash = s_cfgHashCache;
}
