#include <Arduino.h>
#include <Preferences.h>
#include "camera.h"
#include "pin_config.h"
#include "esp_camera.h"
#include "esp_jpg_decode.h"
#include "esp_psram.h"

#define USE_LED_FLASH 1

const uint8_t EXPOSURE_TARGET_P90 = 220;
const uint8_t EXPOSURE_CONVERGE_TOLERANCE = 8;
const uint8_t EXPOSURE_CLIP_HIGH_MAX = 2;
const uint8_t EXPOSURE_MAX_STEPS = 8;
const uint8_t EXPOSURE_SETTLE_FRAMES = 2;
const uint16_t EXPOSURE_AEC_MIN = 4;
const uint16_t EXPOSURE_AEC_MAX = 1200;
const uint16_t EXPOSURE_AEC_START = 300;
const uint8_t EXPOSURE_AGC_MAX = 30;
const uint8_t EXPOSURE_AGC_STEP = 4;
const uint32_t EXPOSURE_TRIAL_MAGIC = 0x45585031;

const uint8_t QUALITY_BAND = 40;
const uint8_t QUALITY_CLIP_HIGH_MAX = 10;
const uint8_t QUALITY_CONTRAST_MIN = 25;
const uint8_t QUALITY_BAD_STREAK = 3;
const uint32_t RECALIBRATION_MIN_INTERVAL_MS = 60UL * 60UL * 1000UL;
const uint32_t RECALIBRATION_MAX_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;

const uint8_t ROI_X_FROM_PCT = 20;
const uint8_t ROI_X_TO_PCT = 80;
const uint8_t ROI_Y_FROM_PCT = 20;
const uint8_t ROI_Y_TO_PCT = 80;

const char* EXPOSURE_NS = "cam";
const char* EXPOSURE_KEY_AEC = "aec";
const char* EXPOSURE_KEY_AGC = "agc";

Preferences exposurePrefs;

RTC_NOINIT_ATTR uint32_t exposureTrial;
bool exposureTrialActive = false;

uint8_t badFrameStreak = 0;
uint8_t lastFrameError = 255;
bool recalibrationRequested = false;
bool recalibrationRan = false;
uint32_t lastRecalibration = 0;
uint32_t recalibrationInterval = RECALIBRATION_MIN_INTERVAL_MS;

struct HistogramCtx
{
  uint16_t bins[256];
  uint32_t pixels;
  uint16_t width;
  uint16_t height;
  uint16_t roiX0;
  uint16_t roiX1;
  uint16_t roiY0;
  uint16_t roiY1;
  const uint8_t* jpeg;
  size_t jpegLen;
};

size_t jpegStatsReader(void* arg, size_t index, uint8_t* buf, size_t len)
{
  HistogramCtx* ctx = (HistogramCtx*)arg;
  if (index >= ctx->jpegLen)
  {
    return 0;
  }
  size_t available = ctx->jpegLen - index;
  if (len > available)
  {
    len = available;
  }
  if (buf)
  {
    memcpy(buf, ctx->jpeg + index, len);
  }
  return len;
}

bool jpegStatsWriter(void* arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* data)
{
  HistogramCtx* ctx = (HistogramCtx*)arg;

  if (!data)
  {
    if (x == 0 && y == 0)
    {
      ctx->width = w;
      ctx->height = h;
      ctx->roiX0 = (uint16_t)((uint32_t)w * ROI_X_FROM_PCT / 100);
      ctx->roiX1 = (uint16_t)((uint32_t)w * ROI_X_TO_PCT / 100);
      ctx->roiY0 = (uint16_t)((uint32_t)h * ROI_Y_FROM_PCT / 100);
      ctx->roiY1 = (uint16_t)((uint32_t)h * ROI_Y_TO_PCT / 100);
    }
    return true;
  }

  for (uint16_t row = 0; row < h; row++)
  {
    uint16_t absY = y + row;
    if (absY < ctx->roiY0 || absY >= ctx->roiY1)
    {
      continue;
    }
    for (uint16_t col = 0; col < w; col++)
    {
      uint16_t absX = x + col;
      if (absX < ctx->roiX0 || absX >= ctx->roiX1)
      {
        continue;
      }
      const uint8_t* px = data + ((size_t)row * w + col) * 3;
      uint8_t luma = (uint8_t)((77 * px[0] + 150 * px[1] + 29 * px[2]) >> 8);
      ctx->bins[luma]++;
      ctx->pixels++;
    }
  }
  return true;
}

uint8_t histogramPercentile(const HistogramCtx* ctx, uint8_t percent)
{
  uint32_t target = (uint32_t)ctx->pixels * percent / 100;
  uint32_t seen = 0;
  for (uint16_t level = 0; level < 256; level++)
  {
    seen += ctx->bins[level];
    if (seen >= target)
    {
      return (uint8_t)level;
    }
  }
  return 255;
}

bool measureFrame(camera_fb_t* fb, FrameStats* stats)
{
  memset(stats, 0, sizeof(FrameStats));
  if (!fb || fb->format != PIXFORMAT_JPEG)
  {
    return false;
  }

  HistogramCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.jpeg = fb->buf;
  ctx.jpegLen = fb->len;

  if (esp_jpg_decode(fb->len, JPG_SCALE_8X, jpegStatsReader, jpegStatsWriter, &ctx) != ESP_OK)
  {
    return false;
  }
  if (ctx.pixels == 0)
  {
    return false;
  }

  uint64_t sum = 0;
  uint32_t dark = 0;
  uint32_t bright = 0;
  for (uint16_t level = 0; level < 256; level++)
  {
    sum += (uint64_t)ctx.bins[level] * level;
    if (level <= 8)
    {
      dark += ctx.bins[level];
    }
    if (level >= 247)
    {
      bright += ctx.bins[level];
    }
  }

  stats->mean = (uint8_t)(sum / ctx.pixels);
  stats->p10 = histogramPercentile(&ctx, 10);
  stats->p90 = histogramPercentile(&ctx, 90);
  stats->contrast = (uint8_t)(stats->p90 - stats->p10);
  stats->clipLow = (uint8_t)((uint64_t)dark * 100 / ctx.pixels);
  stats->clipHigh = (uint8_t)((uint64_t)bright * 100 / ctx.pixels);
  stats->valid = true;
  return true;
}

bool measureNextFrame(FrameStats* stats)
{
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb)
  {
    return false;
  }
  bool ok = measureFrame(fb, stats);
  esp_camera_fb_return(fb);
  return ok;
}

void dropFrames(uint8_t frames)
{
  for (uint8_t i = 0; i < frames; i++)
  {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb)
    {
      esp_camera_fb_return(fb);
    }
  }
}

void lockCameraSettings(sensor_t *s)
{
  s->set_whitebal(s, 0);
  s->set_awb_gain(s, 0);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
}

void printSensorValues(sensor_t *s) 
{
  auto st = s->status;

  Serial.println(F("=== Camera Sensor Status ==="));
  Serial.printf("framesize: %d\n", st.framesize);
  Serial.printf("scale: %d\n", st.scale);
  Serial.printf("binning: %d\n", st.binning);
  Serial.printf("quality: %d\n", st.quality);
  Serial.printf("brightness: %d\n", st.brightness);
  Serial.printf("contrast: %d\n", st.contrast);
  Serial.printf("saturation: %d\n", st.saturation);
  Serial.printf("sharpness: %d\n", st.sharpness);
  Serial.printf("denoise: %d\n", st.denoise);
  Serial.printf("special_effect: %d\n", st.special_effect);
  Serial.printf("wb_mode: %d\n", st.wb_mode);
  Serial.printf("awb: %d\n", st.awb);
  Serial.printf("awb_gain: %d\n", st.awb_gain);
  Serial.printf("aec: %d\n", st.aec);
  Serial.printf("aec2: %d\n", st.aec2);
  Serial.printf("ae_level: %d\n", st.ae_level);
  Serial.printf("aec_value: %u\n", st.aec_value);
  Serial.printf("agc: %d\n", st.agc);
  Serial.printf("agc_gain: %u\n", st.agc_gain);
  Serial.printf("gainceiling: %u\n", st.gainceiling);
  Serial.printf("bpc: %d\n", st.bpc);
  Serial.printf("wpc: %d\n", st.wpc);
  Serial.printf("raw_gma: %d\n", st.raw_gma);
  Serial.printf("lenc: %d\n", st.lenc);
  Serial.printf("hmirror: %d\n", st.hmirror);
  Serial.printf("vflip: %d\n", st.vflip);
  Serial.printf("dcw: %d\n", st.dcw);
  Serial.printf("colorbar: %d\n", st.colorbar);
  Serial.println(F("============================"));
}

void setupLedFlash()
{
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
}

void ledFlashOn()
{
  digitalWrite(LED_GPIO_NUM, HIGH);
  delay(100);
}

void ledFlashOff()
{
  delay(100);
  digitalWrite(LED_GPIO_NUM, LOW);
}

bool deInit()
{
  esp_err_t result = esp_camera_deinit();
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(200);
  return result == ESP_OK;
}

bool initCamera()
{
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(200);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(200);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 2;
  config.jpeg_quality = 4;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound())
  {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.printf("PSRAM OK, size = %u bytes\n", esp_psram_get_size());
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) 
  {
    Serial.printf("Kamera se nepodařila inicializovat (chyba 0x%x)\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) 
  {
    printSensorValues(s);
    lockCameraSettings(s);
  }

  #if defined(LED_GPIO_NUM)
    setupLedFlash();
  #endif

  setupExposure();

  Serial.println("Kamera inicializována.");
  return true;
}

bool exposureManualSupported(sensor_t *s)
{
  return s->set_exposure_ctrl && s->set_gain_ctrl && s->set_aec_value && s->set_agc_gain;
}

bool exposureValuesSane(uint16_t aec, uint8_t agc)
{
  return aec > 0 && aec <= EXPOSURE_AEC_MAX && agc <= EXPOSURE_AGC_MAX;
}

uint16_t clampAec(uint32_t value)
{
  if (value < EXPOSURE_AEC_MIN)
  {
    return EXPOSURE_AEC_MIN;
  }
  if (value > EXPOSURE_AEC_MAX)
  {
    return EXPOSURE_AEC_MAX;
  }
  return (uint16_t)value;
}

uint8_t targetError(const FrameStats* stats)
{
  uint8_t error = stats->p90 > EXPOSURE_TARGET_P90
    ? stats->p90 - EXPOSURE_TARGET_P90
    : EXPOSURE_TARGET_P90 - stats->p90;
  if (stats->clipHigh > EXPOSURE_CLIP_HIGH_MAX)
  {
    uint16_t penalty = (uint16_t)error + (stats->clipHigh - EXPOSURE_CLIP_HIGH_MAX) * 10;
    return penalty > 254 ? 254 : (uint8_t)penalty;
  }
  return error;
}

bool calibrateExposure(uint8_t* achievedError)
{
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !exposureManualSupported(s))
  {
    Serial.println("Kamera: senzor neumí ruční expozici, kalibrace vynechána.");
    return false;
  }

  uint16_t aec = exposureValuesSane((uint16_t)s->status.aec_value, 0)
    ? (uint16_t)s->status.aec_value
    : EXPOSURE_AEC_START;
  uint8_t agc = 0;

  uint16_t bestAec = aec;
  uint8_t bestAgc = agc;
  uint8_t bestError = 255;

  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOn();
  #endif

  s->set_exposure_ctrl(s, 0);
  s->set_gain_ctrl(s, 0);

  FrameStats stats;
  for (uint8_t step = 0; step < EXPOSURE_MAX_STEPS; step++)
  {
    s->set_aec_value(s, aec);
    s->set_agc_gain(s, agc);
    dropFrames(EXPOSURE_SETTLE_FRAMES);

    if (!measureNextFrame(&stats))
    {
      Serial.println("Kamera: kalibrace - snímek se nepodařilo změřit.");
      break;
    }

    uint8_t error = targetError(&stats);
    Serial.printf("Kamera: kalibrace krok %u aec=%u agc=%u -> p90=%u mean=%u kontrast=%u přepal=%u%% černá=%u%%\n",
                  (unsigned)(step + 1), (unsigned)aec, (unsigned)agc,
                  (unsigned)stats.p90, (unsigned)stats.mean, (unsigned)stats.contrast,
                  (unsigned)stats.clipHigh, (unsigned)stats.clipLow);

    if (error < bestError)
    {
      bestError = error;
      bestAec = aec;
      bestAgc = agc;
    }
    if (error <= EXPOSURE_CONVERGE_TOLERANCE)
    {
      break;
    }

    uint32_t proposed;
    if (stats.clipHigh > EXPOSURE_CLIP_HIGH_MAX)
    {
      proposed = (uint32_t)aec * 3 / 4;
    }
    else
    {
      proposed = (uint32_t)aec * EXPOSURE_TARGET_P90 / (stats.p90 > 0 ? stats.p90 : 1);
    }
    if (proposed > (uint32_t)aec * 4)
    {
      proposed = (uint32_t)aec * 4;
    }
    if (proposed < (uint32_t)aec / 4)
    {
      proposed = (uint32_t)aec / 4;
    }
    uint16_t next = clampAec(proposed);

    if (next == aec)
    {
      if (stats.p90 < EXPOSURE_TARGET_P90 && agc + EXPOSURE_AGC_STEP <= EXPOSURE_AGC_MAX)
      {
        agc += EXPOSURE_AGC_STEP;
        continue;
      }
      break;
    }
    aec = next;
  }

  s->set_aec_value(s, bestAec);
  s->set_agc_gain(s, bestAgc);
  dropFrames(EXPOSURE_SETTLE_FRAMES);

  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOff();
  #endif

  if (achievedError)
  {
    *achievedError = bestError;
  }

  if (bestError == 255)
  {
    Serial.println("Kamera: kalibrace neuspěla, expozice zůstává nezměněna.");
    return false;
  }

  Serial.printf("Kamera: expozice nastavena (aec_value=%u agc_gain=%u, cíl p90=%u, odchylka %u).\n",
                (unsigned)bestAec, (unsigned)bestAgc,
                (unsigned)EXPOSURE_TARGET_P90, (unsigned)bestError);
  return true;
}

void forgetStoredExposure()
{
  if (!exposurePrefs.begin(EXPOSURE_NS, false))
  {
    return;
  }
  exposurePrefs.remove(EXPOSURE_KEY_AEC);
  exposurePrefs.remove(EXPOSURE_KEY_AGC);
  exposurePrefs.end();
}

void confirmStoredExposure()
{
  if (!exposureTrialActive)
  {
    return;
  }
  exposureTrialActive = false;
  exposureTrial = 0;
  Serial.println("Kamera: uložená expozice potvrzena, snímek se čte.");
}

bool applyStoredExposure(sensor_t *s)
{
  if (!exposureManualSupported(s))
  {
    Serial.println("Kamera: senzor neumí ruční expozici, kalibruji.");
    return false;
  }

  if (exposureTrial == EXPOSURE_TRIAL_MAGIC)
  {
    exposureTrial = 0;
    Serial.println("Kamera: uložená expozice po restartu nepotvrzena, zahazuji ji a kalibruji.");
    forgetStoredExposure();
    return false;
  }

  if (!exposurePrefs.begin(EXPOSURE_NS, true))
  {
    return false;
  }
  bool stored = exposurePrefs.isKey(EXPOSURE_KEY_AEC) && exposurePrefs.isKey(EXPOSURE_KEY_AGC);
  uint16_t aec = exposurePrefs.getUShort(EXPOSURE_KEY_AEC, 0);
  uint8_t agc = exposurePrefs.getUChar(EXPOSURE_KEY_AGC, 0);
  exposurePrefs.end();

  if (!stored)
  {
    return false;
  }
  if (!exposureValuesSane(aec, agc))
  {
    Serial.printf("Kamera: uložená expozice mimo rozsah (aec_value=%u agc_gain=%u), zahazuji ji.\n",
                  (unsigned)aec, (unsigned)agc);
    forgetStoredExposure();
    return false;
  }

  exposureTrial = EXPOSURE_TRIAL_MAGIC;
  exposureTrialActive = true;

  s->set_exposure_ctrl(s, 0);
  s->set_gain_ctrl(s, 0);
  s->set_aec_value(s, aec);
  s->set_agc_gain(s, agc);
  Serial.printf("Kamera: expozice z NVS (aec_value=%u agc_gain=%u), kalibrace přeskočena.\n",
                (unsigned)aec, (unsigned)agc);
  return true;
}

void storeExposure(sensor_t *s)
{
  if (!exposureManualSupported(s))
  {
    return;
  }
  if (!exposureValuesSane((uint16_t)s->status.aec_value, (uint8_t)s->status.agc_gain))
  {
    Serial.printf("Kamera: expozice mimo rozsah (aec_value=%u agc_gain=%u), neukládám.\n",
                  (unsigned)s->status.aec_value, (unsigned)s->status.agc_gain);
    return;
  }
  if (!exposurePrefs.begin(EXPOSURE_NS, false))
  {
    Serial.println("Kamera: NVS se nepodařilo otevřít, expozice se neuloží.");
    return;
  }
  exposurePrefs.putUShort(EXPOSURE_KEY_AEC, (uint16_t)s->status.aec_value);
  exposurePrefs.putUChar(EXPOSURE_KEY_AGC, (uint8_t)s->status.agc_gain);
  exposurePrefs.end();
  Serial.printf("Kamera: expozice uložena do NVS (aec_value=%u agc_gain=%u).\n",
                (unsigned)s->status.aec_value, (unsigned)s->status.agc_gain);
}

void setupExposure()
{
  sensor_t *s = esp_camera_sensor_get();
  if (!s)
  {
    return;
  }
  if (applyStoredExposure(s))
  {
    return;
  }
  uint8_t achieved = 255;
  if (calibrateExposure(&achieved))
  {
    storeExposure(s);
    lastFrameError = achieved;
  }
  lastRecalibration = millis();
  recalibrationRan = true;
}

void noteFrameStats(const FrameStats* stats)
{
  if (!stats || !stats->valid)
  {
    return;
  }

  lastFrameError = targetError(stats);
  Serial.printf("Kvalita snímku: p90=%u (cíl %u) p10=%u mean=%u kontrast=%u přepal=%u%% černá=%u%%\n",
                (unsigned)stats->p90, (unsigned)EXPOSURE_TARGET_P90, (unsigned)stats->p10,
                (unsigned)stats->mean, (unsigned)stats->contrast,
                (unsigned)stats->clipHigh, (unsigned)stats->clipLow);

  if (stats->contrast < QUALITY_CONTRAST_MIN)
  {
    Serial.printf("Kvalita snímku: nízký kontrast (%u), expozicí se to nespraví - zkontroluj optiku.\n",
                  (unsigned)stats->contrast);
  }

  bool exposureBad = lastFrameError > QUALITY_BAND || stats->clipHigh > QUALITY_CLIP_HIGH_MAX;
  badFrameStreak = exposureBad ? (uint8_t)(badFrameStreak + 1) : 0;

  if (badFrameStreak >= QUALITY_BAD_STREAK)
  {
    badFrameStreak = 0;
    recalibrationRequested = true;
    Serial.println("Kvalita snímku: expozice mimo pásmo, žádám rekalibraci.");
  }
}

bool exposureRecalibrationDue()
{
  if (!recalibrationRequested)
  {
    return false;
  }
  if (!recalibrationRan)
  {
    return true;
  }
  return millis() - lastRecalibration >= recalibrationInterval;
}

void runRecalibration()
{
  recalibrationRequested = false;
  recalibrationRan = true;
  lastRecalibration = millis();

  sensor_t *s = esp_camera_sensor_get();
  if (!s || !exposureManualSupported(s))
  {
    return;
  }

  uint16_t prevAec = (uint16_t)s->status.aec_value;
  uint8_t prevAgc = (uint8_t)s->status.agc_gain;
  uint8_t prevError = lastFrameError;

  uint8_t achieved = 255;
  bool ok = calibrateExposure(&achieved);

  if (!ok || achieved >= prevError)
  {
    Serial.printf("Kamera: rekalibrace nepomohla (odchylka %u vs %u), vracím předchozí expozici.\n",
                  (unsigned)achieved, (unsigned)prevError);
    s->set_aec_value(s, prevAec);
    s->set_agc_gain(s, prevAgc);
    dropFrames(EXPOSURE_SETTLE_FRAMES);
    uint32_t next = recalibrationInterval * 6;
    recalibrationInterval = next > RECALIBRATION_MAX_INTERVAL_MS ? RECALIBRATION_MAX_INTERVAL_MS : next;
    Serial.printf("Kamera: další rekalibrace nejdřív za %lu min.\n",
                  (unsigned long)(recalibrationInterval / 60000));
    return;
  }

  recalibrationInterval = RECALIBRATION_MIN_INTERVAL_MS;
  lastFrameError = achieved;
  storeExposure(s);
}

camera_fb_t* capture()
{
  sensor_t *s = esp_camera_sensor_get();

  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOn();
  #endif

  camera_fb_t* flush = esp_camera_fb_get();
  if (flush)
  {
    esp_camera_fb_return(flush);
  }

  unsigned long t = micros();
  camera_fb_t* fb = esp_camera_fb_get();
  Serial.print("Time: ");
  Serial.println(micros() - t);
  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOff();
  #endif
  if (s)
  {
    printSensorValues(s);
  }
  if (!fb)
  {
    Serial.println("capture() failed - no framebuffer");
  }
  return fb;
}

void returnFb(camera_fb_t* fb)
{
  if (!fb)
  {
    return;
  }
  esp_camera_fb_return(fb);
}

void warmUp(uint8_t frames)
{
  bool gotFrame = false;
  for (uint8_t i = 0; i < frames; i++)
  {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb)
    {
      gotFrame = true;
      esp_camera_fb_return(fb);
    }
    delay(200);
  }
  if (gotFrame)
  {
    confirmStoredExposure();
  }
  Serial.printf("Kamera: warm-up %u snimku hotovo.\n", frames);
}
