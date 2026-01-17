#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <SwitchManager.hpp>
#include <Utils.hpp>
#include <string.h>

// =============================================================
//  Helpers
// =============================================================
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
    DBG_PRINTF("[ESPNOW][bundle] Battery=%u%%\n", (unsigned)pct);
    SendAck(EVT_BATTERY_PREFIX, &pct, 1, true);
  } else {
    DBG_PRINTLN("[ESPNOW][bundle] Power=null -> skip BAT");
  }

  // No STATE here; master can request via STQ if needed
}

void EspNowManager::heartbeatTick_() {
  // intentionally empty (HB is request/response only)
}

uint8_t EspNowManager::getCapBits_() {
  if (IS_SLAVE_ALARM) {
    return 0x06; // Alarm role: shock + reed only
  }
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
  const uint8_t open = 1;
  SendAck(EVT_REED, &open, 1, false);
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
  AckStatePayload payload{};

  const bool cfg   = isConfigured_();
  const bool armed = CONF ? CONF->GetBool(ARMED_STATE, false) : false;
  const bool motionEnabled = CONF ? CONF->GetBool(MOTION_TRIG_ALARM, false) : false;
  payload.cfg = cfg ? 1 : 0;
  payload.armed = armed ? 1 : 0;
  payload.motion = motionEnabled ? 1 : 0;
  payload.role = IS_SLAVE_ALARM ? 1 : 0;

  const bool lock = IS_SLAVE_ALARM ? false
                                   : (CONF ? CONF->GetBool(LOCK_STATE, true) : true);
  const bool hasReed = IS_SLAVE_ALARM ? true
                                      : (CONF ? CONF->GetBool(HAS_REED_SWITCH_KEY,
                                                              HAS_REED_SWITCH_DEFAULT)
                                              : false);
  const bool door = hasReed && sw ? sw->isDoorOpen() : false;
  const bool motorMoving =
      (!IS_SLAVE_ALARM && motor &&
       (motor->getLockTaskHandle() != nullptr || motor->getUnlockTaskHandle() != nullptr));

  payload.lock = lock ? 1 : 0;
  payload.door = door ? 1 : 0;
  payload.motor = motorMoving ? 1 : 0;

  const int batt  = Power ? (int)Power->getBatteryPercentage() : -1;
  const int pmode = Power ? (int)Power->getPowerMode() : 0;
  int band = 0;
  if (Power) {
    const bool critical = (Power->getPowerMode() == CRITICAL_POWER_MODE);
    if (critical) band = 2;
    else if (batt >= 0 && batt < LOW_BATT_TRHESHOLD) band = 1;
  }
  payload.batt = (batt >= 0 && batt <= 100) ? static_cast<uint8_t>(batt) : 0xFF;
  payload.pmode = static_cast<uint8_t>(pmode);
  payload.band = static_cast<uint8_t>(band);
  payload.breach = breach ? 1 : 0;

  payload.seq_le = ++seq_;
  payload.up_ms_le = millis();

  if (reason && *reason) {
    size_t n = strnlen(reason, NOW_STATE_REASON_MAX);
    payload.reason_len = static_cast<uint8_t>(n);
    if (n > 0) {
      memcpy(payload.reason, reason, n);
    }
  }

  DBG_PRINTLN("[ESPNOW][STATE] Send ACK_STATE");
  SendAck(ACK_STATE,
          reinterpret_cast<const uint8_t*>(&payload),
          sizeof(payload),
          true);
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
  struct HeartbeatPayload {
    uint32_t seq_le;
    uint32_t up_ms_le;
  } payload{};
  payload.seq_le = ++seq_;
  payload.up_ms_le = now;
  DBG_PRINTLN("[ESPNOW][HB] Send ACK_HEARTBEAT");
  SendAck(ACK_HEARTBEAT,
          reinterpret_cast<const uint8_t*>(&payload),
          sizeof(payload),
          true);
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
