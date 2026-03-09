#pragma once

#include <Arduino.h>

struct Stream3d8FeedResult {
  uint16_t acceptedChars;
  uint16_t ignoredChars;
  uint16_t invalidChars;
  uint16_t completedFrames;
  uint16_t partialChars;
};

struct Stream3d8FinalizeResult {
  bool committed;
  uint16_t receivedChars;
  uint16_t invalidChars;
  bool overflowed;
};

void stream3d8Init();
void stream3d8Reset();
void stream3d8SetEnabled(bool enabled);
bool stream3d8IsEnabled();
void stream3d8SetLogging(bool enabled);
bool stream3d8GetLogging();
void stream3d8BeginFrame();
void stream3d8ClearPartial();
bool stream3d8FeedChunk(const char *chunk, Stream3d8FeedResult &out);
bool stream3d8FinalizeFrame(Stream3d8FinalizeResult &out);
bool stream3d8CommitRgbFrame(const uint8_t *rgb, uint16_t rgbLen, uint16_t durationMs);
bool stream3d8RenderLatestFrame();
bool stream3d8HasFrame();
uint32_t stream3d8GetFrameCount();
uint16_t stream3d8GetPartialChars();
uint16_t stream3d8GetRenderTickMs();
