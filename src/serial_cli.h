#pragma once

#include <Arduino.h>

#include "app_types.h"

void serialCliInit();
void serialCliHandle();
void serialCliHandleStream(Stream &io, bool echoInput);
void serialCliProcessChar(char c, Print &out, bool echoInput);

RenderMode serialCliGetRenderMode();
void serialCliSetRenderMode(RenderMode mode);
uint16_t serialCliGetAnimStepMs();
uint16_t serialCliGetSdAnimFrameMs();
uint16_t serialCliAdjustSdAnimFrameMs(int16_t deltaMs);
uint16_t serialCliGetSdAnimTimedSpeedPct();
uint16_t serialCliAdjustSdAnimTimedSpeedPct(int16_t deltaPct);
bool serialCliGetSdAnimTransitionEnabled();
uint16_t serialCliGetSdAnimTransitionMs();
uint16_t serialCliGetSdAnimRenderTickMs();
bool serialCliIsDisplayEnabled();
void serialCliSetDisplayEnabled(bool enabled);
