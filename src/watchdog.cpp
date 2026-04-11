#include "watchdog.h"

#include <IWatchdog.h>

namespace {

static bool g_watchdogEnabled = false;
static const char *g_resetCause = "unknown";

} // namespace

void watchdogCaptureResetCause() {
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) {
    g_resetCause = "iwdg";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != RESET) {
    g_resetCause = "wwdg";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST) != RESET) {
    g_resetCause = "software";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST) != RESET) {
    g_resetCause = "pin";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != RESET) {
    g_resetCause = "brownout";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST) != RESET) {
    g_resetCause = "power";
  } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST) != RESET) {
    g_resetCause = "low-power";
  } else {
    g_resetCause = "unknown";
  }

  __HAL_RCC_CLEAR_RESET_FLAGS();
}

void watchdogInit(uint32_t timeoutMs) {
  if (g_watchdogEnabled) {
    return;
  }

  if (timeoutMs < 100u) {
    timeoutMs = 100u;
  }

  IWatchdog.begin(timeoutMs * 1000u);
  g_watchdogEnabled = true;
}

void watchdogKick() {
  if (!g_watchdogEnabled) {
    return;
  }
  IWatchdog.reload();
}

bool watchdogIsEnabled() { return g_watchdogEnabled; }

const char *watchdogGetResetCause() { return g_resetCause; }
