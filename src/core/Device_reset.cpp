#include <Device.hpp>
#include <ConfigNvs.hpp>
#include <ESPNOWManager.hpp>
#include <FingerprintScanner.hpp>
#include <Logger.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>
#include <RGBLed.hpp>
#include <SleepTimer.hpp>
#include <Utils.hpp>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =========================
// Reset coordination
// =========================
void Device::requestReset(bool factoryReset, const char* reason) {
  if (resetInProgress_) return;
  resetRequested_        = true;
  factoryResetRequested_ = factoryReset;
  resetRequestMs_        = millis();
  resetReason_           = reason ? String(reason) : String("unspecified");

  if (factoryReset && CONF) {
    CONF->PutBool(RESET_FLAG, true);
  }
}

void Device::processResetIfNeeded_() {
  if (!resetRequested_) return;
  performSafeReset_();
}

void Device::performSafeReset_() {
  if (resetInProgress_) return;
  resetInProgress_ = true;
  resetRequested_  = false;

  DBG_PRINTLN("[Device] Reset requested -> orderly shutdown");
  if (resetReason_.length()) {
    DBG_PRINTLN(String("[Device] Reason: ") + resetReason_);
  }
  if (factoryResetRequested_) {
    DBG_PRINTLN("[Device] Factory reset flag set");
  }
  if (RGB) {
  }

  // Halt periodic services to avoid new work while we unwind.
  if (sleepTimer) sleepTimer->reset();

  stopFingerprint_();
  stopMotor_();
  stopTransport_();
  stopRadio_();

  if (factoryResetRequested_ && LOGG) {
    LOGG->deleteLogFile();
  }

  // Small grace period to let tasks tear down.
  vTaskDelay(pdMS_TO_TICKS(200));

  DBG_PRINTLN("[Device] Restarting now...");
  if (!factoryResetRequested_ && CONF) {
    DBG_PRINTLN("[Device] Using CONF->simulatePowerDown() for safe reboot");
    CONF->simulatePowerDown();
  } else {
    CONF->simulatePowerDown();;
  }
  while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void Device::stopRadio_() {
  if (Now) {
    Now->deinit();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void Device::stopMotor_() {
  if (!motorDriver) return;
  motorDriver->stop();
  motorDriver->shutdown();
}

void Device::stopFingerprint_() {
  if (!Fing) return;
  Fing->shutdown();
}

void Device::stopTransport_() {
  if (!Transport) return;
  // TransportManager currently has no explicit stop; placeholder for future.
}
