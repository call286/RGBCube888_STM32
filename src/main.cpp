#include <Arduino.h>
#include <string.h>

#include "cube_config.h"
#include "esp_at_bridge.h"
#include "animations/cube_puzzle_anim.h"
#include "animations/hello_anim.h"
#include "animations/pixel_set_anim.h"
#include "animations/raw_test_anim.h"
#include "ir_receiver.h"
#include "animations/rain_anim.h"
#include "refresh_engine.h"
#include "serial_cli.h"
#include "sd_storage.h"
#include "stream_3d8.h"
#include "animations/wave_anim.h"

static uint32_t lastFrameMs = 0;
static bool sdAnimPaused = false;

static const char *mapIrRawToButton(uint32_t raw) {
  switch (raw) {
  case 0xBA45FF00UL:
    return "POWER";
  case 0xB946FF00UL:
    return "MENU";
  case 0xB847FF00UL:
    return "MUTE";
  case 0xBB44FF00UL:
    return "MODE";
  case 0xBF40FF00UL:
    return "PLUS";
  case 0xBC43FF00UL:
    return "BACK";
  case 0xF807FF00UL:
    return "PREV";
  case 0xEA15FF00UL:
    return "PLAY_PAUSE";
  case 0xF609FF00UL:
    return "NEXT";
  case 0xE916FF00UL:
    return "0";
  case 0xE619FF00UL:
    return "MINUS";
  case 0xF20DFF00UL:
    return "OK";
  case 0xF30CFF00UL:
    return "1";
  case 0xE718FF00UL:
    return "2";
  case 0xA15EFF00UL:
    return "3";
  case 0xF708FF00UL:
    return "4";
  case 0xE31CFF00UL:
    return "5";
  case 0xA55AFF00UL:
    return "6";
  case 0xBD42FF00UL:
    return "7";
  case 0xAD52FF00UL:
    return "8";
  case 0xB54AFF00UL:
    return "9";
  default:
    return nullptr;
  }
}

static void handleIrCommandsStub(RenderMode mode) {
  IrFrame frame;
  while (irReceiverPopFrame(frame)) {
    if (frame.isRepeat) {
      continue;
    }

    const char *button = mapIrRawToButton(frame.raw);
    if (button != nullptr) {
      Serial.print(F("[IR-CMD] "));
      Serial.println(button);

      if (mode == MODE_SD_ANIM) {
        if (strcmp(button, "PLAY_PAUSE") == 0) {
          sdAnimPaused = !sdAnimPaused;
          Serial.print(F("[IR-CMD] sdAnim="));
          Serial.println(sdAnimPaused ? F("PAUSE") : F("PLAY"));
        } else if (strcmp(button, "NEXT") == 0) {
          bool ok = sdStorageSelectRelativeAnimation(+1);
          Serial.println(ok ? F("[IR-CMD] sdAnim next") : F("[IR-CMD] sdAnim next FAIL"));
        } else if (strcmp(button, "PREV") == 0) {
          bool ok = sdStorageSelectRelativeAnimation(-1);
          Serial.println(ok ? F("[IR-CMD] sdAnim prev") : F("[IR-CMD] sdAnim prev FAIL"));
        } else if (strcmp(button, "PLUS") == 0) {
          if (sdStoragePlaybackIsTimed()) {
            uint16_t pct = serialCliAdjustSdAnimTimedSpeedPct(-10);
            Serial.print(F("[IR-CMD] sdAnimTimedSpeedPct="));
            Serial.print(pct);
            Serial.println('%');
          } else {
            uint16_t ms = serialCliAdjustSdAnimFrameMs(-10);
            Serial.print(F("[IR-CMD] sdAnimFrameMs="));
            Serial.println(ms);
          }
        } else if (strcmp(button, "MINUS") == 0) {
          if (sdStoragePlaybackIsTimed()) {
            uint16_t pct = serialCliAdjustSdAnimTimedSpeedPct(+10);
            Serial.print(F("[IR-CMD] sdAnimTimedSpeedPct="));
            Serial.print(pct);
            Serial.println('%');
          } else {
            uint16_t ms = serialCliAdjustSdAnimFrameMs(+10);
            Serial.print(F("[IR-CMD] sdAnimFrameMs="));
            Serial.println(ms);
          }
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  refreshInitPins();
  waveInit();
  rainInit();
  helloInit();
  cubePuzzleInit();
  pixelSetInit();
  rawTestInit();
  initFrameBuffers();
  refreshStart(CubeConfig::FRAME_HZ);

  serialCliInit();
  irReceiverInit();
  espAtBridgeInit();
  stream3d8Init();
  sdStorageInit();
  if (sdStorageHasAnimations()) {
    serialCliSetRenderMode(MODE_SD_ANIM);
    Serial.println(F("[SD] default mode set to SD animations"));
  }
  lastFrameMs = millis();
}

void loop() {
  serialCliHandle();
  espAtBridgeHandle();

  RenderMode mode = serialCliGetRenderMode();
  bool displayEnabled = serialCliIsDisplayEnabled();
  static RenderMode lastMode = MODE_WAVE;
  if (mode != lastMode) {
    if (mode == MODE_SD_ANIM) {
      sdAnimPaused = false; // Default SD mode behavior: play.
    }
    lastMode = mode;
  }
  irReceiverHandle(mode == MODE_IR_DEBUG);
  if (mode == MODE_IR_DEBUG) {
    // Keep debug-only behavior strict: never execute queued commands.
    irReceiverFlushFrames();
  } else {
    handleIrCommandsStub(mode);
  }

  uint32_t now = millis();
  uint16_t frameStepMs = serialCliGetAnimStepMs();
  if (mode == MODE_SD_ANIM) {
    frameStepMs = serialCliGetSdAnimRenderTickMs();
  } else if (mode == MODE_RX_STREAM) {
    frameStepMs = stream3d8GetRenderTickMs();
  }
  static bool lastDisplayEnabled = true;
  if (!displayEnabled && lastDisplayEnabled) {
    fillBackAll(false);
    commitBackToFront();
    lastFrameMs = now;
  }
  lastDisplayEnabled = displayEnabled;
  if ((uint32_t)(now - lastFrameMs) >= frameStepMs) {
    lastFrameMs = now;

    if (!displayEnabled) {
      fillBackAll(false);
    } else if (mode == MODE_WAVE) {
      waveRenderFrame();
    } else if (mode == MODE_ALL_ON) {
      fillBackAll(true);
    } else if (mode == MODE_RAIN) {
      rainRenderFrame();
    } else if (mode == MODE_HELLO) {
      helloRenderFrame();
    } else if (mode == MODE_CUBE_PUZZLE) {
      cubePuzzleRenderFrame();
    } else if (mode == MODE_PIXEL_SET) {
      pixelSetRenderFrame();
    } else if (mode == MODE_RAW_TEST) {
      rawTestRenderFrame();
    } else if (mode == MODE_SD_ANIM) {
      if (!sdAnimPaused) {
        if (!sdStorageRenderNextAnimationFrame(serialCliGetSdAnimFrameMs(),
                                               serialCliGetSdAnimTransitionEnabled(),
                                               serialCliGetSdAnimTransitionMs(),
                                               serialCliGetSdAnimTimedSpeedPct())) {
          fillBackAll(false);
        }
      }
    } else if (mode == MODE_RX_STREAM) {
      if (!stream3d8RenderLatestFrame()) {
        fillBackAll(false);
      }
    } else if (mode == MODE_IR_DEBUG) {
      fillBackAll(false);
    } else {
      fillBackAll(false);
    }

    commitBackToFront();
  }
}
