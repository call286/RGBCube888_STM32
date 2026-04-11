#pragma once

#include <Arduino.h>

void watchdogCaptureResetCause();
void watchdogInit(uint32_t timeoutMs);
void watchdogKick();
bool watchdogIsEnabled();
const char *watchdogGetResetCause();
