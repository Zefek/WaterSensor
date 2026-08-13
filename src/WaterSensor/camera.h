#define CAMERA_MODEL_AI_THINKER
#include "pin_config.h"
#include "esp_camera.h"

struct FrameStats
{
  uint8_t mean;
  uint8_t p10;
  uint8_t p90;
  uint8_t contrast;
  uint8_t clipLow;
  uint8_t clipHigh;
  bool valid;
};

void setupLedFlash();
void ledFlashOn();
void ledFlashOff();
bool initCamera();
bool deInit();
bool calibrateExposure(uint8_t* achievedError);
void setupExposure();
bool measureFrame(camera_fb_t* fb, FrameStats* stats);
void noteFrameStats(const FrameStats* stats);
bool exposureRecalibrationDue();
void runRecalibration();
camera_fb_t* capture();
void returnFb(camera_fb_t* fb);
void warmUp(uint8_t frames = 3);
