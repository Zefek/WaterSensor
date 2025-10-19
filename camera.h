#define CAMERA_MODEL_AI_THINKER
#include "pin_config.h"
#include "esp_camera.h"

void setupLedFlash();
void ledFlashOn();
void ledFlashOff();
bool initCamera();
bool deInit();
camera_fb_t* capture();
void returnFb(camera_fb_t* fb);