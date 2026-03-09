#pragma once

#include <Arduino.h>

void waveInit();
void waveRenderFrame();

uint8_t waveGetSpeed();
uint8_t waveGetScale();
uint8_t waveGetThreshold();
bool waveGetSwapRG();

void waveSetSpeed(uint8_t v);
void waveSetScale(uint8_t v);
void waveSetThreshold(uint8_t v);
void waveToggleSwapRG();
