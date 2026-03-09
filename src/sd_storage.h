#pragma once

#include <Arduino.h>

struct SdAnimationFileInfo {
  char path[64];
  uint32_t sizeBytes;
  uint32_t frameCount;
  bool frameAligned;
  bool timedFrames;
  uint16_t frameBytes;
};

void sdStorageInit();
bool sdStorageIsReady();
bool sdStorageHasAnimations();

void sdStoragePrintTree(Print &out);
bool sdStorageReadTextFile(const char *path, char *out, size_t outSize, size_t *bytesRead = nullptr);
bool sdStorageWriteTextFile(const char *path, const char *text, bool append);
bool sdStorageReadWriteSelfTest(Print &out);

bool sdStorageLoadWifiSettings();
bool sdStoragePushWifiSettingsToEsp();
bool sdStorageLoadBridgeSettings();
bool sdStoragePushBridgeSettingsToEsp();

bool sdStorageScanAnimations();
void sdStoragePrintAnimations(Print &out);
void sdStorageResetAnimationPlayback();
bool sdStorageRenderNextAnimationFrame(uint16_t frameStepMs, bool transitionEnabled,
                                       uint16_t transitionMs, uint16_t timedSpeedPct);
bool sdStoragePlaybackIsTimed();
bool sdStorageSelectRelativeAnimation(int8_t delta);
void sdStorageSetPlaybackLogging(bool enabled);
bool sdStorageGetPlaybackLogging();
uint16_t sdStorageGetAnimationCount();
const SdAnimationFileInfo *sdStorageGetAnimationInfo(uint16_t index);
