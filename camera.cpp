#include <Arduino.h>
#include "camera.h"
#define CAMERA_MODEL_AI_THINKER
#include "pin_config.h"
#include "esp_camera.h"
#include "esp_psram.h"

static void lockCameraSettings(sensor_t *s)
{
  s->set_sharpness(s, 1);
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

  camera_config_t config;
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
  config.fb_count = 1;
  config.jpeg_quality = 5;
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

  Serial.println("Kamera inicializována.");
  return true;
}

camera_fb_t* capture()
{
  #if defined(LED_GPIO_NUM)
    ledFlashOn();
  #endif
  unsigned long t = micros();
  camera_fb_t* fb = esp_camera_fb_get();
  Serial.print("Time: ");
  Serial.println(micros() - t);
  #if defined(LED_GPIO_NUM)
    ledFlashOff();
  #endif
  sensor_t *s = esp_camera_sensor_get();
  printSensorValues(s);
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
