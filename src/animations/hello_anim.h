#pragma once

#include <Arduino.h>

void helloInit();
void helloRenderFrame();

uint8_t helloGetSpinStep();
uint8_t helloGetFlyFrames();
uint8_t helloGetHoldFrames();
uint8_t helloGetGapFrames();
bool helloGetSwapRG();

void helloSetSpinStep(uint8_t v);
void helloSetFlyFrames(uint8_t v);
void helloSetHoldFrames(uint8_t v);
void helloSetGapFrames(uint8_t v);
void helloToggleSwapRG();
