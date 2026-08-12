#include <Arduino.h>
#include <Preferences.h>
#include "camera.h"
#include "pin_config.h"
#include "esp_camera.h"
#include "esp_psram.h"

#define USE_LED_FLASH 1

const uint8_t AE_CONVERGE_FRAMES = 10;
const uint16_t AE_CONVERGE_DELAY_MS = 100;

const uint16_t EXPOSURE_AEC_MAX = 1200;
const uint8_t EXPOSURE_AGC_MAX = 30;
const uint32_t EXPOSURE_TRIAL_MAGIC = 0x45585031;

const char* EXPOSURE_NS = "cam";
const char* EXPOSURE_KEY_AEC = "aec";
const char* EXPOSURE_KEY_AGC = "agc";

Preferences exposurePrefs;

RTC_NOINIT_ATTR uint32_t exposureTrial;
bool exposureTrialActive = false;

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

void calibrateExposure()
{
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !s->set_exposure_ctrl || !s->set_gain_ctrl)
  {
    return;
  }

  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOn();
  #endif

  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
  for (uint8_t i = 0; i < AE_CONVERGE_FRAMES; i++)
  {
    camera_fb_t* tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(AE_CONVERGE_DELAY_MS);
  }
  s->set_exposure_ctrl(s, 0);
  s->set_gain_ctrl(s, 0);

  camera_fb_t* flush = esp_camera_fb_get();
  if (flush) esp_camera_fb_return(flush);

  #if defined(LED_GPIO_NUM) && USE_LED_FLASH
    ledFlashOff();
  #endif

  if (s->init_status)
  {
    s->init_status(s);
  }

  Serial.printf("Kamera: expozice zafixována (aec_value=%u agc_gain=%u).\n",
                (unsigned)s->status.aec_value, (unsigned)s->status.agc_gain);
}

bool exposureManualSupported(sensor_t *s)
{
  return s->set_exposure_ctrl && s->set_gain_ctrl && s->set_aec_value && s->set_agc_gain;
}

bool exposureValuesSane(uint16_t aec, uint8_t agc)
{
  return aec > 0 && aec <= EXPOSURE_AEC_MAX && agc <= EXPOSURE_AGC_MAX;
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
  calibrateExposure();
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
