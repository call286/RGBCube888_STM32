#pragma once

#include <Arduino.h>

enum RenderMode : uint8_t {
  MODE_WAVE = 0,
  MODE_ALL_ON = 1,
  MODE_ALL_OFF = 2,
  MODE_RAIN = 3,
  MODE_HELLO = 4,
  MODE_IR_DEBUG = 5,
  MODE_CUBE_PUZZLE = 6,
  MODE_PIXEL_SET = 7,
  MODE_RAW_TEST = 8,
  MODE_SD_ANIM = 9,
  MODE_RX_STREAM = 10
};
