#pragma once
#include <Arduino.h>

#define DIAG_CORR_HEX_BUF      17   // 64bit ID = 16 hex znaků
#define DIAG_TRANSFER_HEX_BUF  64   // transfer blob (26 B) = 52 hex znaků

#ifndef DIAG_INTERVAL_MS
#define DIAG_INTERVAL_MS (5UL * 60UL * 1000UL)
#endif

uint64_t diagNextCorrelationId();
void diagRecordTransfer(uint64_t corrId, uint32_t imgSize, uint32_t bytesSent,
                        uint32_t durationMs, uint8_t tryCount, bool success,
                        int16_t httpCode, int8_t rssi);
bool diagPrevTransferHex(char* hexbuf, size_t buflen);
void diagCorrelationHex(uint64_t corrId, char* hexbuf, size_t buflen);

void diagCountWifiReconnect();
void diagCountCapture();
void diagCountSendFailure();
void diagCountTlsError();
void diagCountOtaFailure();
void diagCountCameraError();
void diagNoteLoopMs(uint32_t ms);

size_t diagBuildDeviceBlob(uint8_t* buf, size_t buflen);

size_t diagBuildConfigBlob(uint8_t* buf, size_t buflen);
bool diagConfigChanged();
void diagMarkConfigSent();
