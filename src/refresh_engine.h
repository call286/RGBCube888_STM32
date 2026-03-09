#pragma once

#include <Arduino.h>
#include "cube_config.h"

static const uint8_t REFRESH_PWM_STEPS = 8u;

struct FrameBuffer {
  uint8_t channels[CubeConfig::LAYERS][192];
  uint8_t chainA[REFRESH_PWM_STEPS][CubeConfig::LAYERS][CubeConfig::BYTES_PER_CHAIN];
  uint8_t chainB[REFRESH_PWM_STEPS][CubeConfig::LAYERS][CubeConfig::BYTES_PER_CHAIN];
};

void refreshInitPins();
void refreshStart(uint16_t frameHz);

void clearBack();
void fillBackAll(bool on);
void setChanLevelInBack(uint8_t z, uint16_t ch, uint8_t level);
void setChanInBack(uint8_t z, uint16_t ch, bool on);
void commitBackToFront();
void initFrameBuffers();
