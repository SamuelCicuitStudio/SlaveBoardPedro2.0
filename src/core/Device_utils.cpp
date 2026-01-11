#include <Device.hpp>
#include <Config.hpp>
#include <ConfigNvs.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>
#include <SwitchManager.hpp>
#include <Utils.hpp>
#include <esp_system.h>
#include <stdio.h>

// =========================
// Small utilities
// =========================
uint32_t Device::ms_(){ return millis(); }

// Persisted / manager states
bool Device::isConfigured_() const { return CONF && CONF->GetBool(DEVICE_CONFIGURED, false); }
bool Device::isArmed_() const      { return CONF && CONF->GetBool(ARMED_STATE, false); }
bool Device::isMotionEnabled_() const {
  if (configModeActive_) return true;
  return CONF && CONF->GetBool(MOTION_TRIG_ALARM, false);
}
bool Device::isLocked_() const     { return CONF && CONF->GetBool(LOCK_STATE, true); }
bool Device::isDoorOpen_() const   { return Sw && Sw->isDoorOpen(); }
bool Device::isMotorMoving_() const{ return motorDriver && motorDriver->isMovingOrSettling(MOTOR_SETTLE_MS); }

void Device::printMACIfUserButton_() const {
  // Debug helper: print MAC only when pressing USER_BUTTON_PIN.
  if (Sw) {
    return;
  }
  if (!digitalRead(USER_BUTTON_PIN)) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#       Slave MAC Address:     " + String(macStr) + "          #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();
  }
}
