#pragma once

#include <Arduino.h>

void rainInit();
void rainRenderFrame();

uint8_t rainGetDropCount();
uint8_t rainGetFallStepFrames();
uint8_t rainGetTailLength();
uint8_t rainGetRainbowStep();
uint8_t rainGetRespawnDelay();
bool rainGetSwapRG();

void rainSetDropCount(uint8_t v);
void rainSetFallStepFrames(uint8_t v);
void rainSetTailLength(uint8_t v);
void rainSetRainbowStep(uint8_t v);
void rainSetRespawnDelay(uint8_t v);
void rainToggleSwapRG();
