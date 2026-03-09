#pragma once

#include <Arduino.h>

struct IrFrame {
  uint32_t raw;
  bool isRepeat;
};

void irReceiverInit();
void irReceiverHandle(bool printFrames);
bool irReceiverPopFrame(IrFrame &out);
void irReceiverFlushFrames();
