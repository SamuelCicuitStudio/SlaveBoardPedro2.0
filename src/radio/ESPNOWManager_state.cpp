#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <SwitchManager.hpp>
#include <Utils.hpp>

// =============================================================
//  Helpers
// =============================================================
String EspNowManager::extractCmdCode_(const String& msg) {
  int end = msg.length();
  int space = msg.indexOf(' ');
  if (space >= 0 && space < end) end = space;
  int colon = msg.indexOf(':');
  if (colon >= 0 && colon < end) end = colon;
  if (end < 0) end = msg.length();
  return msg.substring(0, end);
}

bool EspNowManager::isConfigured_() const {
  bool cfg = (CONF && CONF->GetBool(DEVICE_CONFIGURED, false));
  return cfg;
}

void EspNowManager::sendConfiguredBundle_(const char* reason) {
  DBG_PRINTLN(String("[ESPNOW][bundle] Send CONFIGURED+BATT reason=") + (reason ? reason : "N/A"));

  // Announce configured
  SendAck(ACK_CONFIGURED, true);

  // Battery report once at start
  if (Power) {
    uint8_t pct = (uint8_t)Power->getBatteryPercentage();
    String batteryStatus = String(EVT_BATTERY_PREFIX) + ":" + String(pct);
    DBG_PRINTF("[ESPNOW][bundle] Battery=%u%%\n", (unsigned)pct);
    SendAck(batteryStatus, true);
  } else {
    DBG_PRINTLN("[ESPNOW][bundle] Power=null -> skip BAT");
  }

  // No STATE here; master can request via STQ if needed
}

void EspNowManager::heartbeatTick_() {
  // intentionally empty (HB is request/response only)
}

uint8_t EspNowManager::getCapBits_() {
  if (!capBitsShadowValid_) {
    uint8_t bits = 0;
    if (CONF) {
      bits |= CONF->GetBool(HAS_OPEN_SWITCH_KEY,   HAS_OPEN_SWITCH_DEFAULT)   ? 0x01 : 0;
      bits |= CONF->GetBool(HAS_SHOCK_SENSOR_KEY,  HAS_SHOCK_SENSOR_DEFAULT)  ? 0x02 : 0;
      bits |= CONF->GetBool(HAS_REED_SWITCH_KEY,   HAS_REED_SWITCH_DEFAULT)   ? 0x04 : 0;
      bits |= CONF->GetBool(HAS_FINGERPRINT_KEY,   HAS_FINGERPRINT_DEFAULT)   ? 0x08 : 0;
    }
    capBitsShadow_ = bits;
    capBitsShadowValid_ = true;
  }
  return capBitsShadow_;
}

void EspNowManager::setCapBitsShadow_(uint8_t bits) {
  capBitsShadow_ = bits;
  capBitsShadowValid_ = true;
}

// =============================================================
//  Public API (State/Heartbeat)
// =============================================================
void EspNowManager::RequestOff() {
  if (!isConfigured_()) return;
  if (!Power) {
    DBG_PRINTLN("[ESPNOW][RequestOff] No Power manager; sending EVT_HGBT");
    SendAck(EVT_HGBT, false);
    return;
  }
  if (Power->batteryPercentage < 15) {
    DBG_PRINTF("[ESPNOW][RequestOff] Low battery %u%%; sending EVT_LWBT\n", (unsigned)Power->batteryPercentage);
    SendAck(EVT_LWBT, false);
  } else {
    DBG_PRINTF("[ESPNOW][RequestOff] Battery %u%%; sending EVT_HGBT\n", (unsigned)Power->batteryPercentage);
    SendAck(EVT_HGBT, false);
  }
}

void EspNowManager::RequesAlarm() {
  if (!isConfigured_()) return;
  DBG_PRINTLN("[ESPNOW][RequesAlarm] EVT_REED");
  SendAck(String(EVT_REED) + ":1", false);
}

void EspNowManager::RequestUnlock() {
  if (!isConfigured_()) return;
  DBG_PRINTLN("[ESPNOW][RequestUnlock] EVT_GENERIC");
  SendAck(EVT_GENERIC, false);
}

void EspNowManager::SendMotionTrigg() {
  if (!isConfigured_()) return;
  DBG_PRINTLN("[ESPNOW][SendMotionTrigg] EVT_MTRTTRG");
  SendAck(EVT_MTRTTRG, false);
}

void EspNowManager::sendState(const char* reason) {
  if (!isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][STATE] Not configured -> skip");
    return;
  }
  String line = String(ACK_STATE);

  const bool cfg   = isConfigured_();
  const bool armed = CONF ? CONF->GetBool(ARMED_STATE, false) : false;
  const bool motionEnabled = CONF ? CONF->GetBool(MOTION_TRIG_ALARM, false) : false;
  line += " cfg="   + String((int)cfg);
  line += " armed=" + String((int)armed);
  line += " motion=" + String((int)motionEnabled);
  line += " role=" + String(IS_SLAVE_ALARM ? 1 : 0);

  const bool lock = CONF ? CONF->GetBool(LOCK_STATE, true) : true;
  const bool door = sw ? sw->isDoorOpen() : false;
  const bool motorMoving =
      motor && (motor->getLockTaskHandle() != nullptr || motor->getUnlockTaskHandle() != nullptr);

  line += " lock="  + String((int)lock);
  line += " door="  + String((int)door);
  line += " motor=" + String((int)motorMoving);

  const int batt  = Power ? (int)Power->getBatteryPercentage() : -1;
  const int pmode = Power ? (int)Power->getPowerMode() : 0;
  int band = 0;
  if (Power) {
    const bool critical = (Power->getPowerMode() == CRITICAL_POWER_MODE);
    if (critical) band = 2;
    else if (batt >= 0 && batt < LOW_BATT_TRHESHOLD) band = 1;
  }
  if (batt >= 0) line += " batt=" + String(batt);
  line += " pmode=" + String(pmode);
  line += " band=" + String(band);
  line += " breach=" + String((int)breach);

  const uint32_t up = millis();
  line += " seq=" + String(++seq_);
  line += " up="  + String(up);

  if (reason && *reason) {
    line += " reason=";
    line += reason;
  }

  DBG_PRINTLN(String("[ESPNOW][STATE] Send: '") + line + "'");
  SendAck(line, true);
}

void EspNowManager::sendHeartbeat(bool force) {
  if (!isConfigured_()) {
    DBG_PRINTLN("[ESPNOW][HB] Not configured -> skip");
    return;
  }
  if (!force) {
    DBG_PRINTLN("[ESPNOW][HB] force=false (master-driven only) -> skip");
    return;
  }
  const uint32_t now = millis();
  lastHbMs_ = now;
  String hb = String(ACK_HEARTBEAT) + " seq=" + String(++seq_) + " up=" + String(now);
  DBG_PRINTLN(String("[ESPNOW][HB] Send: '") + hb + "'");
  SendAck(hb, true);
}

// =============================================================
//  Presence / PING
// =============================================================
bool EspNowManager::pingMaster(uint8_t /*tries*/) {
  if (!isConfigured_()) {
    return false;
  }
  const uint32_t now = millis();
  return (now - lastHbMs_) < 60000UL;
}
