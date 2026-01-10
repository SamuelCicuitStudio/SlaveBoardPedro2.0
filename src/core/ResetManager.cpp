#include <ResetManager.hpp>
#include <ConfigNvs.hpp>
#include <Device.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


namespace {
Device* g_device = nullptr;
}

void ResetManager::Init(Device* dev) {
  g_device = dev;
}

void ResetManager::RequestReset(ResetKind kind, const char* reason) {
  const bool factoryReset = (kind == ResetKind::Factory);

  // Persist factory-reset intent early so it survives unexpected crashes.
  if (factoryReset && CONF) {
    CONF->PutBool(RESET_FLAG, true);
  }

  if (g_device) {
    g_device->requestReset(factoryReset, reason);
    return;
  }

  // Fallback: no device registered; restart immediately.
  DBG_PRINTLN("[Reset] No device registered, performing immediate restart.");
  CONF->simulatePowerDown();
  while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
