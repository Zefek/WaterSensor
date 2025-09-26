#include "camera.h"
#define CAMERA_MODEL_AI_THINKER
#include "pin_config.h"
#include "esp_camera.h"
#include <Arduino.h>

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
  digitalWrite(LED_GPIO_NUM, LOW); 
}
// 🧠 Inicializace kamery
bool initCamera() 
{
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
  config.xclk_freq_hz = 10000000;
  config.frame_size = FRAMESIZE_XGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 8;
  config.fb_count = 2;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) 
  {
    if (psramFound()) 
    {
      config.jpeg_quality = 8;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else 
    {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_XGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else 
  {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
  }
  // Inicializace
  if (esp_camera_init(&config) != ESP_OK) 
  {
    Serial.println("❌ Kamera se nepodařila inicializovat");
    return false;
  }
  #if defined(LED_GPIO_NUM)
    setupLedFlash();
  #endif
  return true;
}

camera_fb_t* capture() 
{
  #if defined(LED_GPIO_NUM)
    ledFlashOn();
  #endif
  camera_fb_t* fb = esp_camera_fb_get();
  #if defined(LED_GPIO_NUM)
    ledFlashOff();
  #endif
  return fb;
}

void returnFb(camera_fb_t* fb)
{
  esp_camera_fb_return(fb);
}