#include <Device.hpp>
#include <ConfigNvs.hpp>
#include <ESPNOWManager.hpp>
#include <FingerprintScanner.hpp>
#include <NVSManager.hpp>
#include <RGBLed.hpp>
#include <ShockSensor.hpp>
#include <Utils.hpp>

// =========================
// Capability snapshot
// =========================
void Device::refreshCapabilities_() {
  if (!CONF) return;
  hasOpenSwitch_  = CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT);
  hasShock_       = CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT);
  hasReed_        = CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT);
  hasFingerprint_ = CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT);

  // Alarm role is a strict subset: reed + shock only.
  if (isAlarmRole_) {
    hasOpenSwitch_    = false;
    hasFingerprint_   = false;
    hasShock_         = true;
    hasReed_          = true;
  }

  updateShockSensor_();
  if (Fing) {
    Fing->setSupported(hasFingerprint_ && !isAlarmRole_);
    Fing->setEnabled(effectiveBand_ == 0);
  }
}

void Device::updateShockSensor_() {
  if (!shockSensor) return;
  if (!hasShock_) {
    shockSensor->disable();
    return;
  }
  ShockConfig cfg = ShockSensor::loadConfig(CONF);
  shockSensor->applyConfig(cfg);
}

// =========================
// Config mode gate
// =========================
void Device::updateConfigMode_() {
  const bool newMode = (Now && Now->isConfigMode());
  if (newMode == configModeActive_) return;

  configModeActive_ = newMode;
  DBG_PRINTLN(String("[Device] Config mode ") + (configModeActive_ ? "ENABLED" : "DISABLED"));

  // LED background hints
  if (RGB) {
    if (configModeActive_) {
      RGB->setDeviceState(DeviceState::READY_OFFLINE);
    } else if (!isConfigured_()) {
      RGB->setDeviceState(DeviceState::PAIRING);
    } else {
      RGB->setDeviceState(DeviceState::READY_ONLINE);
    }
  }

  // Reset transient flows when entering config mode
  if (configModeActive_) {
    awaitingDoorCycle_ = false;
  }
}
