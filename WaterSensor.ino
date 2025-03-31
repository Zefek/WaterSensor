#include <EspDrv.h>
#include <MQTTClient.h>
#include <SoftwareSerial.h>
#include "config.h"

void MQTTMessageReceive(char* topic, uint8_t* payload, uint16_t length) { }
MQTTConnectData mqttConnectData = { MQTTHost, 1883, "WaterMeter", MQTTUsername, MQTTPassword, "", 0, false, "", false, 0x0F }; 

SoftwareSerial serial(4, 5);
EspDrv espDrv(&serial);
MQTTClient mqttClient(&espDrv, MQTTMessageReceive);
char data[32];
unsigned long lastSendToMQTT = 0;

unsigned long lastRissing = 0;
unsigned long count = 0; 

void Add()
{
  if(millis() - lastRissing > 84)
  {
    count++;
    lastRissing = millis();
  }
}

bool Connect()
{
  int wifiStatus = espDrv.GetConnectionStatus();
  bool wifiConnected = wifiStatus == WL_CONNECTED;
  if(wifiStatus == WL_DISCONNECTED || wifiStatus == WL_IDLE_STATUS)
  {
    wifiConnected = espDrv.Connect(WifiSSID, WifiPassword);
  }
  if(wifiConnected)
  {

    bool isConnected = mqttClient.IsConnected();
    if(!isConnected)
    {
      return mqttClient.Connect(mqttConnectData);
    }
    else
    {
      return true;
    }
  }
  return false;
}

void setup() 
{
  pinMode(2, INPUT);
  Serial.begin(57600);
  serial.begin(57600);
  espDrv.Init(16);
  espDrv.Connect(WifiSSID, WifiPassword);
  attachInterrupt(digitalPinToInterrupt(2), Add, RISING);
}

void loop() 
{
  mqttClient.Loop();
  unsigned long currentMillis = millis();
  if(currentMillis - lastSendToMQTT >= 300000)
  {    
    detachInterrupt(digitalPinToInterrupt(2));
    Serial.print("Variable 1:");
    Serial.println(count);
    if(Connect())
    {
      mqttClient.Publish(WATERCONSUMPTION, 0);
      mqttClient.Publish(WATERCONSUMPTION, count);
      mqttClient.Disconnect();
      count = 0;
    }
    lastSendToMQTT = currentMillis;
    attachInterrupt(digitalPinToInterrupt(2), Add, RISING);
  }
}